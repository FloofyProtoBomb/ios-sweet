#!/usr/bin/env python3
"""Check display, CPU affinity and identity metadata; no kernel/phone required."""
import argparse
from pathlib import Path
import struct

import xnu


def integer(node, key):
    value = node.prop(key)
    assert len(value) == 4
    return struct.unpack('<I', value)[0]


def child(node, name):
    return next(c for c in node.children if c.name == name)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    old, _ = xnu.mkadt.adt.parse(xnu.tree())
    assert len(child(old, 'cpus').children) == 1
    assert 'disp0' not in [c.name for c in child(old, 'arm-io').children]
    for count in range(1, 9):
        raw = xnu.tree(ui=True, cpus=count)
        root, used = xnu.mkadt.adt.parse(raw)
        assert used == len(raw)
        cpus = child(root, 'cpus').children
        assert len(cpus) == count
        assert [c.name for c in cpus] == [f'cpu{i}' for i in range(count)]
        assert [integer(c, 'reg') for c in cpus] == [i << 8 for i in range(count)]
        assert [integer(c, 'cluster-core-id') for c in cpus] == list(range(count))
        for core, cpu in enumerate(cpus):
            assert cpu.prop('compatible') == b'qcom,kryo460\0ARM,v8\0'
            assert integer(cpu, 'timebase-frequency') == 19200000
            assert cpu.prop('state') == (b'stopped\0' if core else b'running\0')
        io = child(root, 'arm-io')
        display = child(io, 'disp0')
        assert display.prop('compatible') == b'dre,display\0'
        assert (integer(display, 'canvas-width'), integer(display, 'canvas-height')) == (1125, 2436)
        assert integer(child(io, 'wdt'), 'no-pmu') == 1
        assert child(root, 'sweet-fbcon').prop('reg') == struct.pack('<QQ', 0x85200000, 0xc00000)
        assert root.prop('serial-number') == bytes(64)
        assert child(root, 'chosen').prop('dre,host-uuid') == bytes(16)
        def handles(n):
            for k, _, v in n.props:
                if k == 'AAPL,phandle': yield int.from_bytes(v, 'little')
            for c in n.children: yield from handles(c)
        values = list(handles(root))
        assert len(values) == len(set(values))
    for count in (0, 9):
        try: xnu.tree(ui=True, cpus=count)
        except ValueError: pass
        else: raise AssertionError('invalid CPU count accepted')
    try: xnu.tree(cpus=8)
    except ValueError: pass
    else: raise AssertionError('SMP accepted without the UI/SMP profile')
    fixture = args.out / 'synthetic-product.adt'
    fixture.write_bytes(xnu.mkadt.adt.serialize(xnu.mkadt.node('root', children=[
        xnu.mkadt.node('product', [('product-name', b'iPhone10,3\0'), ('AAPL,phandle', bytes(4))])
    ])))
    root, _ = xnu.mkadt.adt.parse(xnu.tree(ui=True, product_path=fixture))
    assert child(root, 'product').prop('product-name') == b'iPhone10,3\0'
    assert not any(k == 'AAPL,phandle' for k, _, _ in child(root, 'product').props)
    print('PASS display/identity metadata, 1-8 CPU trees, unique phandles, invalid-count rejection and product import')
