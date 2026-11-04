#!/usr/bin/env python3
"""Retain the DRE kernel, init and identity, substituting QEMU virt's GIC/timer."""
import argparse
from pathlib import Path
import re
import uuid

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
    ap.add_argument("--test-serial", help="explicit synthetic serial for a generic QEMU image")
    ap.add_argument("--qemu-smp", action="store_true", help="adapt CPU affinities to QEMU virt's Aff0 layout")
    args = ap.parse_args()
    root, _ = mkadt.adt.parse((args.out / "dre.adt").read_bytes())
    if args.test_serial:
        if not re.fullmatch(r"[A-Za-z0-9-]{1,63}", args.test_serial):
            ap.error("invalid test serial")
        def replace(node, key, value):
            node.props = [(k, f, value if k == key else v) for k, f, v in node.props]
        serial = (args.test_serial.encode() + b"\0").ljust(64, b"\0")
        replace(root, "serial-number", serial)
        replace(next(c for c in root.children if c.name == "oneplus-hardware"), "serial-number", serial)
        replace(next(c for c in root.children if c.name == "chosen"), "dre,host-uuid",
                uuid.uuid5(uuid.NAMESPACE_URL, "urn:oneplus:dre:" + args.test_serial).bytes)
    elif not root.prop("serial-number").strip(b"\0"):
        ap.error("a generic ADT needs --test-serial in QEMU; the handset detects its real serial")
    virtualize(root)
    if args.qemu_smp:
        kernel = args.kernel.read_bytes()
        if b"dre_qemu_affinity" not in kernel:
            ap.error("kernel lacks the explicit QEMU SMP adapter")
        cpus = next(c for c in root.children if c.name == "cpus")
        for index, cpu in enumerate(cpus.children):
            cpu.props = [(k, f, mkadt.u32(index) if k == "reg" else v) for k, f, v in cpu.props]
    adt = args.out / "dre-virt.adt"
    adt.write_bytes(mkadt.adt.serialize(root))
    cmd = (args.out / "payload.bin").read_bytes()[0x48:0x48 + 608].split(b"\0", 1)[0].decode()
    if args.qemu_smp:
        cmd += " dre_qemu_affinity=1"
    data, _ = mkpayload.build(str(args.kernel), str(adt), cmd, str(args.out / "dre-init.macho"))
    (args.out / "virt-payload.bin").write_bytes(data)
