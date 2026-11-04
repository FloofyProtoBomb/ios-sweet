#!/usr/bin/env python3
"""Validate Holi CPU/PSCI metadata with synthetic FDTs; never start a CPU."""
import argparse
import ctypes as C
import os
from pathlib import Path
import struct
import subprocess

HERE = Path(__file__).resolve().parent


def fixture(cells=2, ids=range(8), method=b'smc\0', enable=b'psci\0', compat=b'qcom,kryo\0', status=None):
    names = bytearray()
    def prop(name, value):
        key = len(names); names.extend(name.encode() + b'\0')
        return struct.pack('>3I', 3, len(value), key) + value + bytes(-len(value) % 4)
    def node(name, properties=b'', children=b''):
        raw = name.encode() + b'\0'
        return struct.pack('>I', 1) + raw + bytes(-len(raw) % 4) + properties + children + struct.pack('>I', 2)
    children = b''
    for index in ids:
        children += node(f'cpu@{index << 8:x}', prop('device_type', b'cpu\0') +
                         prop('compatible', compat) + prop('enable-method', enable) +
                         prop('reg', (index << 8).to_bytes(cells * 4, 'big')) +
                         (prop('status', status) if status is not None else b''))
    tree = node('', children=node('cpus', prop('#address-cells', struct.pack('>I', cells)) +
                   prop('#size-cells', bytes(4)), children) + node('soc', children=node('psci',
                   prop('compatible', b'arm,psci-1.0\0') + prop('method', method)))) + struct.pack('>I', 9)
    return struct.pack('>10I', 0xd00dfeed, 56 + len(tree) + len(names), 56, 56 + len(tree),
                       40, 17, 16, 0, len(names), len(tree)) + bytes(16) + tree + names


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zig', default='zig')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--dtb', type=Path, help='optional private handset FDT')
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    library = (args.out / ('cpu-topology.dll' if os.name == 'nt' else 'cpu-topology.so')).resolve()
    flags = ['-Wl,--export-all-symbols'] if os.name == 'nt' else ['-fPIC']
    sources = [HERE / 'cpu_topology.c'] + [HERE / 'libfdt' / p for p in
                  ('fdt.c', 'fdt_ro.c', 'fdt_check.c', 'fdt_addresses.c')]
    # fdt_addresses also contains an unused writer. A host DLL exports that
    # function too, unlike the board link's section GC. Abort if a metadata
    # test ever accidentally reaches the writer; never provide fake success.
    guard = args.out / 'reject-fdt-write.c'
    guard.write_text('#include <stdlib.h>\n#include <libfdt.h>\n'
        'int fdt_appendprop(void *f, int n, const char *k, const void *v, int len)\n'
        '{ (void)f; (void)n; (void)k; (void)v; (void)len; abort(); }\n')
    sources.append(guard)
    subprocess.run([args.zig, 'cc', '-O2', '-shared', *flags, '-I' + str(HERE / 'libfdt'),
                    *map(str, sources), '-o', str(library)], check=True)
    subprocess.run([args.zig, 'cc', '-target', 'aarch64-linux-musl', '-O2', '-ffreestanding',
                    '-Wall', '-Wextra', '-Werror', '-I' + str(HERE / 'libfdt'), '-c',
                    str(HERE / 'cpu_topology.c'), '-o', str(args.out / 'cpu-topology-arm64.o')], check=True)
    check = C.CDLL(str(library)).dre_cpu_topology
    check.argtypes = [C.c_void_p, C.c_uint]; check.restype = C.c_char_p
    count = 0
    def call(data, selected, expected):
        global count
        result = check(C.create_string_buffer(bytes(data)), selected)
        assert result == expected, (result, expected)
        count += 1
    for cells in (1, 2):
        for cpus in range(1, 9): call(fixture(cells=cells), cpus, None)
    for cpus in (0, 9, 0xffffffff): call(fixture(), cpus, b'unsupported CPU count')
    call(fixture(method=b'hvc\0'), 8, b'PSCI method mismatch')
    call(fixture(enable=b'spin-table\0'), 8, b'CPU enable method mismatch')
    call(fixture(enable=b'psci\0junk\0'), 8, b'CPU enable method mismatch')
    call(fixture(compat=b'arm,cortex-a55\0'), 8, b'CPU enable method mismatch')
    call(fixture(ids=range(7)), 8, b'incomplete eight-core topology')
    call(fixture(ids=range(9)), 8, b'CPU affinity mismatch')
    # Distinct node names are not required for this malformed duplicate-reg test.
    call(fixture(ids=[0, 1, 2, 3, 4, 5, 6, 6]), 8, b'CPU affinity mismatch')
    call(fixture(status=b'okay\0'), 8, None)
    call(fixture(status=b'ok\0'), 8, None)
    call(fixture(status=b'disabled\0'), 8, b'selected CPU disabled')
    if args.dtb:
        for cpus in (1, 8): call(args.dtb.read_bytes(), cpus, None)
    print(f'PASS {count} CPU topology checks; metadata only, no PSCI calls')
