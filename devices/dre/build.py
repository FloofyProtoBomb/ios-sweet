#!/usr/bin/env python3
"""Build the DE2118 temporary boot image. Requires Zig 0.14.1."""
import argparse
import gzip
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import uuid

HERE = Path(__file__).resolve().parent
SHIM = HERE.parent.parent / "shim"
PAGE = 4096


def pad(data):
    return data + bytes(-len(data) % PAGE)


def boot_v3(kernel):
    # A valid empty gzip/newc generic ramdisk. ABL supplies the active
    # slot's vendor ramdisk, DTB, and overlay separately for header v3.
    trailer = b"TRAILER!!!\0"
    fields = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(trailer), 0]
    entry = b"070701" + b"".join(b"%08x" % v for v in fields) + trailer
    ramdisk = gzip.compress(entry.ljust(512, b"\0"), mtime=0)
    # The connected DE2118's LineageOS 22.2 boot_b header, Android 15.0.0,
    # July 2025 patch level, empty generic cmdline. Keep vendor_boot in place.
    os_version = (15 << 25) | (25 << 4) | 7
    header = struct.pack("<8s9I1536s", b"ANDROID!", len(kernel),
                         len(ramdisk), os_version, 1580, 0, 0, 0, 0, 3, b"")
    return pad(header) + pad(kernel) + pad(ramdisk)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--zig", default="zig")
    ap.add_argument("--out", type=Path, default=HERE / "out")
    kernels = ap.add_mutually_exclusive_group()
    kernels.add_argument("--xnu", type=Path, help="embed a kernel built with MACHINE_CONFIG=DRE")
    kernels.add_argument("--upstream-0027", type=Path, help="retarget the pinned upstream CI kernel")
    ap.add_argument("--identity", type=Path, help="optional private JSON with a serial expectation and/or IMEI")
    ap.add_argument("--board-tests", action="store_true", help="survey UFS power/link and live DT at handoff")
    ap.add_argument("--ufs-clock-enable", action="store_true",
                    help="enable UFS GCC branches with a bounded wait and rollback")
    args = ap.parse_args()
    if (args.identity or args.board_tests) and not (args.xnu or args.upstream_0027):
        ap.error("--identity and --board-tests need an XNU kernel")
    if args.ufs_clock_enable and not args.board_tests:
        ap.error("--ufs-clock-enable requires --board-tests")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    zig = os.path.abspath(args.zig) if Path(args.zig).is_file() else args.zig
    cc = [zig, "cc", "-target", "aarch64-linux-musl", "-mcpu=generic",
          "-O2", "-ffreestanding", "-fno-pic", "-fno-pie", "-fno-builtin",
          "-fno-stack-protector", "-mgeneral-regs-only", "-mstrict-align",
          "-ffunction-sections", "-fdata-sections", "-fvisibility=hidden",
          "-DFB_BASE=0", "-I" + str(HERE / "libfdt"), "-I" + str(SHIM)]
    sources = [HERE / name for name in ("start.S", "dre.c", "string.c")]
    sources += [SHIM / "console.c"]
    sources += [HERE / "libfdt" / name for name in
                ("fdt.c", "fdt_ro.c", "fdt_check.c", "fdt_addresses.c")]
    kernel = args.xnu or args.upstream_0027
    if kernel:
        from xnu import embed
        # mockfs exposes this project-built Mach-O as /sbin/launchd on md0.
        # No phone storage controller is linked into the 0027 kernel.
        init_obj = out / "init.o"
        init_flags = ["-DDRE_CHECK_IDENTIFIERS"]
        if args.board_tests:
            cc += ["-DDRE_BOARD_TESTS"]
            init_flags += ["-DDRE_BOARD_TESTS"]
            sources += [HERE / name for name in ("board_tests.c", "guarded_read.S",
                                                "ufs_clock.c", "peripheral_state.c")]
            if args.ufs_clock_enable:
                cc += ["-DDRE_UFS_CLOCK_ENABLE"]
        serial = json.loads(args.identity.read_text()).get("serial") if args.identity else None
        if serial is not None:
            if not re.fullmatch(r"[A-Za-z0-9-]{1,63}", serial):
                raise ValueError("invalid hardware serial")
            host_id = uuid.uuid5(uuid.NAMESPACE_URL, "urn:oneplus:dre:" + serial)
            init_flags.append("-DDRE_EXPECT_UUID=" + ",".join(map(str, host_id.bytes)))
            if args.board_tests:
                header = out / "dre_serial.h"
                header.write_text('#define DRE_EXPECT_SERIAL "' + serial + '"\n', encoding="ascii")
                cc += ["-include", str(header)]
        subprocess.run([zig, "cc", "-target", "aarch64-macos-none", "-g0",
                        '-DINIT_BOARD="DRE"', *init_flags, "-c", str(SHIM.parent / "tools/sweet-init.S"),
                        "-o", str(init_obj)], check=True)
        embed(kernel, out, init_obj, donor=args.upstream_0027 is not None, identity_path=args.identity)
        source = out / "embedded.S"
        payload = (out / "payload.bin").read_bytes()
        # Zig's assembly cache does not track the file named by .incbin.
        digest = hashlib.sha256(payload).hexdigest()
        source.write_text(f'/* {digest} */\n.section .dre_payload,"a"\n.incbin "payload.bin"\n',
                          encoding="ascii")
        sources += [source, HERE / "handoff.c", HERE / "identity.c", SHIM / "loader.c"]
        cc += ["-DDRE_XNU"]
    objects = []
    for source in sources:
        obj = out / (source.stem + ".o")
        subprocess.run(cc + ["-c", str(source), "-o", str(obj)], cwd=out, check=True)
        objects.append(str(obj))
    elf = out / "dre.elf"
    subprocess.run(cc + ["-static", "-nostdlib", "-Wl,-e,_head", "-Wl,--gc-sections",
                         "-Wl,--build-id=none", "-T", str(HERE / "image.ld"),
                         *objects, "-o", str(elf)], check=True)
    image = out / "Image"
    subprocess.run([zig, "objcopy", "-O", "binary", str(elf), str(image)], check=True)
    code = image.read_bytes()
    if kernel and not code.endswith(payload):
        raise ValueError("linked image does not contain the freshly generated payload")
    image_size, = struct.unpack_from("<Q", code, 16)
    if code[56:60] != b"ARM\x64" or not len(code) <= image_size <= 95 * 1024**2:
        raise ValueError("invalid Linux arm64 Image header")
    code = code.ljust(image_size, b"\0")
    image.write_bytes(code)
    output = out / ("dre-xnu-boot.img" if kernel else "dre-probe-boot.img")
    output.write_bytes(boot_v3(code))
    print(output)


if __name__ == "__main__":
    main()
