#!/usr/bin/env python3
"""Retain the DRE kernel, init and identity, substituting QEMU virt's GIC/timer."""
import argparse
from pathlib import Path

from xnu import mkadt, mkpayload


def virtualize(node):
    for i, (key, flags, value) in enumerate(node.props):
        if node.name == "gic" and key == "reg":
            value = mkadt.u64(mkadt.QEMU_GICD - mkadt.SOC_BASE, 0x10000,
                              mkadt.QEMU_GICR - mkadt.SOC_BASE, 0x100000)
        if key == "virtual-timer-intid":
            value = mkadt.u32(mkadt.QEMU_VTIMER_INTID)
        if key in ("timebase-frequency", "fixed-frequency", "peripheral-frequency"):
            value = mkadt.u32(mkadt.QEMU_TIMEBASE)
        node.props[i] = (key, flags, value)
    for child in node.children:
        virtualize(child)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", type=Path, help="build.py output directory")
    ap.add_argument("kernel", type=Path)
    args = ap.parse_args()
    root, _ = mkadt.adt.parse((args.out / "dre.adt").read_bytes())
    virtualize(root)
    adt = args.out / "dre-virt.adt"
    adt.write_bytes(mkadt.adt.serialize(root))
    cmd = (args.out / "payload.bin").read_bytes()[0x48:0x48 + 608].split(b"\0", 1)[0].decode()
    data, _ = mkpayload.build(str(args.kernel), str(adt), cmd, str(args.out / "dre-init.macho"))
    (args.out / "virt-payload.bin").write_bytes(data)
