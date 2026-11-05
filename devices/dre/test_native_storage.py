#!/usr/bin/env python3
"""Run the native VA/PA engine and ownership ABI against synthetic hardware.

No handset, disk device or Apple artifact is used. This tests the production
C engine, including restore failures, rather than a Python reimplementation.
"""
import argparse
import ctypes as C
from pathlib import Path
import struct
import subprocess
import sys
import zlib

HERE = Path(__file__).resolve().parent
READ = C.CFUNCTYPE(C.c_int, C.c_size_t, C.POINTER(C.c_uint32))
WRITE = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_uint32)
GUARD = C.CFUNCTYPE(C.c_int, C.c_void_p)
class Route(C.Structure):
    _fields_ = [('v', C.c_uint32 * 6), ('matches', C.c_uint), ('physical', C.c_int), ('translated', C.c_int)]
class State(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('caps', 'block', 'ocs', 'response', 'status')] + \
        [(n, C.c_uint64) for n in ('last', 'first', 'blocks')] + \
        [('commands', C.c_uint), ('safe', C.c_int), ('guard', C.c_void_p), ('cookie', C.c_void_p), ('lost', C.c_int)]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zig', default='zig'); ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    stub = args.out / 'platform.c'
    stub.write_text('''#include <stdint.h>
volatile uint8_t *dre_kernel_ufs_arena;
uint64_t dre_kernel_ufs_arena_pa;
int (*read_cb)(uintptr_t, uint32_t *);
int (*write_cb)(uintptr_t, uint32_t);
int dre_read32(uintptr_t a, uint32_t *v) { return read_cb(a,v); }
int dre_write32(uintptr_t a, uint32_t v) { return write_cb(a,v); }
uint64_t dre_ufs_test_ticks(void) { static uint64_t t; return t += 1000000; }
''')
    dll = args.out / ('engine.dll' if sys.platform == 'win32' else 'engine.so')
    subprocess.run([args.zig, 'cc', '-O2', '-shared', '-DDRE_UFS_KERNEL', '-DDRE_UFS_HOST_TEST',
                    *(['-Wl,--export-all-symbols'] if sys.platform == 'win32' else ['-fPIC']),
                    str(HERE / 'ufs_read.c'), str(HERE / 'storage_abi.c'), str(stub), '-o', str(dll)], check=True)
    api = C.CDLL(str(dll.resolve()))
    probe = api.dre_ufs_read_guarded
    probe.argtypes = [C.POINTER(Route), C.POINTER(State), GUARD, C.c_void_p]; probe.restype = C.c_char_p
    read = api.dre_ufs_read_block_guarded
    read.argtypes = [C.POINTER(Route), C.POINTER(State), GUARD, C.c_void_p, C.c_uint32, C.c_void_p]
    read.restype = C.c_char_p
    valid = api.dre_storage_abi_valid; valid.argtypes = [C.POINTER(C.c_uint64)]; valid.restype = C.c_int
    checks = 0
    base = [0x0031534655455244, 0xa8040000, 0x4000, 0xb2000000, 0x100000, 0xa8000000, 0x2000000, 0]
    def abi(words, expected):
        nonlocal checks
        words = list(words); words[7] = zlib.crc32(struct.pack('<7Q', *words[:7]))
        assert bool(valid((C.c_uint64 * 8)(*words))) == expected, words
        checks += 1
    abi(base, True)
    for index, value in ((0, 0), (1, 0xa8040001), (1, 0xa7ffc000), (1, 0xa9ffc001),
                         (2, 8192), (3, 0xa8008000), (3, 0xffffffff), (3, 0x70000000),
                         (4, 0), (4, 0x200001), (5, 0x100000000), (5, 0xffffffffffffffff),
                         (6, 0), (6, 0x6000001)):
        w = list(base); w[index] = value; abi(w, False)
    bad = (C.c_uint64 * 8)(*base); assert not valid(bad); checks += 1
    memory = C.create_string_buffer(0x8000)
    va = (C.addressof(memory) + 0x3fff) & ~0x3fff
    pa = 0xa8040000
    C.c_void_p.in_dll(api, 'dre_kernel_ufs_arena').value = va
    C.c_uint64.in_dll(api, 'dre_kernel_ufs_arena_pa').value = pa
    assert va != pa, 'test requires different virtual and physical addresses'
    arena = (C.c_ubyte * 0x4000).from_address(va)
    entries = bytearray(4096)
    entries[0] = 1
    struct.pack_into('<QQ', entries, 32, 100, 199)
    entries[56:74] = 'userdata\0'.encode('utf-16le')
    header = bytearray(4096)
    struct.pack_into('<8sIIIIQQQQ', header, 0, b'EFI PART', 0x10000, 92, 0, 0, 1, 999, 3, 998)
    struct.pack_into('<QIII', header, 72, 2, 32, 128, zlib.crc32(entries))
    struct.pack_into('<I', header, 16, zlib.crc32(header[:92]))
    payload = bytes((i * 17 + 3) % 256 for i in range(4096))
    disk = {1: bytes(header), 2: bytes(entries), 100: payload}
    for mode in ('pass', 'guard_before', 'guard_after', 'timeout', 'ocs', 'restore', 'short'):
        regs = {0: 1 << 24, 0x34: 1, 0x30: 3, 0x58: 0, 0x78: 0,
                0x50: 0x81000000, 0x54: 0, 0x60: 1, 0x24: 0x1234}
        commands = []; active_mode = 'pass'; callback_errors = []
        @READ
        def rd(address, out):
            if address - 0x04804000 not in regs: return 0
            out[0] = regs[address - 0x04804000]; return 1
        @WRITE
        def wr(address, value):
            try:
                off = address - 0x04804000
                assert off in (0x20, 0x24, 0x50, 0x54, 0x58, 0x5c, 0x60)
                if active_mode == 'restore' and off == 0x50 and value == 0x81000000: return 0
                if off == 0x5c: regs[0x58] = 0; return 1
                regs[off] = value
                if off != 0x58: return 1
                assert value == 1 and regs[0x50] == pa
                words = struct.unpack('<8I', bytes(arena[:32]))
                assert words[4:6] == (pa + 0x400, 0), words
                command = bytes(arena[0x400:0x480]); op = 0xff if command[0] == 0 else command[16]
                assert op in (0xff, 0, 0x9e, 0x28) and command[2] == 0
                length = int.from_bytes(command[12:16], 'big')
                if length:
                    assert struct.unpack('<II', bytes(arena[0x800:0x808])) == (pa + 0x1000, 0)
                commands.append(op)
                if active_mode == 'timeout': return 1
                data = b''
                if op == 0x9e: data = struct.pack('>QI', 999, 4096) + bytes(20)
                if op == 0x28:
                    assert command[24] == 1
                    data = disk[int.from_bytes(command[18:22], 'big')]
                for i, v in enumerate(data): arena[0x1000 + i] = v
                arena[8] = int(active_mode == 'ocs')
                arena[0x600] = 0x20 if op == 0xff else 0x21
                arena[0x603] = command[3]
                if active_mode == 'short': arena[0x601] = 0x20
                regs[0x58] = 0
                return 1
            except BaseException as error:
                callback_errors.append(error); return 0
        @GUARD
        def guard(_):
            return int(active_mode != 'guard_before' and not (active_mode == 'guard_after' and commands))
        C.c_void_p.in_dll(api, 'read_cb').value = C.cast(rd, C.c_void_p).value
        C.c_void_p.in_dll(api, 'write_cb').value = C.cast(wr, C.c_void_p).value
        route, state = Route(), State()
        assert probe(C.byref(route), C.byref(state), guard, None) == b'GPT_PASS'
        assert (state.first, state.blocks, state.last, state.safe) == (100, 100, 999, 1)
        checks += 1; commands.clear(); active_mode = mode
        out = C.create_string_buffer(b'Z' * 4096, 4096)
        result = read(C.byref(route), C.byref(state), guard, None, 100, out)
        expected = {'pass': b'READ_PASS', 'guard_before': b'DMA_GUARD_BLOCKED',
                    'guard_after': b'DMA_GUARD_CHANGED', 'timeout': b'COMMAND_TIMEOUT',
                    'ocs': b'BAD_RESPONSE', 'restore': b'RESTORE_FAILED', 'short': b'SHORT_TRANSFER'}[mode]
        assert not callback_errors, callback_errors
        assert result == expected, (mode, result)
        assert bool(state.safe) == (mode not in ('guard_after', 'restore'))
        assert out.raw == (payload if mode == 'pass' else b'Z' * 4096)
        assert commands == ([] if mode == 'guard_before' else [0x28])
        if mode != 'restore': assert [regs[o] for o in (0x50, 0x54, 0x60, 0x24)] == [0x81000000, 0, 1, 0x1234]
        checks += 1
        if mode == 'pass':
            commands.clear()
            assert read(C.byref(route), C.byref(state), guard, None, 1000, out) == b'LBA_RANGE'
            assert not commands; checks += 1
    print(f'PASS {checks} native ownership/VA-PA/read/restore checks')

if __name__ == '__main__': main()
