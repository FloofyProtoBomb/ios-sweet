#!/usr/bin/env python3
"""Check RAM exclusion policy and wire-format SPI/USB preparation. No device."""
import argparse
import ctypes as C
import os
from pathlib import Path
import struct
import subprocess

HERE = Path(__file__).resolve().parent

def fixture(*, reserve=(), dynamic=(), memreserve=(), memory=((0x80000000, 0x100000000),), broken=False, initrd=None, owned_pools=(), board=False):
    strings = bytearray()
    def prop(k, v):
        index = len(strings); strings.extend(k.encode() + b'\0')
        return struct.pack('>3I', 3, len(v), index) + v + bytes(-len(v) % 4)
    def node(name, props=b'', children=b''):
        name = name.encode() + b'\0'
        return struct.pack('>I', 1) + name + bytes(-len(name) % 4) + props + children + struct.pack('>I', 2)
    def ranges(values): return b''.join(struct.pack('>QQ', a, n) for a, n in values)
    children = node('memory', prop('device_type', b'memory\0') + prop('reg', ranges(memory)))
    if initrd is not None:
        children += node('chosen', prop('linux,initrd-start', struct.pack('>Q', initrd[0])) +
                         prop('linux,initrd-end', struct.pack('>Q', initrd[1])))
    if board:
        children += node('soc', prop('#address-cells', struct.pack('>I', 1)) +
                         prop('#size-cells', struct.pack('>I', 1)),
                         node('ufshc@4804000', prop('reg', struct.pack('>II', 0x04804000, 0x3000))))
    reservations = b''.join(node(f'fixed{i}', prop('reg', ranges([v]))) for i, v in enumerate(reserve))
    reservations += b''.join(node(f'dynamic{i}', prop('alloc-ranges', ranges([v]))) for i, v in enumerate(dynamic))
    reservations += b''.join(node(f'pool{i}', prop('alloc-ranges', ranges([v])) +
                                  prop('size', struct.pack('>Q', 0x100000)) +
                                  prop('compatible', b'shared-dma-pool\0')) for i, v in enumerate(owned_pools))
    if broken: reservations += node('unknown', prop('size', struct.pack('>Q', 4096)))
    children += node('reserved-memory', prop('#address-cells', struct.pack('>I', 2)) +
                     prop('#size-cells', struct.pack('>I', 2)) + prop('ranges', b''), reservations)
    properties = prop('#address-cells', struct.pack('>I', 2)) + prop('#size-cells', struct.pack('>I', 2))
    if board:
        properties += prop('compatible', b'qcom,holi\0') + prop('oplus,dtsi_no', struct.pack('>I', 133180))
    tree = node('', properties, children) + struct.pack('>I', 9)
    reserved = ranges(memreserve) + bytes(16)
    offset = 40 + len(reserved)
    return struct.pack('>10I', 0xd00dfeed, offset + len(tree) + len(strings), offset,
                       offset + len(tree), 40, 17, 16, 0, len(strings), len(tree)) + reserved + tree + strings

class SPI(C.Structure): _fields_ = [('words', C.c_uint32 * 16)]
class TRB(C.Structure): _fields_ = [('words', C.c_uint32 * 4)]
class Event(C.Structure): _fields_ = [(k, C.c_uint32) for k in ('raw', 'device', 'endpoint', 'type', 'status', 'parameters', 'next')]

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('--zig', default='zig')
    ap.add_argument('--out', type=Path, required=True); ap.add_argument('--dtb', type=Path)
    args = ap.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    guard = args.out / 'reject-write.c'
    guard.write_text('#include <stdlib.h>\n#include <libfdt.h>\nint fdt_appendprop(void *f,int n,const char *k,const void *v,int z)\n{(void)f;(void)n;(void)k;(void)v;(void)z;abort();}\n')
    lib = (args.out / ('prep.dll' if os.name == 'nt' else 'prep.so')).resolve()
    sources = [HERE / 'dma_memory.c', HERE / 'usb/transport.c', HERE / 'touch/spi_plan.c', guard]
    sources += [HERE / 'libfdt' / p for p in ('fdt.c', 'fdt_ro.c', 'fdt_check.c', 'fdt_addresses.c')]
    flags = ['-Wl,--export-all-symbols'] if os.name == 'nt' else ['-fPIC']
    subprocess.run([args.zig, 'cc', '-O2', '-shared', *flags, '-I' + str(HERE / 'libfdt'), *map(str, sources), '-o', str(lib)], check=True)
    api = C.CDLL(str(lib)); checks = 0
    ram = api.dre_dma_ram_allowed; ram.argtypes = [C.c_void_p, C.c_uint64, C.c_uint64]; ram.restype = C.c_int
    def memory(data, a, n, expected):
        global checks
        assert bool(ram(C.create_string_buffer(data), a, n)) == expected, ('RAM', hex(a), n, expected)
        checks += 1
    for start, size, expected in ((0x90000000, 8, True), (0x80000000, 8, True), (0x17ffffff8, 8, True),
                                  (0x17ffffff8, 9, False), (0x70000000, 8, False), (0, 8, False),
                                  (0xc600000, 8, False), (0x90000000, 0, False), ((1 << 64) - 8, 16, False)):
        memory(fixture(), start, size, expected)
    for kind in ('reserve', 'dynamic', 'memreserve'):
        data = fixture(**{kind: [(0x90000000, 4096)]})
        for start, size, expected in ((0x8ffffff8, 8, True), (0x8ffffff8, 9, False),
                                      (0x90000000, 8, False), (0x90000fff, 2, False), (0x90001000, 8, True)):
            memory(data, start, size, expected)
        memory(fixture(**{kind: [((1 << 64) - 8, 16)]}), 0xa8000000, 8, False)
    memory(fixture(broken=True), 0xa8000000, 8, False)
    memory(fixture(memory=[(0x80000000, 0x1000), (0x80002000, 0x1000)]), 0x80000ff8, 16, False)
    if args.dtb:
        data = args.dtb.read_bytes()
        memory(data, 0x110000000, 8, True); memory(data, 0x85200000, 8, False)
        memory(data, 0xc600000, 8, False)
    spi = api.dre_spi_plan; spi.argtypes = [C.POINTER(SPI), C.c_uint64, C.c_uint64, C.c_uint, C.c_uint, C.c_uint, C.c_uint]; spi.restype = C.c_char_p
    value = SPI(); assert spi(C.byref(value), 0xa8001000, 0xa8002000, 65, 0, 2, 4) == b'SPI_PLAN_OK'
    assert list(value.words) == [0x03000004, 0, 0x00020004, 0x00220001, 3, 0, 65, 0x00200801,
                                 0xa8001000, 0, 65, 0x00100200, 0xa8002000, 0, 65, 0x00100000]
    checks += 1
    for mode in range(4):
        for length in (1, 64, 255, 4096):
            assert spi(C.byref(value), 0x1a8001000, 0x1a8003000, length, mode, 7, 4095) == b'SPI_PLAN_OK'
            assert value.words[0] == 0x03000004 | ((mode & 1) * 16 + (mode >> 1) * 32) << 8
            assert value.words[9] == value.words[13] == 1
            checks += 1
    invalid_spi = [(0, 0xa8002000, 65, 0, 2, 4), (0xa8001000, 0xa8001001, 65, 0, 2, 4),
                   (1 << 48, 0xa8002000, 65, 0, 2, 4), ((1 << 48) - 1, 0xa8002000, 65, 0, 2, 4)]
    base = [0xa8001000, 0xa8002000, 65, 0, 2, 4]
    for index, bad in ((2, 0), (2, 4097), (3, 4), (4, 8), (5, 0), (5, 4096)):
        row = base.copy(); row[index] = bad; invalid_spi.append(row)
    for row in invalid_spi:
        assert spi(C.byref(value), *row) != b'SPI_PLAN_OK' and not any(value.words); checks += 1
    trb = api.dre_usb_trb; trb.argtypes = [C.POINTER(TRB), C.c_uint, C.c_uint64, C.c_uint, C.c_uint]; trb.restype = C.c_char_p
    t = TRB()
    for kind, length, address, control in ((2, 8, 0x1a8000040, 0x822), (3, 0, 0, 0x832),
        (4, 0, 0, 0x842), (5, 64, 0x1a8000080, 0x852), (1, 65535, 0x1a8001000, 0x812)):
        assert trb(C.byref(t), kind, address, length, 40) == b'USB_TRB_OK'
        assert list(t.words) == [address & 0xffffffff, address >> 32, length, control] and not t.words[3] & 1
        checks += 1
    for kind, address, length, width in ((0, 0x1000, 8, 40), (8, 0x1000, 0, 40), (2, 0x1000, 7, 40),
        (3, 0x1000, 0, 40), (4, 0, 1, 40), (5, 0x1000, 0, 40), (1, 0x1000, 65536, 40),
        (2, 0, 8, 40), (2, (1 << 32) - 7, 8, 32), (2, 1 << 40, 8, 40), (2, 0x1000, 8, 31), (2, 0x1000, 8, 65)):
        assert trb(C.byref(t), kind, address, length, width) != b'USB_TRB_OK' and not any(t.words); checks += 1
    for address, width in ((1 << 48, 64), (1 << 60, 64), ((1 << 64) - 8, 64), ((1 << 32) - 8, 32)):
        assert trb(C.byref(t), 2, address, 8, width) == b'USB_TRB_OK'
        assert list(t.words) == [address & 0xffffffff, address >> 32, 8, 0x822]
        checks += 1
    for address in ((1 << 64) - 7, (1 << 64) - 1):
        assert trb(C.byref(t), 2, address, 8, 64) == b'USB_BUFFER' and not any(t.words); checks += 1
    decode = api.dre_usb_event; decode.argtypes = [C.c_void_p, C.c_uint, C.c_uint, C.c_uint, C.POINTER(Event)]; decode.restype = C.c_char_p
    for ep in range(32):
        for kind in (1, 2, 3, 7):
            raw = ep << 1 | kind << 6 | 5 << 12 | 0x1234 << 16
            ring = C.create_string_buffer(bytes(4092) + struct.pack('<I', raw)); event = Event()
            assert decode(ring, 4096, 4092, 4, C.byref(event)) == b'USB_EVENT_OK'
            assert (event.device, event.endpoint, event.type, event.status, event.parameters, event.next) == (0, ep, kind, 5, 0x1234, 0)
            checks += 1
    for kind in (0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11):
        event = Event(); ring = C.create_string_buffer(struct.pack('<I', 1 | kind << 8 | 0x1ab << 16) + bytes(12))
        assert decode(ring, 16, 0, 4, C.byref(event)) == b'USB_EVENT_OK'
        assert (event.device, event.type, event.parameters, event.next) == (1, kind, 0x1ab, 4); checks += 1
    for capacity, offset, count in ((0, 0, 4), (12, 0, 4), (8192, 0, 4), (4096, 4096, 4), (4096, 1, 4), (4096, 0, 5), (4096, 0, 4100)):
        event = Event(); assert decode(C.create_string_buffer(4096), capacity, offset, count, C.byref(event)) == b'USB_EVENT_BOUNDS'; checks += 1
    event = Event(); assert decode(C.create_string_buffer(16), 16, 4, 0, C.byref(event)) == b'USB_EVENT_EMPTY' and event.next == 4; checks += 1
    for raw in (3, 0x00001001, 0x02000001, 0x00000440):
        event = Event(); assert decode(C.create_string_buffer(struct.pack('<I', raw) + bytes(12)), 16, 0, 4, C.byref(event)) == b'USB_EVENT_UNKNOWN'; checks += 1
    print(f'PASS {checks} RAM/SPI/USB preparation checks; no hardware submission or enumeration')
