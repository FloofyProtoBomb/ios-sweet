#!/usr/bin/env python3
"""Exercise the portable NT36672C decoder with synthetic wire reports."""
import argparse
import ctypes as C
import os
from pathlib import Path
import random
import subprocess

HERE = Path(__file__).resolve().parent


class Contact(C.Structure):
    _fields_ = [(name, C.c_uint16) for name in ('x', 'y', 'pressure', 'width')]


class Frame(C.Structure):
    _fields_ = [('active_mask', C.c_uint16), ('count', C.c_uint16), ('contacts', Contact * 10)]


def checksum(payload):
    payload[64] = (-sum(payload[:64])) & 255
    return bytes(payload)


def run(lib):
    decode = lib.dre_touch_decode
    decode.argtypes = [C.c_void_p, C.c_size_t, C.c_uint16, C.c_uint16, C.POINTER(Frame)]
    decode.restype = C.c_int
    checked = 0

    def call(payload, result=0, maxima=(4095, 4095), length=None):
        nonlocal checked
        output = Frame()
        C.memset(C.byref(output), 0xa5, C.sizeof(output))
        before = bytes(output)
        buf = C.create_string_buffer(bytes(payload))
        actual = decode(buf, len(payload) if length is None else length, *maxima, C.byref(output))
        assert actual == result, (actual, result)
        if result:
            assert bytes(output) == before, 'rejected report partially changed the frame'
        checked += 1
        return output

    # Literal field packing exercises independent expected coordinates/pressure.
    packet = bytearray(65)
    packet[:6] = bytes.fromhex('09 12 34 56 00 2a')
    packet[62] = 1
    valid = checksum(packet)
    frame = call(valid)
    assert (frame.active_mask, frame.count) == (1, 1)
    assert tuple(getattr(frame.contacts[0], k) for k in ('x', 'y', 'pressure', 'width')) == (293, 838, 298, 1)
    assert not any(bytes(frame.contacts[i]) != bytes(8) for i in range(1, 10))
    assert bytes(call(valid + b'debug-data')) == bytes(frame)
    call(valid, maxima=(292, 838), result=5)
    call(valid, maxima=(293, 837), result=5)
    call(valid, maxima=(293, 838))
    for n in range(65):
        call(valid[:n], result=2)
    for n in range(65):
        corrupted = bytearray(valid)
        corrupted[n] ^= 1
        call(corrupted, result=3)
    for first in (1, 11 * 8 + 1, 31 * 8 + 2):
        bad = bytearray(valid)
        bad[0] = first
        call(checksum(bad), result=4)
    duplicate = bytearray(valid)
    duplicate[6:12] = duplicate[:6]
    call(checksum(duplicate), result=4)
    # The high pressure bytes follow wire slot order, even with shuffled IDs.
    shuffled = bytearray(65)
    shuffled[0:6] = bytes.fromhex('4a 01 02 34 05 fa')  # id 9, move
    shuffled[6:12] = bytes.fromhex('11 02 03 45 06 00') # id 2, down
    shuffled[62:64] = bytes((3, 0))
    frame = call(checksum(shuffled))
    assert (frame.active_mask, frame.count) == (0x102, 2)
    assert (frame.contacts[8].pressure, frame.contacts[1].pressure) == (1000, 1)
    # A valid release record produces an empty next frame. Faults never do.
    released = bytearray(valid)
    released[0] = 11
    assert bytes(call(checksum(released))) == bytes(C.sizeof(Frame))
    call(bytes((0x77,)) * 65, result=6)
    call(bytes((0xfd, 0xfe)) * 33, result=7)
    call(valid, maxima=(0, 4095), result=1)
    call(valid, maxima=(4096, 4095), result=1)
    sentinel = Frame()
    assert decode(None, 65, 1080, 2400, C.byref(sentinel)) == 1
    assert decode(C.create_string_buffer(valid), 65, 1080, 2400, None) == 1

    rng = random.Random(36672)
    for _ in range(300):
        packet = bytearray(65)
        ids = rng.sample(range(1, 11), rng.randrange(11))
        expected = {}
        for slot, identity in enumerate(ids):
            x, y = rng.randrange(1081), rng.randrange(2401)
            width, pressure = rng.randrange(256), rng.randrange(1001) if slot < 2 else rng.randrange(256)
            packet[slot * 6:slot * 6 + 6] = bytes((identity * 8 + rng.choice((1, 2)),
                x // 16, y // 16, x % 16 * 16 + y % 16, width, pressure % 256))
            if slot < 2:
                packet[62 + slot] = pressure // 256
            expected[identity - 1] = (x, y, max(1, pressure), max(1, width))
        frame = call(checksum(packet), maxima=(1080, 2400))
        assert frame.count == len(ids)
        assert frame.active_mask == sum(1 << i for i in expected)
        for index, contact in enumerate(frame.contacts):
            assert tuple(getattr(contact, k) for k in ('x', 'y', 'pressure', 'width')) == expected.get(index, (0, 0, 0, 0))
    print(f'PASS {checked + 2} touch-report checks; synthetic reports only, no SPI or HID access')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zig', default='zig')
    ap.add_argument('--out', type=Path, default=HERE.parent / 'out/touch-test')
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    library = (args.out / ('touch-report.dll' if os.name == 'nt' else 'touch-report.so')).resolve()
    flags = ['-Wl,--export-all-symbols'] if os.name == 'nt' else ['-fPIC']
    subprocess.run([args.zig, 'cc', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', *flags,
                    str(HERE / 'report.c'), '-o', str(library)], check=True)
    subprocess.run([args.zig, 'cc', '-target', 'aarch64-linux-musl', '-O2', '-ffreestanding',
                    '-fno-builtin', '-mstrict-align', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
                    '-c', str(HERE / 'report.c'), '-o', str(args.out / 'touch-report-arm64.o')], check=True)
    run(C.CDLL(str(library)))
