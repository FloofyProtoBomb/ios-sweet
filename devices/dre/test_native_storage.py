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
from test_driver_prep import fixture

HERE = Path(__file__).resolve().parent
READ = C.CFUNCTYPE(C.c_int, C.c_size_t, C.POINTER(C.c_uint32))
WRITE = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_uint32)
GUARD = C.CFUNCTYPE(C.c_int, C.c_void_p)
TICKS = C.CFUNCTYPE(C.c_uint64)
class Route(C.Structure):
    _fields_ = [('v', C.c_uint32 * 6), ('matches', C.c_uint), ('physical', C.c_int), ('translated', C.c_int)]
class State(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('caps', 'block', 'ocs', 'response', 'status')] + \
        [(n, C.c_uint64) for n in ('last', 'first', 'blocks')] + \
        [('commands', C.c_uint), ('safe', C.c_int), ('guard', C.c_void_p), ('cookie', C.c_void_p), ('lost', C.c_int)] + \
        [(n,C.c_uint32) for n in ('last_opcode','command_lba','fault_doorbell','fault_interrupts','fault_hcs','fault_ocs')] + \
        [('fault_uec',C.c_uint32*5)] + \
        [(n,C.c_uint32) for n in ('fault_valid','fault_power','fault_utrl_lo','fault_utrl_hi')] + \
        [('command_ticks',C.c_uint64), ('command_blocks', C.c_uint)]

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zig', default='zig'); ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--device-dtb', type=Path, help='optional private live-DT ownership fixture; never printed/copied')
    args = ap.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    stub = args.out / 'platform.c'
    stub.write_text('''#include <stdint.h>
#include "storage_live.h"
int dre_storage_test_live(const void *dt, const uint64_t w[8], unsigned size) {
    return !fdt_check_full(dt, size) && dre_storage_live_valid(dt, w);
}
volatile uint8_t *dre_kernel_ufs_arena;
uint64_t dre_kernel_ufs_arena_pa;
int (*read_cb)(uintptr_t, uint32_t *);
int (*write_cb)(uintptr_t, uint32_t);
uint64_t (*tick_cb)(void);
int dre_read32(uintptr_t a, uint32_t *v) { return read_cb(a,v); }
int dre_write32(uintptr_t a, uint32_t v) { return write_cb(a,v); }
uint64_t dre_ufs_test_ticks(void) { static uint64_t t; return tick_cb ? tick_cb() : (t += 1000000); }
''')
    dll = args.out / ('engine.dll' if sys.platform == 'win32' else 'engine.so')
    cells = args.out / 'cells.c'
    cells.write_text((HERE / 'libfdt/fdt_addresses.c').read_text().split('/* This function assumes')[0])
    subprocess.run([args.zig, 'cc', '-O2', '-shared', '-DDRE_UFS_KERNEL', '-DDRE_UFS_HOST_TEST',
                    *(['-Wl,--export-all-symbols'] if sys.platform == 'win32' else ['-fPIC']),
                    '-I' + str(HERE), '-I' + str(HERE / 'libfdt'), str(HERE / 'ufs_read.c'),
                    str(HERE / 'storage_abi.c'), str(HERE / 'dma_memory.c'),
                    *(str(HERE / 'libfdt' / f) for f in ('fdt.c', 'fdt_ro.c', 'fdt_check.c')),
                    str(cells), str(stub), '-o', str(dll)], check=True)
    api = C.CDLL(str(dll.resolve()))
    probe = api.dre_ufs_read_guarded
    probe.argtypes = [C.POINTER(Route), C.POINTER(State), GUARD, C.c_void_p]; probe.restype = C.c_char_p
    read = api.dre_ufs_read_block_guarded
    read.argtypes = [C.POINTER(Route), C.POINTER(State), GUARD, C.c_void_p, C.c_uint32, C.c_void_p]
    read.restype = C.c_char_p
    read_blocks = api.dre_ufs_read_blocks_guarded
    read_blocks.argtypes = [*read.argtypes[:-1], C.c_uint, C.c_void_p]
    read_blocks.restype = C.c_char_p
    write = api.dre_ufs_write_userdata_guarded
    write.argtypes = [C.POINTER(Route), C.POINTER(State), GUARD, C.c_void_p, C.c_uint64, C.c_void_p]
    write.restype = C.c_char_p
    flush = api.dre_ufs_flush_guarded
    flush.argtypes = [C.POINTER(Route), C.POINTER(State), GUARD, C.c_void_p]
    flush.restype = C.c_char_p
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
    live = api.dre_storage_test_live
    live.argtypes = [C.c_void_p, C.POINTER(C.c_uint64), C.c_uint]; live.restype = C.c_int
    safe_initrd = (0xa4500000, 0xa5c00000)
    for data, expected in ((fixture(initrd=safe_initrd), True), (fixture(), False),
                           (fixture(initrd=(0xa8040000, 0xa8044000)), False),
                           (fixture(initrd=(0xb0000000, 0xa0000000)), False),
                           (fixture(initrd=safe_initrd, reserve=[(base[1], 16384)]), False),
                           (fixture(initrd=safe_initrd, dynamic=[(base[3], base[4])]), False),
                           (fixture(initrd=safe_initrd, memreserve=[(base[1], 16384)]), False)):
        w = list(base); w[7] = zlib.crc32(struct.pack('<7Q', *w[:7]))
        assert bool(live(C.create_string_buffer(data), (C.c_uint64 * 8)(*w), len(data))) == expected
        checks += 1
    pool = fixture(initrd=safe_initrd, owned_pools=[(0, 0xffffffff)])
    w = list(base); w[7] = zlib.crc32(struct.pack('<7Q', *w[:7]))
    assert live(C.create_string_buffer(pool), (C.c_uint64 * 8)(*w), len(pool)); checks += 1
    ram = api.dre_dma_ram_allowed
    ram.argtypes = [C.c_void_p, C.c_uint64, C.c_uint64]; ram.restype = C.c_int
    assert not ram(C.create_string_buffer(pool), base[1], base[2]); checks += 1
    if args.device_dtb:
        data = args.device_dtb.read_bytes()
        w = list(base); w[3] = 0xa0000000; w[4] = len(data)
        w[7] = zlib.crc32(struct.pack('<7Q', *w[:7]))
        assert live(C.create_string_buffer(data), (C.c_uint64 * 8)(*w), len(data)); checks += 1
        w[3] = 0x82000000; w[7] = zlib.crc32(struct.pack('<7Q', *w[:7]))
        assert not live(C.create_string_buffer(data), (C.c_uint64 * 8)(*w), len(data)); checks += 1
        for mib in (512, 1024, 1536, 2048, 2049):
            assert bool(ram(C.create_string_buffer(data), 0x100000000, mib << 20)) == (mib <= 2048)
            checks += 1
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
    disk = {1: bytes(header), 2: bytes(entries), 100: payload, 101: bytes([0x81]) * 4096,
            102: bytes([0x42]) * 4096}
    tick_hook = C.c_void_p.in_dll(api, 'tick_cb')
    for mode in ('pass', 'guard_before', 'guard_after', 'timeout', 'ocs', 'restore', 'short',
                 'timeout_restore', 'timeout_power_off', 'timeout_power_unreadable', 'timeout_layer_unreadable'):
        regs = {0: 1 << 24, 0x34: 1, 0x30: 3, 0x58: 0, 0x78: 0,
                0x50: 0x81000000, 0x54: 0, 0x60: 1, 0x24: 0x1234, 0x20: 0x100}
        regs.update({0x38 + i * 4: 0x80000001 + i for i in range(5)})
        commands = []; active_mode = 'pass'; callback_errors = []
        pending_polls = 0
        timer_samples = iter(())
        @TICKS
        def counter():
            return next(timer_samples, 100000001)
        @READ
        def rd(address, out):
            nonlocal pending_polls
            if address == 0x01445004:
                if active_mode == 'timeout_power_unreadable': return 0
                out[0] = 0 if active_mode == 'timeout_power_off' else 1 << 31
                return 1
            if active_mode == 'timeout_layer_unreadable' and address == 0x04804040: return 0
            if address - 0x04804000 not in regs: return 0
            offset = address - 0x04804000
            out[0] = regs[offset]
            if offset == 0x58 and pending_polls:
                pending_polls -= 1
                if not pending_polls:
                    regs[0x58] = 0
            return 1
        @WRITE
        def wr(address, value):
            nonlocal pending_polls, timer_samples
            try:
                off = address - 0x04804000
                assert off in (0x20, 0x24, 0x50, 0x54, 0x58, 0x5c, 0x60)
                if active_mode in ('restore', 'write_restore', 'timeout_restore') and off == 0x50 and value == 0x81000000: return 0
                if active_mode in ('timeout_power_off', 'timeout_power_unreadable') and commands:
                    assert False, 'attempted register restoration after lost/unverified domain'
                if off == 0x5c: regs[0x58] = 0; return 1
                regs[off] = value
                if off != 0x58: return 1
                assert value == 1 and regs[0x50] == pa
                words = struct.unpack('<8I', bytes(arena[:32]))
                assert words[4:6] == (pa + 0x400, 0), words
                command = bytes(arena[0x400:0x480]); op = 0xff if command[0] == 0 else command[16]
                assert op in (0xff, 0, 0x9e, 0x28, 0x2a, 0x35) and command[2] == 0
                length = int.from_bytes(command[12:16], 'big')
                if length:
                    assert struct.unpack('<II', bytes(arena[0x800:0x808])) == (pa + 0x1000, 0)
                    assert words[7] == (256 << 16) | 1
                    assert struct.unpack('<I', bytes(arena[0x80c:0x810])) == (length - 1,)
                    assert command[1] == (0x20 if op == 0x2a else 0x40)
                    assert (words[0] >> 25) & 3 == (1 if op == 0x2a else 2)
                commands.append(op)
                if active_mode.startswith('timeout') or active_mode == 'write_timeout' and op == 0x2a or active_mode == 'flush_timeout' and op == 0x35: return 1
                data = b''
                if op == 0x9e: data = struct.pack('>QI', 999, 4096) + bytes(20)
                if op == 0x28:
                    count = int.from_bytes(command[23:25], 'big')
                    lba = int.from_bytes(command[18:22], 'big')
                    assert 1 <= count <= 3 and length == count * 4096
                    data = b''.join(disk[lba + i] for i in range(count))
                if op == 0x2a:
                    assert command[24] == 1 and length == 4096
                    lba = int.from_bytes(command[18:22], 'big')
                    assert 100 <= lba < 200, 'write escaped userdata'
                    disk[lba] = bytes(arena[0x1000:0x2000])
                for i, v in enumerate(data): arena[0x1000 + i] = v
                arena[8] = int(active_mode == 'ocs' or active_mode == 'write_ocs' and op == 0x2a or active_mode == 'flush_ocs' and op == 0x35)
                arena[0x600] = 0x20 if op == 0xff else 0x21
                arena[0x603] = command[3]
                if active_mode == 'short': arena[0x601] = 0x20
                if active_mode == 'counter_backstep' and op == 0x28 and command[18:22] == bytes.fromhex('00000002'):
                    pending_polls = 2
                    timer_samples = iter((100000000, 100000000, 99999999, 100000001))
                    tick_hook.value = C.cast(counter, C.c_void_p).value
                else:
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
        assert commands == [0xff, 0, 0x9e, 0x28, 0x28], 'read-only probe submitted a write'
        assert (state.first, state.blocks, state.last, state.safe) == (100, 100, 999, 1)
        checks += 1; commands.clear(); active_mode = mode
        out = C.create_string_buffer(b'Z' * 4096, 4096)
        result = read(C.byref(route), C.byref(state), guard, None, 100, out)
        expected = {'pass': b'READ_PASS', 'guard_before': b'DMA_GUARD_BLOCKED',
                    'guard_after': b'DMA_GUARD_CHANGED', 'timeout': b'COMMAND_TIMEOUT',
                    'ocs': b'BAD_RESPONSE', 'restore': b'RESTORE_FAILED', 'short': b'SHORT_TRANSFER',
                    'timeout_restore':b'RESTORE_FAILED', 'timeout_power_off':b'POWER_LOST',
                    'timeout_power_unreadable':b'POWER_UNVERIFIED',
                    'timeout_layer_unreadable':b'COMMAND_TIMEOUT'}[mode]
        assert not callback_errors, callback_errors
        assert result == expected, (mode, result)
        if mode.startswith('timeout'):
            assert state.last_opcode == 0x28 and state.command_lba == 100 and state.command_blocks == 1
            assert state.fault_ocs == 0xf
            assert state.command_ticks >= 9600000
            if mode in ('timeout_power_off', 'timeout_power_unreadable'):
                assert state.fault_valid == (0x80000100 if mode.endswith('off') else 0x80000000)
            else:
                assert state.fault_doorbell == 1 and state.fault_hcs == 3
                assert state.fault_utrl_lo == pa and state.fault_utrl_hi == 0
                assert state.fault_power == 1 << 31
                assert state.fault_valid == (0x800007ff ^ (1 << 5) if mode.endswith('layer_unreadable') else 0x800007ff)
                assert list(state.fault_uec) == [0x80000001 + i if not (i == 2 and mode.endswith('layer_unreadable')) else 0 for i in range(5)]
        assert bool(state.safe) == (mode not in ('guard_after', 'restore', 'timeout_restore', 'timeout_power_off', 'timeout_power_unreadable'))
        assert out.raw == (payload if mode == 'pass' else b'Z' * 4096)
        assert commands == ([] if mode == 'guard_before' else [0x28])
        if mode not in ('restore', 'timeout_restore', 'timeout_power_off', 'timeout_power_unreadable'):
            assert [regs[o] for o in (0x50, 0x54, 0x60, 0x24)] == [0x81000000, 0, 1, 0x1234]
        checks += 1
        if mode == 'pass':
            commands.clear()
            assert read(C.byref(route), C.byref(state), guard, None, 1000, out) == b'LBA_RANGE'
            assert not commands; checks += 1
            for count in (2, 3):
                multi = C.create_string_buffer(count * 4096)
                commands.clear()
                assert read_blocks(C.byref(route), C.byref(state), guard, None, 100, count, multi) == b'READ_PASS'
                assert commands == [0x28] and state.command_blocks == count and state.safe
                assert multi.raw == b''.join(disk[100 + i] for i in range(count))
                assert not callback_errors, callback_errors
                checks += 1
            untouched = C.create_string_buffer(b'Z' * (3 * 4096), 3 * 4096)
            for lba, count, rejection in ((100, 0, b'READ_RANGE'), (100, 4, b'READ_RANGE'),
                                          (998, 3, b'LBA_RANGE'), (0xfffffffe, 3, b'LBA_RANGE')):
                commands.clear()
                state.last = 0x100000000 if lba == 0xfffffffe else 999
                assert read_blocks(C.byref(route), C.byref(state), guard, None, lba, count, untouched) == rejection
                assert not commands and untouched.raw == b'Z' * (3 * 4096)
                checks += 1
            state.last = 999
            for failure, expected_failure in (('short', b'SHORT_TRANSFER'), ('timeout', b'COMMAND_TIMEOUT')):
                active_mode = failure
                commands.clear()
                assert read_blocks(C.byref(route), C.byref(state), guard, None, 100, 3, untouched) == expected_failure
                assert commands == [0x28] and state.command_blocks == 3 and state.safe
                assert untouched.raw == b'Z' * (3 * 4096)
                assert [regs[o] for o in (0x50, 0x54, 0x60, 0x24)] == [0x81000000, 0, 1, 0x1234]
                checks += 1
            # Keep the fifth command pending while CNTVCT moves backward one tick.
            active_mode = 'counter_backstep'
            commands.clear()
            try:
                result = probe(C.byref(route), C.byref(state), guard, None)
            finally:
                tick_hook.value = None
            assert result == b'GPT_PASS', (active_mode, result)
            assert commands == [0xff, 0, 0x9e, 0x28, 0x28]
            assert state.command_lba == 2 and state.command_ticks == 1 and not state.fault_valid
            checks += 1
    for mode, expected, issued in (
        ('pass', b'WRITE_PASS', [0x2a,0x35]),
        ('guard_before', b'DMA_GUARD_BLOCKED', []),
        ('guard_after', b'DMA_GUARD_CHANGED', [0x2a]),
        ('write_timeout', b'COMMAND_TIMEOUT', [0x2a]),
        ('write_ocs', b'BAD_RESPONSE', [0x2a]),
        ('flush_timeout', b'COMMAND_TIMEOUT', [0x2a,0x35]),
        ('flush_ocs', b'BAD_RESPONSE', [0x2a,0x35]),
        ('write_restore', b'RESTORE_FAILED', [0x2a,0x35])):
        active_mode='pass'; commands.clear(); regs[0x50]=0x81000000; regs[0x60]=1
        assert probe(C.byref(route), C.byref(state), guard, None) == b'GPT_PASS'
        before={n:disk[n] for n in (1,2)}
        replacement=C.create_string_buffer(bytes([0xa5])*4096,4096)
        active_mode=mode; commands.clear()
        assert write(C.byref(route),C.byref(state),guard,None,0,replacement)==expected,mode
        assert commands==issued,(mode,commands)
        assert not callback_errors,callback_errors
        assert {n:disk[n] for n in (1,2)}==before
        assert bool(state.safe)==(mode not in ('guard_after','write_restore'))
        checks+=1
    active_mode='pass'; commands.clear(); regs[0x50]=0x81000000; regs[0x60]=1
    assert probe(C.byref(route),C.byref(state),guard,None)==b'GPT_PASS'
    for relative in (100,0xffffffffffffffff):
        commands.clear()
        assert write(C.byref(route),C.byref(state),guard,None,relative,replacement)==b'WRITE_RANGE'
        assert not commands; checks+=1
    for field,value in (('first',5),('blocks',999),('last',4),('block',512)):
        old=getattr(state,field); setattr(state,field,value); commands.clear()
        assert write(C.byref(route),C.byref(state),guard,None,0,replacement)==b'WRITE_RANGE'
        assert not commands; setattr(state,field,old); checks+=1
    for callback,data in ((GUARD(),replacement),(guard,None)):
        commands.clear()
        assert write(C.byref(route),C.byref(state),callback,None,0,data)==b'WRITE_GUARD_REQUIRED'
        assert not commands; checks+=1
    commands.clear()
    assert write(C.byref(route),C.byref(state),guard,None,99,replacement)==b'WRITE_PASS'
    assert commands==[0x2a,0x35] and disk[199]==replacement.raw; checks+=1
    commands.clear()
    assert flush(C.byref(route),C.byref(state),guard,None)==b'FLUSH_PASS'
    assert commands==[0x35]; checks+=1
    commands.clear()
    assert flush(C.byref(route),C.byref(state),GUARD(),None)==b'WRITE_GUARD_REQUIRED'
    assert not commands; checks+=1
    print(f'PASS {checks} native ownership/VA-PA/read/write/flush/restore checks')

if __name__ == '__main__': main()
