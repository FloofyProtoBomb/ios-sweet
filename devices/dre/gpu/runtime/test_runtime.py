#!/usr/bin/env python3
"""Run metadata controls and compile the read-only observer, without a GPU."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def check_ios_object(path: Path):
    raw = path.read_bytes()
    magic, cpu, subtype, kind, count, command_bytes, _, _ = struct.unpack_from("<8I", raw)
    if (magic, cpu, subtype, kind) != (0xFEEDFACF, 0x0100000C, 0, 1):
        raise ValueError("expected a plain-arm64 Mach-O object")
    cursor, platforms = 32, []
    if cursor + command_bytes > len(raw) or count > 128:
        raise ValueError("truncated Mach-O commands")
    for _ in range(count):
        command, size = struct.unpack_from("<2I", raw, cursor)
        if size < 8 or cursor + size > 32 + command_bytes:
            raise ValueError("invalid Mach-O command")
        if command == 0x32:
            platforms.append(struct.unpack_from("<2I", raw, cursor + 8))
        cursor += size
    if cursor != 32 + command_bytes or platforms != [(2, 0x00100000)]:
        raise ValueError("expected explicit iOS 16.0 build version")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    zig = str(args.zig.resolve())
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", str(HERE),
                    "-p", "test_inspect_abi.py", "-v"], check=True)
    binary = args.out / ("test-factory-probe.exe" if sys.platform == "win32" else "test-factory-probe")
    subprocess.run([zig, "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-DDRE_FACTORY_LIBRARY_ONLY", str(HERE / "factory_probe.c"),
        str(HERE / "test_factory_probe.c"), "-o", str(binary)], check=True)
    output = subprocess.check_output([str(binary.resolve())], text=True)
    print(output, end="")
    codec = args.out / ("test-compiler-request.exe" if sys.platform == "win32" else "test-compiler-request")
    subprocess.run([zig, "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        str(HERE / "compiler_request.c"), str(HERE / "test_compiler_request.c"),
        "-o", str(codec)], check=True)
    print(subprocess.check_output([str(codec.resolve())], text=True), end="")
    obj = args.out / "factory_probe.arm64-ios.o"
    # Zig supplies Darwin C headers but no full iPhoneOS SDK. This compiles the
    # complete CLI's imports/types as an iOS object; it does not link/run it.
    headers = args.zig.resolve().parent / "lib/libc/include/any-macos-any"
    subprocess.run([zig, "cc", "-target", "aarch64-ios.16.0",
        "-isystem", str(headers), "-O2", "-Wall", "-Wextra", "-Werror", "-c",
        str(HERE / "factory_probe.c"), "-o", str(obj)], check=True)
    check_ios_object(obj)
    codec_obj = args.out / "compiler_request.arm64-ios.o"
    subprocess.run([zig, "cc", "-target", "aarch64-ios.16.0", "-isystem", str(headers),
        "-O2", "-Wall", "-Wextra", "-Werror", "-c", str(HERE / "compiler_request.c"),
        "-o", str(codec_obj)], check=True)
    check_ios_object(codec_obj)
    from prepare_reference import adapt_extent
    original = 'if(!base || class_getInstanceSize(base)!=536){NBLOG(@"native-metal: unexpected _MTLDevice ABI");return nil;}'
    assert "!=528" in adapt_extent(original)
    for changed in (original.replace("536", "600"), original + original):
        try:
            adapt_extent(changed)
        except ValueError:
            pass
        else:
            raise AssertionError("changed source guard was accepted")
    report = {"status": "PASS", "abi_guard_unit_cases": 11, "metadata_transport_cases": 16,
        "source_guard_cases": 3, "plain_arm64_ios16_object": True,
        "compiler_codec_golden_and_negative_controls": True,
        "iphoneos_sdk_linked": False, "native_discovery_run": False,
        "metal_acceleration_pass": False}
    (args.out / "tests.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
