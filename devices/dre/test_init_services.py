#!/usr/bin/env python3
"""Execute the actual diagnostic Mach-O against synthetic Darwin syscalls.

Checks CPU/log reporting and bounds/privacy. This does not run an XNU kernel,
exercise IOKit user clients, start secondary CPUs or render an iOS screen.
"""
import argparse
from pathlib import Path
import struct
import sys
import uuid
import zlib

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn import arm64_const as A

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import mkinit
import mkpayload

SURFACE = b'DRE SURF: IOCoreSurfaceRoot registered\n'
FRAMEBUFFER = b'DRE FB: AppleCLCD 1080x2400 stride 4320\n'


def run(obj, name, chunks, expected, active=8, accessible=True, cycles=3, native=None, ram_mib=None, ram_actual=None, status_mode=None):
    image = mkinit.build(obj)
    macho = mkpayload.parse_macho(image)
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for segment in macho['segs']:
        if segment['name'] == '__PAGEZERO': continue
        cpu.mem_map(segment['vmaddr'], segment['vmsize'])
        cpu.mem_write(segment['vmaddr'], image[segment['fileoff']:segment['fileoff'] + segment['filesize']])
    stack, stack_size = 0x200000000, 0x40000
    cpu.mem_map(stack, stack_size)
    cpu.reg_write(A.UC_ARM64_REG_SP, stack + stack_size - 15)  # crt0 must align it.
    output, ticks, opens = bytearray(), 0, 0
    native_reads = 0
    gpt = bytearray(4096)
    struct.pack_into('<8sIIIIQQ', gpt, 0, b'EFI PART', 0x10000, 92, 0, 0, 1, 0xffffff)
    struct.pack_into('<I', gpt, 16, zlib.crc32(gpt[:92]))
    if native == 'bad_crc': gpt[60] ^= 1
    chunks = list(chunks)
    keys = {'hw.machine': b'iPhone10,3\0', 'hw.product': b'iPhone10,3\0',
            'hw.model': b'D22AP\0', 'hw.target': b'D22AP\0',
            'hw.ncpu': struct.pack('<I', 8), 'hw.activecpu': struct.pack('<I', active),
            'hw.memsize': struct.pack('<Q', ((ram_actual or ram_mib or 512) - 1) << 20),
            'kern.dre_ufs_state': b'READ:PASS\0',
            'kern.bootargs': b'cpus=8 dre_report=UFS=EMU|SERIAL=EMU\0'}

    def reg(n): return cpu.reg_read(getattr(A, f'UC_ARM64_REG_X{n}'))
    def finish(value, error=False):
        cpu.reg_write(A.UC_ARM64_REG_X0, value)
        flags = cpu.reg_read(A.UC_ARM64_REG_NZCV) & ~(1 << 29)
        cpu.reg_write(A.UC_ARM64_REG_NZCV, flags | ((1 << 29) if error else 0))
    def syscall(uc, number, _):
        nonlocal ticks, opens, native_reads
        call = reg(16)
        if call == 5:
            path = bytes(cpu.mem_read(reg(0), 32)).split(b'\0')[0]
            if path == b'/dev/console': finish(0)
            elif native is not None and path in (b'/dev/rufsroot0', b'/dev/rdremedia0', b'/dev/ufsroot0'):
                if native == 'missing': finish(6, True)
                elif native == 'disk_open_error' and path == b'/dev/rdremedia0': finish(6, True)
                elif native == 'block_open_error' and path == b'/dev/ufsroot0': finish(6, True)
                elif reg(1) == 1:
                    finish(10 if native == 'write_allowed' else 13 if native == 'write_errno' else 30,
                           native != 'write_allowed')
                else: finish({b'/dev/rufsroot0':10, b'/dev/rdremedia0':20, b'/dev/ufsroot0':30}[path])
            else: raise AssertionError(path)
        elif native is not None and call == 6:
            assert reg(0) in (10, 20, 30); finish(0)
        elif native is not None and call == 54:
            assert reg(0) in (10, 20)
            if native == 'ioctl_error': finish(5, True)
            elif native == 'part_count_error' and reg(0) == 10 and reg(1) == 0x40086419: finish(5, True)
            elif native == 'disk_ioctl_error' and reg(0) == 20: finish(25, True)
            else:
                values = {0x40046418: struct.pack('<I', 4096 if native == 'bad_blocksize' else 512),
                          0x40086419: struct.pack('<Q', 0x8000000),
                          0x4004641d: struct.pack('<I', int(native == 'writable'))}
                assert reg(1) in values
                cpu.mem_write(reg(2), values[reg(1)]); finish(0)
        elif native is not None and call == 199:
            assert [reg(i) for i in range(3)] in ([20, 4096, 0], [10, 512, 0], [30, 512, 0])
            if native == 'part_seek_error' and reg(0) == 10: finish(29, True)
            elif native == 'disk_seek_error' and reg(0) == 20: finish(22, True)
            elif native == 'block_seek_error' and reg(0) == 30: finish(22, True)
            else: finish(reg(1))
        elif native is not None and call == 3:
            if reg(0) == 10:
                assert reg(2) == 512
                if native == 'part_read_error': finish(5, True)
                else:
                    cpu.mem_write(reg(1), b'Z' * 512); finish(128 if native == 'part_short_read' else 512)
            elif reg(0) == 30:
                assert reg(2) == 512
                if native == 'block_read_error': finish(5, True)
                else:
                    data = bytes([0x5b if native == 'block_bad_data' else 0x5a]) + b'Z' * 511
                    cpu.mem_write(reg(1), data); finish(128 if native == 'block_short_read' else 512)
            else:
                assert reg(0) == 20 and reg(2) == 4096
                if native == 'disk_read_error': finish(5, True)
                else:
                    native_reads += 1
                    cpu.mem_write(reg(1), bytes(gpt)); finish(512 if native == 'short_read' else 4096)
        elif call == 336:
            opens += 1
            assert [reg(i) for i in range(4)] == [4, 0, 0, 0] and reg(5) == 65536
            if not accessible: finish(1, True)
            else:
                raw = chunks.pop(0) if chunks else b''
                assert len(raw) <= reg(5)
                cpu.mem_write(reg(4), raw); finish(len(raw))
        elif call == 4:
            assert reg(0) == 0 and reg(2) < 4096
            output.extend(cpu.mem_read(reg(1), reg(2))); finish(reg(2))
        elif call == 20: finish(1)
        elif call == 142:
            cpu.mem_write(reg(0), uuid.uuid5(uuid.NAMESPACE_URL, 'urn:oneplus:dre:SYNTHETIC').bytes); finish(0)
        elif call == 274:
            key = bytes(cpu.mem_read(reg(0), reg(1))).decode()
            assert key in keys, key
            value = keys[key]
            if key == 'kern.dre_ufs_state':
                if status_mode == 'missing': finish(2, True); return
                if status_mode == 'oversize':
                    cpu.mem_write(reg(3), struct.pack('<Q', 97)); finish(0); return
                if status_mode == 'unterminated': value = b'FAKE-PRIVATE-IDENTIFIER'.ljust(96, b'X')
                if status_mode == 'control': value = b'READ:\x01FAKE-PRIVATE-IDENTIFIER\0'
            available, = struct.unpack('<Q', cpu.mem_read(reg(3), 8))
            assert len(value) <= available
            cpu.mem_write(reg(2), value); cpu.mem_write(reg(3), struct.pack('<Q', len(value))); finish(0)
        elif call == 93:
            ticks += 1; finish(0)
            if ticks >= cycles: cpu.emu_stop()
        else: raise AssertionError(f'unexpected syscall {call}')

    cpu.hook_add(UC_HOOK_INTR, syscall)
    cpu.emu_start(macho['entry'], 0, count=30000000)
    assert ticks == cycles, 'did not complete diagnostic cycles'
    text = output.decode()
    assert f'CPUS=8/{active} ' + ('PASS' if active == 8 else 'FAIL') in text, text
    assert 'DRE identity checks PASS' in text, text
    assert expected in text, text
    assert 'FAKE-PRIVATE-IDENTIFIER' not in text, 'raw kernel log leaked to console'
    if native is not None:
        result = 'BSD_RAW+BLOCK+GPT_PASS + WRITE_OPEN_REJECTED' if native == 'pass' else 'UNAVAILABLE_OR_FAILED'
        assert text.count('NATIVE_UFS=' + result) == cycles, text
        assert native_reads == int(native in ('pass', 'short_read', 'bad_crc') or native.startswith('block_')), 'native result was not cached'
        stages = {'missing': (1,6), 'ioctl_error':(2,5), 'bad_blocksize':(2,0),
                  'part_count_error':(3,5), 'writable':(4,0), 'part_seek_error':(5,29),
                  'part_read_error':(6,5), 'part_short_read':(6,0), 'write_errno':(7,13),
                  'write_allowed':(7,0), 'disk_open_error':(8,6), 'disk_ioctl_error':(9,25),
                  'disk_seek_error':(10,22), 'disk_read_error':(11,5), 'short_read':(11,0), 'bad_crc':(12,0),
                  'block_open_error':(13,6), 'block_seek_error':(14,22), 'block_read_error':(15,5),
                  'block_short_read':(15,0), 'block_bad_data':(15,0)}
        if native == 'pass': assert 'NATIVE_STAGE=' not in text
        else:
            stage, error = stages[native]
            assert text.count(f'NATIVE_STAGE={stage:03} ERRNO={error:03}') == cycles, text
        state = 'STATUS_UNAVAILABLE' if status_mode else 'READ:PASS'
        assert text.count('NATIVE_DRIVER=' + state) == cycles, text
    if ram_mib is not None:
        message = f'RAM_MIB={ram_mib - 1} PASS' if ram_actual in (None, ram_mib) else 'RAM_SIZE=UNEXPECTED'
        assert text.count(message) == cycles, text
    if expected.endswith('SURFACE+FRAMEBUFFER') and len(chunks) == 0:
        assert opens <= 2, 'successful startup evidence was not cached'
    print('PASS', name)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('object', type=Path, help='Mach-O object assembled with CPU/UI/board flags')
    ap.add_argument('--native-ufs', action='store_true')
    ap.add_argument('--ram-mib', type=int, choices=(512, 1024, 2048))
    args = ap.parse_args()
    obj = args.object.read_bytes()
    if args.native_ufs:
        for mode in ('pass', 'missing', 'bad_crc', 'short_read', 'ioctl_error', 'writable',
                     'bad_blocksize', 'part_count_error', 'part_seek_error', 'part_read_error',
                     'part_short_read', 'write_errno', 'write_allowed', 'disk_open_error',
                     'disk_ioctl_error', 'disk_seek_error', 'disk_read_error', 'block_open_error',
                     'block_seek_error', 'block_read_error', 'block_short_read', 'block_bad_data'):
            run(obj, 'native ' + mode, [SURFACE + FRAMEBUFFER], 'UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER', native=mode, ram_mib=args.ram_mib)
        if args.ram_mib and args.ram_mib != 512:
            run(obj, 'wrong RAM size', [SURFACE + FRAMEBUFFER], 'UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER',
                native='pass', ram_mib=args.ram_mib, ram_actual=512)
        for mode in ('missing', 'oversize', 'unterminated', 'control'):
            run(obj, 'driver status ' + mode, [SURFACE + FRAMEBUFFER], 'UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER',
                native='pass', ram_mib=args.ram_mib, status_mode=mode)
        sys.exit(0)
    run(obj, 'both', [b'FAKE-PRIVATE-IDENTIFIER\n' + SURFACE + FRAMEBUFFER], 'UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER')
    run(obj, 'separate reads', [SURFACE, FRAMEBUFFER], 'UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER')
    run(obj, 'empty', [b''], 'UI_DRIVER_LOGS=NOT_BOTH_SEEN')
    run(obj, 'surface only', [SURFACE], 'UI_DRIVER_LOGS=NOT_BOTH_SEEN')
    run(obj, 'framebuffer only', [FRAMEBUFFER], 'UI_DRIVER_LOGS=NOT_BOTH_SEEN')
    run(obj, 'near miss', [SURFACE[:-1] + b'FAILED\nDRE FB: AppleCLCD_FAIL\n'], 'UI_DRIVER_LOGS=NOT_BOTH_SEEN')
    run(obj, 'unavailable', [], 'UI_DRIVER_LOGS=UNAVAILABLE', accessible=False)
    run(obj, 'offline CPU', [SURFACE + FRAMEBUFFER], 'UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER', active=7)
    for size in (len(SURFACE + FRAMEBUFFER), 65535, 65536):
        run(obj, f'bounded {size}', [b'X' * (size - len(SURFACE + FRAMEBUFFER)) + SURFACE + FRAMEBUFFER],
            'UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER')
