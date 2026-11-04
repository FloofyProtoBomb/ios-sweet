#!/usr/bin/env python3
"""Bounds-check the firmware parser using synthetic layouts, never bundled blobs."""
import argparse
import ctypes as C
import os
from pathlib import Path
import random
import struct
import subprocess

HERE = Path(__file__).resolve().parent


class Partition(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ('file_offset', 'address', 'length', 'crc')]


class Firmware(C.Structure):
    _fields_ = [('write_length', C.c_uint32), ('version', C.c_uint32),
                ('count', C.c_uint32), ('parts', Partition * 32)]


def fixture(overlay=False):
    p = bytearray(8192)
    struct.pack_into('<6I', p, 0, 0x30 if overlay else 0x60, 0x10000, 63, 0x100, 0x20000, 31)
    if overlay:
        p[0x28] = 0x11
        struct.pack_into('<4I', p, 0x100, 0x30000, 15, 0x300, 0x12345678)
    else:
        struct.pack_into('<4I', p, 0x30, 0x40000, 0x5f, 0, 0xaabbccdd)
    p[4096:4098] = bytes((9, 246))
    p[-3:] = b'NVT'
    return p


def run(lib, private_files):
    parse = lib.dre_touch_parse_firmware
    parse.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(Firmware)]
    parse.restype = C.c_char_p
    count = 0

    def call(p, expected=None):
        nonlocal count
        out = Firmware()
        C.memset(C.byref(out), 0xa5, C.sizeof(out))
        before = bytes(out)
        error = parse(C.create_string_buffer(bytes(p)), len(p), C.byref(out))
        result = error.decode() if error else None
        if expected != '*':
            assert result == expected, (result, expected)
        if error:
            assert before == bytes(out), 'rejected firmware partially changed output'
        else:
            assert out.count <= 32 and out.write_length <= len(p)
            for part in out.parts[:out.count]:
                assert not part.length or (part.file_offset + part.length <= out.write_length and
                                           part.address + part.length <= 0x1000000)
        count += 1
        return out

    normal = fixture()
    fw = call(normal)
    assert (fw.write_length, fw.version, fw.count) == (8192, 9, 5)
    assert tuple(getattr(fw.parts[0], k) for k in ('file_offset', 'address', 'length')) == (0x60, 0x10000, 64)
    assert fw.parts[2].length == 0x60 and fw.parts[2].crc == 0xaabbccdd
    overlay = call(fixture(True))
    assert overlay.count == 3 and overlay.parts[2].crc == 0x12345678 and overlay.parts[2].length == 16
    call(normal + b'\xa5' * 208)
    factory = normal.copy(); factory[-3:] = b'MOD'; call(factory)
    for n in (0, 1, 3, 4, 31, 47, 48, 63, 4095, 512 * 1024 + 1):
        call(bytes(n), 'SIZE')
    bad = normal.copy(); bad[-3:] = b'BAD'; call(bad, 'END_MARKER')
    bad = normal.copy(); bad[4097] ^= 1; call(bad, 'VERSION')
    for header in (0, 0x31, 0x10000):
        bad = normal.copy(); struct.pack_into('<I', bad, 0, header); call(bad, 'HEADER')
    bad = normal.copy(); struct.pack_into('<I', bad, 0, 0x230); call(bad, 'PART_COUNT')
    for off, value, error in ((12, 0xffffffff, 'FILE_RANGE'), (8, 0xffffffff, 'FILE_RANGE'),
                               (4, 0xfffff0, 'SRAM_RANGE'), (4, 0xffffffff, 'SRAM_RANGE'),
                               (8, 0, 'EMPTY_CORE')):
        bad = normal.copy(); struct.pack_into('<I', bad, off, value); call(bad, error)
    bad = fixture(True); struct.pack_into('<I', bad, 12, 8190); call(bad, 'FILE_RANGE')
    # Reserved empty DLM would still put its overlay header out of bounds.
    bad = fixture(True); struct.pack_into('<I', bad, 20, 0); struct.pack_into('<I', bad, 12, 8190)
    call(bad, 'OVERLAY_HEADER')
    for n in range(4096, 8192, 37):
        call(normal[:n], 'END_MARKER')
    rng = random.Random(36672)
    for _ in range(500):
        bad = (normal if rng.randrange(2) else fixture(True)).copy()
        off = rng.choice([*range(0x80), *range(0x100, 0x110), 4096, 4097, 8189, 8190, 8191])
        bad[off] ^= 1 << rng.randrange(8)
        call(bad, '*')
    assert parse(None, len(normal), C.byref(Firmware())) == b'SIZE'
    assert parse(C.create_string_buffer(bytes(normal)), len(normal), None) == b'SIZE'
    print(f'PASS {count + 2} firmware-parser checks; no upload or hardware access')
    for path in private_files:
        fw = call(path.read_bytes())
        print(f'PASS private fixture {path.name}: {fw.write_length} bytes, {fw.count} parts, version {fw.version:#x}')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zig', default='zig')
    ap.add_argument('--out', type=Path, default=HERE.parent / 'out/firmware-test')
    ap.add_argument('--firmware', type=Path, action='append', default=[])
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    library = (args.out / ('touch-firmware.dll' if os.name == 'nt' else 'touch-firmware.so')).resolve()
    flags = ['-Wl,--export-all-symbols'] if os.name == 'nt' else ['-fPIC']
    subprocess.run([args.zig, 'cc', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', *flags,
                    str(HERE / 'firmware.c'), '-o', str(library)], check=True)
    subprocess.run([args.zig, 'cc', '-target', 'aarch64-linux-musl', '-O2', '-ffreestanding',
                    '-fno-builtin', '-mstrict-align', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
                    '-c', str(HERE / 'firmware.c'), '-o', str(args.out / 'firmware-arm64.o')], check=True)
    run(C.CDLL(str(library)), args.firmware)
