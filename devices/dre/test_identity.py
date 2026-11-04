#!/usr/bin/env python3
"""Test runtime serial/UUID discovery and bounded ADT edits with synthetic data."""
import argparse
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools"))
import adt


def fdt(args=None, root_serial=None):
    strings, body = bytearray(), bytearray()
    u32 = lambda x: struct.pack(">I", x)
    align = lambda b: b + bytes(-len(b) % 4)

    def node(name):
        body.extend(u32(1) + align(name.encode() + b"\0"))

    def prop(name, value):
        body.extend(u32(3) + u32(len(value)) + u32(len(strings)) + align(value))
        strings.extend(name.encode() + b"\0")

    node("")
    if root_serial is not None:
        prop("serial-number", root_serial)
    node("chosen")
    if args is not None:
        prop("bootargs", args)
    body.extend(u32(2) + u32(2) + u32(9))
    return (struct.pack(">10I", 0xd00dfeed, 56 + len(body) + len(strings),
                        56, 56 + len(body), 40, 17, 16, 0, len(strings), len(body))
            + bytes(16) + body + strings)


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [("name", 0, name.encode() + b"\0"), *props]
    n.children = list(children)
    return n


def slots():
    return node("device-tree", [("serial-number", 0, bytes(64))], [
        node("chosen", [("dre,host-uuid", 0, bytes(16)), ("unchanged", 0, b"chosen-marker")]),
        node("oneplus-hardware", [("serial-number", 0, bytes(64)),
                                  ("imei", 0, b"fixture-preserve-modem-value\0")]),
        node("unrelated", [("marker", 0, b"unrelated-marker")])])


def run(library):
    library.dre_identity_detect.argtypes = [ctypes.c_void_p]
    library.dre_identity_detect.restype = ctypes.c_char_p
    library.dre_identity_serial.restype = ctypes.c_char_p
    library.dre_identity_fixup.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    library.dre_identity_fixup.restype = ctypes.c_char_p
    checked = 0

    def detect(blob, ok):
        nonlocal checked
        error = library.dre_identity_detect(ctypes.create_string_buffer(blob))
        assert (error is None) == ok, error
        checked += 1

    def fixup(blob, ok):
        nonlocal checked
        buf = ctypes.create_string_buffer(blob)
        error = library.dre_identity_fixup(buf, len(blob))
        assert (error is None) == ok, error
        if not ok:
            assert buf.raw[:len(blob)] == blob, "failed validation changed the ADT"
        checked += 1
        return buf.raw[:len(blob)]

    for value in ("A", "DE2118-TEST-A", "B" * 23, "C" * 24, "D" * 63, "aZ09-N200"):
        encoded = value.encode()
        for blob in (fdt(b"console=tty0 androidboot.serialno=" + encoded + b" other=1\0"),
                     fdt(root_serial=encoded + b"\0")):
            detect(blob, True)
            assert library.dre_identity_serial() == encoded
            tree = slots()
            actual = fixup(adt.serialize(tree), True)
            tree.props[1] = ("serial-number", 0, (encoded + b"\0").ljust(64, b"\0"))
            tree.children[0].props[1] = ("dre,host-uuid", 0,
                uuid.uuid5(uuid.NAMESPACE_URL, "urn:oneplus:dre:" + value).bytes)
            tree.children[1].props[1] = tree.props[1]
            assert actual == adt.serialize(tree), "identity mismatch or unrelated ADT mutation"
            # Detecting a new phone must replace all previous identity bytes.
            assert fixup(adt.serialize(slots()), True) == actual
    for blob in (fdt(), fdt(b"console=tty0\0"), fdt(b"androidboot.serialno=\0"),
                 fdt(b"androidboot.serialno=" + b"A" * 64 + b"\0"),
                 fdt(b"androidboot.serialno=bad/value\0"),
                 fdt(b"androidboot.serialno=bad_value\0"),
                 fdt(b"androidboot.serialno=ONE androidboot.serialno=TWO\0"),
                 fdt(b"androidboot.serialno=ONE androidboot.serialno=ONE\0"),
                 fdt(b"androidboot.serialno=NO-TERMINATOR"),
                 fdt(b"xandroidboot.serialno=WRONG\0"),
                 fdt(b"androidboot.serialno_suffix=WRONG\0"),
                 fdt(root_serial=b"NO-TERMINATOR"), fdt(root_serial=b"bad/value\0")):
        detect(blob, False)
        assert library.dre_identity_serial() is None
        fixup(adt.serialize(slots()), False)
    detect(fdt(b"androidboot.serialno=DE2118-FIXTURE\0"), True)
    tree = slots()
    tree.props[1] = ("serial-number", 0, bytes(63))
    fixup(adt.serialize(tree), False)
    tree = slots()
    tree.children[0].props = tree.children[0].props[:1]
    fixup(adt.serialize(tree), False)
    tree = slots()
    tree.children[0].props.append(tree.children[0].props[1])
    fixup(adt.serialize(tree), False)
    tree = slots()
    child = tree.children[-1]
    for i in range(17):
        child.children = [node("deep")]
        child = child.children[0]
    fixup(adt.serialize(tree), False)
    valid = adt.serialize(slots())
    for blob in (valid[:-1], valid + bytes(4), b"", bytes(7),
                 struct.pack("<II", 0xffffffff, 0) + valid[8:]):
        fixup(blob, False)
    bad = bytearray(valid)
    bad[8:40] = b"X" * 32
    fixup(bytes(bad), False)
    print(f"PASS {checked} runtime identity checks using synthetic serials; no device reads")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--zig", default="zig")
    ap.add_argument("--out", type=Path, default=HERE / "out/identity-test")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    lib = (args.out / ("identity-test.dll" if os.name == "nt" else "identity-test.so")).resolve()
    sources = [HERE / "identity.c", *[HERE / "libfdt" / name for name in
               ("fdt.c", "fdt_ro.c")]]
    cmd = [args.zig, "cc", "-shared", "-O2", "-I" + str(HERE / "libfdt")]
    if os.name == "nt":
        cmd += ["-Wl,--export-all-symbols"]
    subprocess.run([*cmd, *map(str, sources), "-o", str(lib)], check=True)
    run(ctypes.CDLL(str(lib)))
