#!/usr/bin/env python3
"""Exercise the real portable table walker with synthetic ARM descriptors."""
import argparse
import ctypes as C
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent

class Context(C.Structure):
    _fields_ = [(p, C.c_uint32) for p in ('bank', 'cbar', 'cba2r', 'control', 'tcr', 'tcr2', 'ttbr_lo', 'ttbr_hi')] + [('captured', C.c_int)]

class Result(C.Structure):
    _fields_ = [('first_pa', C.c_uint64), ('last_pa', C.c_uint64), ('pages', C.c_uint), ('descriptors', C.c_uint)]

READ = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64, C.POINTER(C.c_uint64))
RW = 1 << 10 | 1 << 6

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zig', default='zig'); ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    lib = (args.out / ('dma-walk.dll' if os.name == 'nt' else 'dma-walk.so')).resolve()
    flags = ['-Wl,--export-all-symbols'] if os.name == 'nt' else ['-fPIC']
    subprocess.run([args.zig, 'cc', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', *flags,
                    str(HERE / 'dma_walk.c'), '-o', str(lib)], check=True)
    walk = C.CDLL(str(lib)).dre_dma_walk
    walk.argtypes = [C.POINTER(Context), C.c_uint64, C.c_uint64, READ, C.c_void_p, C.POINTER(Result)]
    walk.restype = C.c_char_p
    checks = 0
    def case(name, expected, *, bits=32, size=8192, target=0xa8000000, mode='page', edit=None,
             context_edit=None, fault=None, changed=None):
        global checks
        ctx = Context(2, 1 << 16, 1, 1, 64 - bits, 5, 0x90000000, 0, 1)
        tables = {}; address = ctx.ttbr_lo
        start = 4 - ((bits - 12 + 8) // 9)
        if mode == 'block':
            stop = 2
        elif mode == 'gigablock': stop = 1
        else: stop = 3
        for level in range(start, stop):
            index = target >> (12 + 9 * (3 - level)) & 511
            tables[address + index * 8] = address + 4096 | 3
            address += 4096
        if stop < 3:
            shift = 12 + 9 * (3 - stop)
            leaf_address = address + ((target >> shift) & 511) * 8
            tables[leaf_address] = target & - (1 << shift) | RW | 1
        else:
            leaf_address = address + ((target >> 12) & 511) * 8
            for page in range(17): tables[leaf_address + page * 8] = (target & -4096) + page * 4096 | RW | 3
        if edit: edit(tables, leaf_address)
        if context_edit: context_edit(ctx)
        reads = []
        @READ
        def read(_, pa, out):
            reads.append(pa)
            if pa == fault or pa not in tables: return 0
            out[0] = tables[pa] ^ (4096 if pa == changed and reads.count(pa) > 1 else 0)
            return 1
        result = Result(); actual = walk(C.byref(ctx), target, size, read, None, C.byref(result)).decode()
        assert actual == expected, (name, actual, expected)
        assert len(reads) <= 128 and all(pa % 8 == 0 for pa in reads)
        if expected == 'IDENTITY_RW':
            assert result.first_pa == target and result.last_pa == target + size - 1
            assert result.pages == ((target & 4095) + size + 4095) // 4096
        if expected in ('CONTEXT_UNSUPPORTED', 'TCR_UNSUPPORTED', 'ROOT_UNSUPPORTED', 'RANGE_UNSUPPORTED'):
            assert not reads, (name, 'read before input validation')
        checks += 1; print('PASS', name)
    for bits in (25, 32, 36, 39, 40, 48):
        target = 0x1000000 if bits == 25 else 0xa8000000
        case(f'{bits}-bit pages', 'IDENTITY_RW', bits=bits, target=target)
    for mode in ('block', 'gigablock'):
        case(mode, 'IDENTITY_RW', bits=39, mode=mode)
    for offset, size in ((7, 1), (4095, 2), (37, 8192), (0, 65536)):
        case(f'offset {offset} size {size}', 'IDENTITY_RW', target=0xa8000000 + offset, size=size)
    case('second page absent', 'UNMAPPED', edit=lambda t, a: t.__setitem__(a + 8, 0))
    case('read only', 'LEAF_PERMISSION', edit=lambda t, a: t.__setitem__(a, t[a] | 128))
    case('privileged only', 'LEAF_PERMISSION', edit=lambda t, a: t.__setitem__(a, t[a] & ~64))
    case('AF clear', 'LEAF_UNSUPPORTED', edit=lambda t, a: t.__setitem__(a, t[a] & ~(1 << 10)))
    case('translated elsewhere', 'MAPPED_OTHER', edit=lambda t, a: t.__setitem__(a, t[a] + 0x100000))
    case('reserved descriptor', 'DESCRIPTOR_UNSUPPORTED', edit=lambda t, a: t.__setitem__(a, t[a] | (1 << 48)))
    case('reserved type', 'DESCRIPTOR_UNSUPPORTED', edit=lambda t, a: t.__setitem__(a, 2))
    case('misaligned block', 'LEAF_UNSUPPORTED', mode='block', edit=lambda t, a: t.__setitem__(a, t[a] | 4096))
    for bit in (61, 62):
        case(f'table permission {bit}', 'TABLE_PERMISSION', edit=lambda t, a, bit=bit: t.__setitem__(next(iter(t)), t[next(iter(t))] | 1 << bit))
    case('unreadable root', 'TABLE_UNREADABLE', fault=0x90000000 + ((0xa8000000 >> 30) & 511) * 8)
    case('changed root', 'TABLE_CHANGED', changed=0x90000000 + ((0xa8000000 >> 30) & 511) * 8)
    for field, value in (('captured', 0), ('cbar', 0), ('cbar', 3 << 16), ('cba2r', 0), ('control', 0), ('control', 17)):
        case(f'context {field} {value}', 'CONTEXT_UNSUPPORTED', context_edit=lambda c, f=field, v=value: setattr(c, f, v))
    for value in (15, 40, 32 | 128, 32 | 1 << 14, 32 | 2 << 14, 32 | 3 << 14):
        case(f'TCR {value}', 'TCR_UNSUPPORTED', context_edit=lambda c, v=value: setattr(c, 'tcr', v))
    case('IPS unsupported', 'TCR_UNSUPPORTED', context_edit=lambda c: setattr(c, 'tcr2', 6))
    case('unaligned root', 'ROOT_UNSUPPORTED', context_edit=lambda c: setattr(c, 'ttbr_lo', c.ttbr_lo | 8))
    case('root outside PA', 'ROOT_UNSUPPORTED', context_edit=lambda c: (setattr(c, 'ttbr_hi', 1), setattr(c, 'tcr2', 0)))
    for size in (0, 65537, (1 << 64) - 1): case(f'size {size}', 'RANGE_UNSUPPORTED', size=size)
    case('too many pages', 'RANGE_UNSUPPORTED', target=0xa8000001, size=65536)
    case('IOVA outside input space', 'RANGE_UNSUPPORTED', target=0x100000000)
    print(f'PASS {checks} table-walker cases; no device or table writes')
