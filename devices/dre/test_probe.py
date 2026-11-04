#!/usr/bin/env python3
"""Exercise the actual v3 kernel bytes under Unicorn 2 on a model DT or live FDT."""
import argparse
import gzip
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC

FB = 0x85200000
FB_SIZE = 0xc00000
DT = 0x82000000
INT = lambda value: struct.pack(">I", value)


def fixture(project=133180, base=FB, size=FB_SIZE, compatible=b"qcom,holi\0"):
    strings, body = bytearray(), bytearray()

    def aligned(data):
        return data + bytes(-len(data) % 4)

    def node(name):
        body.extend(INT(1) + aligned(name.encode() + b"\0"))

    def prop(name, value):
        body.extend(INT(3) + INT(len(value)) + INT(len(strings)) + aligned(value))
        strings.extend(name.encode() + b"\0")

    node("")
    prop("compatible", compatible)
    prop("oplus,dtsi_no", INT(project))
    prop("#address-cells", INT(2))
    prop("#size-cells", INT(2))
    node("reserved-memory")
    prop("#address-cells", INT(2))
    prop("#size-cells", INT(2))
    prop("ranges", b"")
    node("splash_region@0x85200000")
    prop("label", b"cont_splash_region\0")
    prop("reg", struct.pack(">QQ", base, size))
    body.extend(INT(2) * 2)
    node("memory")
    prop("device_type", b"memory\0")
    prop("reg", struct.pack(">QQQQ", 0x80000000, 0x80000000,
                            0x100000000, 0x40000000))
    body.extend(INT(2) * 2 + INT(9))
    start = 56
    return (struct.pack(">10I", 0xd00dfeed, start + len(body) + len(strings),
                        start, start + len(body), 40, 17, 16, 0,
                        len(strings), len(body)) + bytes(16) + body + strings)


def execute(kernel, tree, load, visible):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for addr, data in ((load, kernel), (DT, tree)):
        cpu.mem_map(addr, (len(data) + 4095) & -4096)
        cpu.mem_write(addr, bytes(data))
    # On rejecting paths leave framebuffer and *all* MMIO unmapped.
    if visible:
        cpu.mem_map(FB, FB_SIZE)
    cpu.reg_write(UC_ARM64_REG_X0, DT)
    halt = load + kernel.index(bytes.fromhex("5f2003d5"), 64)
    cpu.emu_start(load, halt, count=100_000_000)
    assert cpu.reg_read(UC_ARM64_REG_PC) == halt, "did not return to halt loop"
    if visible:
        assert cpu.mem_read(FB, 4) == struct.pack("<I", 0xff0000a0), "no console"
        # RESULT is drawn in yellow. DRE PROBE OK begins in white at col 8.
        data = bytes(cpu.mem_read(FB, 1080 * 2400 * 4))
        row_bytes = 1080 * 4
        font = (Path(__file__).resolve().parents[2] / "shim/font.h").read_text()
        import re
        glyphs = [bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", line))
                  for line in font.splitlines() if line.lstrip().startswith("{0x")]

        def matches(row):
            for col, ch in enumerate("DRE PROBE OK"):
                glyph = glyphs[ord(ch) - 32]
                for y, bits in enumerate(glyph):
                    for x in range(8):
                        offset = (24 + row * 48 + y * 3) * row_bytes
                        offset += (24 + (8 + col) * 24 + x * 3) * 4
                        color = 0xffffffff if bits & (0x80 >> x) else 0xff0000a0
                        if struct.unpack_from("<I", data, offset)[0] != color:
                            return False
            return True
        assert any(matches(row) for row in range(48)), "missing success text"


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", type=Path)
    ap.add_argument("--dtb", type=Path)
    args = ap.parse_args()
    image = args.image.read_bytes()
    magic, size, rdsize, _, header_size, _, _, _, _, version = struct.unpack_from("<8s9I", image)
    assert (magic, version, header_size) == (b"ANDROID!", 3, 1580)
    rd = 4096 + ((size + 4095) & -4096)
    assert gzip.decompress(image[rd:rd + rdsize]).startswith(b"070701")
    kernel = image[4096:4096 + size]
    assert kernel[56:60] == b"ARM\x64" and struct.unpack_from("<Q", kernel, 16)[0] == size
    for addr in (0x80200000, 0x91000000):
        execute(kernel, fixture(), addr, True)
    for dtb in (fixture(project=1), fixture(base=0x9c000000), fixture(size=4096),
                fixture(compatible=b"qcom,sm7150\0"), bytes(128)):
        execute(kernel, dtb, 0x80200000, False)
    if args.dtb:
        execute(kernel, args.dtb.read_bytes(), 0x80200000, True)
    print("PASS: v3 layout, relocated ARM64 console, board and memory guards"
          + (", supplied DTB" if args.dtb else ""))
