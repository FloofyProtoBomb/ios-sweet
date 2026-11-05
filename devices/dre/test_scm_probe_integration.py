#!/usr/bin/env python3
"""Exact linked shim regression with INTERCEPTED synthetic SMC instructions.

Never executes an SMC on the host or accesses a device. Private frozen kernel,
image and FDT inputs remain read-only. No identifiers/entropy are printed.
"""
import argparse
import ast
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent


def parser_checks():
    """Run the actual parser/profile validation prefix, without any builder IO."""
    path = HERE / 'build.py'
    tree = ast.parse(path.read_text())
    fn = next(x for x in tree.body if isinstance(x, ast.FunctionDef) and x.name == 'main')
    ends = [i for i, x in enumerate(fn.body) if isinstance(x, ast.Assign) and
            any(isinstance(t, ast.Name) and t.id == 'out' for t in x.targets)]
    assert len(ends) == 1, 'actual build preflight boundary changed'
    fn.body = fn.body[:ends[0]] + [ast.Return(ast.Name('args', ast.Load()))]
    prefix = ast.fix_missing_locations(ast.Module(body=[fn], type_ignores=[]))
    scope = {'argparse': argparse, 'Path': Path, 'HERE': HERE}
    exec(compile(prefix, str(path), 'exec'), scope)

    def parse(args):
        before = sys.argv
        try:
            sys.argv = [str(path), *args]
            with contextlib.redirect_stderr(io.StringIO()):
                return scope['main']()
        finally:
            sys.argv = before
    for args in (['--scm-info-probe'], ['--scm-info-probe', '--xnu', 'unused'],
                 ['--scm-info-probe', '--board-tests', '--upstream-0027', 'unused']):
        try:
            parse(args)
        except SystemExit as error:
            assert error.code == 2
        else:
            raise AssertionError('invalid SCM selection accepted')
    assert parse(['--scm-info-probe', '--xnu', 'unused', '--board-tests']).scm_info_probe
    profile = parse(['--install-root', '--xnu', 'unused', '--product-adt', 'unused'])
    assert profile.board_tests and not profile.scm_info_probe
    return 5


def normalized_default(before, after):
    def normalize(directory):
        adt = (directory / 'dre.adt').read_bytes()
        image = bytearray((directory / 'dre-xnu-boot.img').read_bytes())
        marker = b'random-seed\0'.ljust(32, b'\0') + struct.pack('<I', 64)
        # mkpayload fills memory-map addresses in the ADT after serialization.
        # Locate the exact serialized property, not an unchanged whole tree.
        assert adt.count(marker) == 1 and image.count(marker) == 1
        start = image.index(marker) + len(marker)
        image[start:start + 64] = bytes(64)
        return image
    assert normalize(before) == normalize(after), 'default changed outside actual 64-byte entropy property'
    info = json.loads((after / 'build-info.json').read_text())
    assert info['scm_info_probe'] is False and info['scm_info_schema'] is None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('dtb', type=Path)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--deps', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--default', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--case', action='append', choices=('available','partial','unsupported',
                        'continuation','context_lost','clock_stalled'))
    args = parser.parse_args()
    if args.deps:
        sys.path.insert(0, str(args.deps.resolve()))
    import unicorn
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_INSN
    from unicorn.arm64_const import UC_ARM64_INS_MRS, UC_ARM64_REG_PC, UC_ARM64_REG_SP
    from unicorn import arm64_const
    regs = [getattr(arm64_const, 'UC_ARM64_REG_X' + str(i)) for i in range(8)]
    lr = arm64_const.UC_ARM64_REG_X30
    spec = importlib.util.spec_from_file_location('bundle', HERE / 'test_bundle.py')
    bundle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bundle)
    image, dtb = args.image.read_bytes(), args.dtb.read_bytes()
    kernel = image[4096:4096 + struct.unpack_from('<I', image, 8)[0]]
    syms = bundle.symbols(args.elf, kernel)
    start = syms['dre_a619_smc64_arm64'] - bundle.LOAD
    opcode = b'\x03\x00\x00\xd4'
    assert kernel[start:start + 256].count(opcode) == 1
    smc_pc = bundle.LOAD + kernel.index(opcode, start, start + 256)
    targets = [0x02000601, 0x02000201, 0x02000202, 0x02000205, 0x02000206, 0x02000207]
    later = {syms[n]: n for n in ('dre_cpu_topology', 'dre_identity_detect', 'dre_read32',
             'dre_write32', 'dre_storage_prepare', 'loader_boot_range_with_adt') if n in syms}
    created = []
    selected = 'available'

    class InfoCPU:
        def __init__(self, *a, **kw):
            # The Python Uc factory returns an architecture-specific object;
            # wrap it instead of subclassing its dynamic __new__ factory.
            self.cpu = Uc(*a, **kw)
            self.mode = selected
            self.smc_calls = []
            self.completed = set()
            self.later_calls = []
            self.target = None
            self.physical_ticks = 0
            self.cpu.hook_add(UC_HOOK_CODE, self.info, begin=smc_pc, end=smc_pc)
            for pc in later:
                self.cpu.hook_add(UC_HOOK_CODE, self.later, begin=pc, end=pc)
            created.append(self)

        def __getattr__(self, name):
            return getattr(self.cpu, name)

        def hook_add(self, kind, callback, *a, **kw):
            if kind == UC_HOOK_INSN and a and a[-1] == UC_ARM64_INS_MRS:
                original = callback

                def wrapped(cpu, register, cp, data):
                    key = cp.op0, cp.op1, cp.crn, cp.crm, cp.op2
                    if key == (3, 3, 4, 2, 1):  # actual DAIF raw read
                        cpu.reg_write(register, 0 if self.mode == 'context_lost' and self.smc_calls else 0x3c0)
                        return 1
                    if key == (3, 3, 14, 0, 1):  # probe uses CNTPCT, not CNTVCT
                        if self.mode != 'clock_stalled':
                            self.physical_ticks += 1000
                        cpu.reg_write(register, self.physical_ticks)
                        return 1
                    return original(cpu, register, cp, data)
                callback = wrapped
            return self.cpu.hook_add(kind, callback, *a, **kw)

        def info(self, cpu, pc, size, data):
            values = [cpu.reg_read(r) for r in regs]
            assert values[0] in (0xc2000601, 1) and values[1] == 1 and values[2] in targets
            assert not any(values[i] for i in (3, 4, 5, 7)), 'non-INFO request parameters'
            if values[0] == 1:
                assert values[2] == self.target and values[6] == 0xa6
            else:
                self.target = values[2]
                assert values[6] == 0
            self.smc_calls.append((values[0], values[2]))
            if self.mode == 'continuation':
                cpu.reg_write(regs[0], 1); cpu.reg_write(regs[1], 0xbad); cpu.reg_write(regs[6], 0xa6)
            else:
                available = self.mode != 'unsupported' and not (self.mode == 'partial' and targets.index(self.target) in (1, 3, 5))
                cpu.reg_write(regs[0], 0); cpu.reg_write(regs[1], int(available))
                self.completed.add(self.target)
            cpu.reg_write(UC_ARM64_REG_PC, pc + 4)  # Intercept: NEVER execute SMC.

        def later(self, cpu, pc, size, data):
            if self.mode in ('continuation', 'context_lost', 'clock_stalled'):
                raise AssertionError('fatal INFO result escaped to ' + later[pc])
            expected = 1 if self.mode == 'unsupported' else 6
            assert len(self.completed) == expected, 'later operation preceded complete INFO observation'
            self.later_calls.append(later[pc])

    report = {'schema': 1, 'synthetic_smc_only': True, 'actual_smc_executed': False,
              'unicorn': unicorn.__version__, 'cases': {}}
    parser_count = parser_checks()
    if bool(args.baseline) != bool(args.default):
        parser.error('--baseline and --default must be supplied together')
    if args.baseline:
        normalized_default(args.baseline, args.default)
        report['default_identical_except_existing_entropy'] = True
    old = bundle.Uc
    try:
        bundle.Uc = InfoCPU
        for selected, field in [('available', 'SCM_I=1/3f/0/0/6'),
                                ('partial', 'SCM_I=3/15/2a/0/6'),
                                ('unsupported', 'SCM_I=2/0/1/0/1')]:
            if args.case and selected not in args.case:
                continue
            try:
                fields = bundle.run(image, dtb, syms, 'ready', True, True, True, True)
            except Exception:
                if created:
                    failed = created[-1]
                    print('fixture failure PC/LR/SP:', hex(failed.reg_read(UC_ARM64_REG_PC)),
                          hex(failed.reg_read(lr)), hex(failed.reg_read(UC_ARM64_REG_SP)),
                          'x0/x1', hex(failed.reg_read(regs[0])), hex(failed.reg_read(regs[1])),
                          'INFO calls', failed.smc_calls, 'later', failed.later_calls[-5:])
                raise
            assert field in fields
            cpu = created[-1]
            assert len(cpu.smc_calls) == (1 if selected == 'unsupported' else 6)
            report['cases'][selected] = {'info_calls': len(cpu.smc_calls), 'field': field,
                                        'linked_read_only_ufs_model_passed': True}
    finally:
        bundle.Uc = old

    for selected, expected in [('continuation', 64), ('context_lost', 1), ('clock_stalled', 0)]:
        if args.case and selected not in args.case:
            continue
        cpu = InfoCPU(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.mem_map(bundle.LOAD, (len(kernel) + 4095) & -4096); cpu.mem_write(bundle.LOAD, kernel)
        tree, stop = 0x82000000, 0x71000000
        cpu.mem_map(tree, (len(dtb) + 4095) & -4096); cpu.mem_write(tree, dtb)
        cpu.mem_map(0x85200000, 0xc00000); cpu.mem_map(stop, 4096)
        cpu.mem_write(syms['con_fb'], struct.pack('<Q', 0x85200000))
        counter = 0

        def mrs(machine, register, cp, data):
            nonlocal counter
            key = cp.op0, cp.op1, cp.crn, cp.crm, cp.op2
            values = {(3, 0, 4, 2, 2): 4, (3, 0, 0, 7, 0): 0x101124,
                      (3, 3, 14, 0, 0): 19200000, (3, 0, 0, 0, 5): 0x80000000,
                      (3, 0, 1, 0, 0): 0}
            if key == (3, 3, 14, 0, 2):
                if selected != 'clock_stalled': counter += 1000
                machine.reg_write(register, counter); return 1
            if key in values:
                machine.reg_write(register, values[key]); return 1
            return 0
        cpu.hook_add(UC_HOOK_INSN, mrs, None, 1, 0, UC_ARM64_INS_MRS)
        cpu.reg_write(regs[0], tree); cpu.reg_write(UC_ARM64_REG_SP, syms['_stack_top'])
        cpu.reg_write(lr, stop)
        try:
            cpu.emu_start(syms['dre_handoff'], stop, count=250000000)
        except Exception:
            print('fatal fixture result:', selected, struct.unpack('<8I',
                  bytes(cpu.mem_read(syms['scm_result'], 32))),
                  'counter', counter, 'INFO calls', len(cpu.smc_calls))
            raise
        if cpu.reg_read(UC_ARM64_REG_PC) != stop:
            print('fatal model budget/PC:', selected, hex(cpu.reg_read(UC_ARM64_REG_PC)),
                  'SP/LR', hex(cpu.reg_read(UC_ARM64_REG_SP)), hex(cpu.reg_read(lr)),
                  'INFO calls', len(cpu.smc_calls), 'later', cpu.later_calls)
        assert cpu.reg_read(UC_ARM64_REG_PC) == stop and cpu.reg_read(regs[0])
        assert len(cpu.smc_calls) == expected and not cpu.later_calls
        observed = struct.unpack('<8I', bytes(cpu.mem_read(syms['scm_result'], 32)))
        assert observed[7] == 1 and (observed[6] == 1) == (expected != 0)
        report['cases'][selected] = {'info_calls': expected, 'boot_blocked_before_later_operations': True}
        print('PASS exact shim ' + selected + ': no later topology/device/storage/kernel operation', flush=True)
    report['parser_cases'] = parser_count
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS INFO integration; intercepted synthetic replies, no actual SMC or device access')


if __name__ == '__main__':
    main()
