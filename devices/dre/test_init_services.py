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


def run(obj, name, chunks, expected, active=8, accessible=True, cycles=3, native=None):
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
            elif native is not None and path in (b'/dev/rufsroot0', b'/dev/rdremedia0'):
                if native == 'missing': finish(6, True)
                elif reg(1) == 1: finish(30, True)
                else: finish(10 if path.endswith(b'rufsroot0') else 20)
            else: raise AssertionError(path)
        elif native is not None and call == 6:
            assert reg(0) in (10, 20); finish(0)
        elif native is not None and call == 54:
            assert reg(0) in (10, 20)
            if native == 'ioctl_error': finish(5, True)
            else:
                values = {0x40046418: struct.pack('<I', 512),
                          0x40086419: struct.pack('<Q', 0x8000000),
                          0x4004641d: struct.pack('<I', int(native == 'writable'))}
                assert reg(1) in values
                cpu.mem_write(reg(2), values[reg(1)]); finish(0)
        elif native is not None and call == 199:
            assert [reg(i) for i in range(3)] == [20, 4096, 0]; finish(4096)
        elif native is not None and call == 3:
            assert reg(0) == 20 and reg(2) == 4096
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
        result = 'BSD_GPT_READ_PASS + WRITE_OPEN_REJECTED' if native == 'pass' else 'UNAVAILABLE_OR_FAILED'
        assert text.count('NATIVE_UFS=' + result) == cycles, text
        assert native_reads == int(native in ('pass', 'short_read', 'bad_crc')), 'native result was not cached'
    if expected.endswith('SURFACE+FRAMEBUFFER') and len(chunks) == 0:
        assert opens <= 2, 'successful startup evidence was not cached'
    print('PASS', name)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('object', type=Path, help='Mach-O object assembled with CPU/UI/board flags')
    ap.add_argument('--native-ufs', action='store_true')
    args = ap.parse_args()
    obj = args.object.read_bytes()
    if args.native_ufs:
        for mode in ('pass', 'missing', 'bad_crc', 'short_read', 'ioctl_error', 'writable'):
            run(obj, 'native ' + mode, [SURFACE + FRAMEBUFFER], 'UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER', native=mode)
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
