#!/usr/bin/env python3
"""Read-only, UUID-pinned inspection of the selected iOS Metal device factory.

Requires the local, legitimately obtained plain-arm64 20H392 shared cache and
ipsw. Disassembly is private input: keep --capture outside this repository.
The small JSON report contains observations, never a GPU readiness claim.
No plugin is loaded, process modified, service opened, or GPU work submitted.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import uuid

PROFILE = {
    "product_build": "20H392",
    "cache_uuid": "8F970452-C9E3-3E88-B114-FEB1864BC9E0",
    "metal_uuid": "0F0B7B96-90BF-3A07-B7E1-85E2FB6160E6",
    "iogpu_uuid": "8EB7ED27-E1AF-3C82-825B-130C091D7070",
    "compiler_uuid": "0ACFC872-8027-3DFD-8F30-397C9CB25639",
    "metal_version": "306.6.6",
    "class_address": 0x1E1D88A68,
    "class_ro_address": 0x1D59E51A8,
    "class_instance_size": 528,
}
METAL = "/System/Library/Frameworks/Metal.framework/Metal"
IOGPU = "/System/Library/PrivateFrameworks/IOGPU.framework/IOGPU"
COMPILER = "/System/Library/PrivateFrameworks/MTLCompiler.framework/MTLCompiler"
FUNCTIONS = {
    "factory": (0x181A3C7C8, 76),
    "enumeration": (0x181B144C8, 60),
    "add_device": (0x181A1B368, 80),
    "abstract_initializer": (0x181AAB968, 7),
    "iogpu_create": (0x1ECE1CE38, 200),
    "source_header_producer": (0x181A355EC, 72),
    "source_language_cap": (0x181A35180, 6),
    "source_header_consumer": (0x1BBDAB1B4, 13),
    "archive_reply": (0x1BBDAE714, 96),
}
# Assertions describe only the instruction at the stated address. They do not
# infer a whole ABI from a symbol name or from bytes belonging to the next method.
CALLS = {
    "factory": {
        0x181A3C7EC: 0x186D85EC8,  # IORegistryEntryCreateCFProperty
        0x181A3C820: 0x181BD15C0,  # stringByAppendingPathComponent:
        0x181A3C82C: 0x181BD15E0,  # stringByAppendingString:
        0x181A3C838: 0x181BC47E0,  # bundleWithPath:
        0x181A3C880: 0x181BC48E0,  # classNamed:
        0x181A3C8B4: 0x181BCD6A0,  # principalClass
        0x181A3C8D0: 0x181BCA9A0,  # isSubclassOfClass:
    },
    "enumeration": {
        0x181B14570: 0x181A3C7C8,
        0x181B14580: 0x181BC92E0,  # initWithAcceleratorPort:
        0x181B1458C: 0x181A1B368,
    },
    "add_device": {
        0x181A1B3BC: 0x181BC4FA0,  # conformsToProtocol:
        0x181A1B3D4: 0x181BC4FA0,
        0x181A1B3E4: 0x181BC9200,  # initLimits
        0x181A1B3F0: 0x181BC9180,  # initFeatureQueries
    },
    "iogpu_create": {
        0x1ECE1CE84: 0x1F2F32C78,  # IOServiceOpen
        0x1ECE1CEDC: 0x1F2F32C0C,  # IOConnectCallStructMethod
        0x1ECE1CFC0: 0x1F2F32C0C,
        0x1ECE1D064: 0x1F2F32C0C,
        0x1ECE1D0D0: 0x1F2F32C0C,
    },
}
INSTRUCTION_RE = re.compile(
    r"^0x([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+){4})\s*(.*)$", re.I
)


def instructions(text: str) -> dict[int, tuple[int, str]]:
    result = {}
    for line in text.splitlines():
        found = INSTRUCTION_RE.match(line)
        if not found:
            continue
        address = int(found[1], 16)
        if address in result:
            raise ValueError(f"duplicate instruction at {address:#x}")
        result[address] = (int.from_bytes(bytes.fromhex(found[2]), "little"), found[3])
    return result


def branch_target(address: int, instruction: int) -> int:
    if instruction & 0xFC000000 != 0x94000000:
        raise ValueError(f"instruction at {address:#x} is not BL")
    displacement = instruction & 0x3FFFFFF
    if displacement & 0x2000000:
        displacement -= 0x4000000
    return address + displacement * 4


def require_calls(name: str, text: str) -> None:
    decoded = instructions(text)
    for source, destination in CALLS.get(name, {}).items():
        if source not in decoded or branch_target(source, decoded[source][0]) != destination:
            raise ValueError(f"{name}: call at {source:#x} differs from qualified profile")


def require_tokens(text: str, tokens: tuple[str, ...], what: str) -> None:
    missing = [token for token in tokens if token not in text]
    if missing:
        raise ValueError(f"{what}: missing {missing}")


def verify_class_layout(decoded_class: str, raw_ro: bytes) -> dict:
    if len(raw_ro) != 16:
        raise ValueError("truncated Objective-C class metadata")
    # `dump --addr` applies this cache's slide-chain decoding. `dump --output`
    # alone returns encoded words (including next-link bits), not plain pointers.
    # Do not mask those words to force a desired address.
    fields = decoded_class.splitlines()
    if len(fields) != 5 or any(not re.fullmatch(r"0x[0-9a-f]+", f) for f in fields):
        raise ValueError("expected five decoded Objective-C class addresses")
    if int(fields[4], 16) != PROFILE["class_ro_address"]:
        raise ValueError("unexpected _MTLDevice class_ro pointer")
    flags, start, size, reserved = struct.unpack("<4I", raw_ro)
    if (flags, start, size, reserved) != (4, 8, PROFILE["class_instance_size"], 0):
        raise ValueError("unexpected _MTLDevice class extent/flags")
    return {"name": "_MTLDevice", "instance_start": start, "instance_size": size}


def verify_captures(directory: Path) -> dict:
    metadata = json.loads((directory / "capture.json").read_text(encoding="utf-8"))
    if metadata.get("profile") != PROFILE:
        raise ValueError("capture is not the qualified 20H392 profile")
    for name, digest in metadata["files"].items():
        if Path(name).name != name:
            raise ValueError("invalid capture filename")
        if hashlib.sha256((directory / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f"capture hash mismatch: {name}")
    required_files = {"metal.txt", "iogpu.txt", "compiler.txt", "class-addresses.txt", "class-ro.bin"} | {
        f"{name}.txt" for name in FUNCTIONS
    }
    if set(metadata["files"]) != required_files:
        raise ValueError("missing or unexpected capture files")
    metal = (directory / "metal.txt").read_text(encoding="utf-8")
    iogpu = (directory / "iogpu.txt").read_text(encoding="utf-8")
    compiler = (directory / "compiler.txt").read_text(encoding="utf-8")
    require_tokens(metal, (PROFILE["metal_uuid"], "(306.6.6)",
        '"IOAcceleratorES"', '"MetalPluginName"', '"MetalPluginClassName"',
        '"/System/Library/Extensions"', '@protocol MTLDeviceSPI',
        '0x001dcdc4f70 => 0x001e1d88a68: _MTLDevice',
        '0x001dcdc4a50 => 0x001e1ad0170: MTLDevice',
        '0x001dcdc4a58 => 0x001e1ad01d0: MTLDeviceSPI'), "Metal metadata")
    require_tokens(iogpu, (PROFILE["iogpu_uuid"], '"Metal"',
        '@interface IOGPUMetalDevice : _MTLDevice'), "IOGPU metadata")
    require_tokens(compiler, (PROFILE["compiler_uuid"], '306.6.6.0.0',
        '_MTLCodeGenServiceCreate', '_MTLCodeGenServiceBuildRequest'), "compiler metadata")
    for name in FUNCTIONS:
        require_calls(name, (directory / f"{name}.txt").read_text(encoding="utf-8"))
    abstract = instructions((directory / "abstract_initializer.txt").read_text())
    if abstract.get(0x181AAB978, (0, ""))[0] != 0xD2800000:
        raise ValueError("base initializer no longer explicitly returns nil")
    # Independently checked producer/consumer words, not an assumption that the
    # iOS17 frontend's similarly named private structures describe iOS16.
    compiler_words = {
        "source_header_producer": {0x181A35670: 0x29012013, 0x181A35674: 0x29006818,
                                   0x181A356DC: 0x910042B5},
        "source_header_consumer": {0x1BBDAB1BC: 0x29402528},
        "source_language_cap": {0x181A3518C: 0x52A00069},
        "archive_reply": {0x1BBDAE7F8: 0xD2C00D08, 0x1BBDAE7FC: 0xF8004008,
                          0x1BBDAE800: 0xB9000C13, 0x1BBDAE810: 0x29056C16,
                          0x1BBDAE814: 0x29066814},
    }
    for name, words in compiler_words.items():
        decoded = instructions((directory / f"{name}.txt").read_text())
        for address, word in words.items():
            if decoded.get(address, (None, ""))[0] != word:
                raise ValueError(f"compiler ABI instruction differs at {address:#x}")
    layout = verify_class_layout((directory / "class-addresses.txt").read_text(),
                                (directory / "class-ro.bin").read_bytes())
    return {
        "schema": 1, "status": "QUALIFIED_FACTORY_ABI_OBSERVED", "profile": PROFILE,
        "class": layout,
        "factory": {"service_match": "IOAcceleratorES", "plugin_name_key": "MetalPluginName",
            "plugin_class_key": "MetalPluginClassName",
            "bundle_path": "/System/Library/Extensions/<MetalPluginName>.bundle",
            "class_fallback": "principalClass", "required_base": "_MTLDevice",
            "initializer": "initWithAcceleratorPort:",
            "required_protocols": ["MTLDevice", "MTLDeviceSPI"],
            "registration_methods": ["initLimits", "initFeatureQueries"],
            "base_initializer_returns_nil": True},
        "iogpu": {"api_property": "Metal", "open_type": "1 | ((options & 0xffff) << 16)",
            "initial_calls": [
                {"selector": 6, "input_bytes": 16, "output_bytes": 0,
                 "purpose": "API name", "return_checked_by_original": False},
                {"selector": 2, "input_bytes": 0, "output_bytes": 0x218,
                 "purpose": "mapped shared state metadata", "required_success": True},
                {"selector": 0, "input_bytes": 0, "output_bytes": 0x40,
                 "purpose": "device config", "required_success": True},
                {"selector": 4, "input_bytes": 0, "output_bytes": 0x20,
                 "purpose": "additional communication data", "required_success": False}],
            "full_userclient_abi_qualified": False},
        "capture_sha256": metadata["files"],
        "compiler": {"version": "306.6.6", "source_request_type": 13,
            "source_header_bytes": 16,
            "source_header_u32_fields": ["source_bytes_including_nul", "inline_metadata_bytes",
                                         "argument_bytes_including_nul", "flags"],
            "source_alignment": 4, "source_language_cap": "3.0",
            "archive_reply_header_bytes": 104, "warning_offset_field": 8,
            "warning_length_field": 12, "archive_offset_field": 40,
            "archive_length_field": 44, "debug_offset_field": 48,
            "debug_length_field": 52, "compiler_execution_observed": False},
        "custom_driver_implemented": False, "hardware_execution_observed": False,
        "metal_acceleration_pass": False,
        "limitations": ["Factory ABI observations are not a Metal implementation.",
            "IOGPU selectors do not establish valid shared mappings, resources, queues or fences.",
            "No plugin was loaded, service opened or GPU work submitted."],
    }


def capture(cache: Path, ipsw: Path, directory: Path) -> None:
    with cache.open("rb") as source:
        header = source.read(104)
    if len(header) < 104 or header[:16] != b"dyld_v1   arm64\0":
        raise ValueError("requires the qualified plain-arm64 cache")
    if str(uuid.UUID(bytes=header[88:104])).upper() != PROFILE["cache_uuid"]:
        raise ValueError("shared-cache UUID differs; fixed addresses must not be reused")
    directory.mkdir(parents=True, exist_ok=False)

    def run(name: str, arguments: list[str], binary=False) -> None:
        path = directory / name
        command = [str(ipsw), "dyld", *arguments, "--no-color"]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                check=False, timeout=60)
        if result.returncode or len(result.stdout) > 8 << 20:
            raise ValueError(f"ipsw failed/oversized output for {name}: "
                             + result.stderr.decode("utf-8", "replace")[:600])
        if binary:
            if not path.is_file():
                raise ValueError(f"ipsw did not write {name}")
        else:
            path.write_bytes(result.stdout)

    # --stubs triggers an enormous unsupported stub-island allocation on this
    # cache/tool version. ObjC refs are sufficient and complete without it.
    for name, image in (("metal.txt", METAL), ("iogpu.txt", IOGPU), ("compiler.txt", COMPILER)):
        run(name, ["macho", str(cache), image, "--loads", "--symbols", "--objc",
                   "--objc-refs", "--strings"])
    for name, (address, count) in FUNCTIONS.items():
        run(f"{name}.txt", ["disass", str(cache), "--vaddr", hex(address),
                           "--count", str(count), "--quiet"])
    run("class-addresses.txt", ["dump", str(cache), hex(PROFILE["class_address"]),
                                "--count", "5", "--addr"])
    for name, address, size in (("class-ro.bin", PROFILE["class_ro_address"], 16),):
        run(name, ["dump", str(cache), hex(address), "--size", str(size),
                   "--output", str(directory / name)], binary=True)
    files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in directory.iterdir()}
    (directory / "capture.json").write_text(json.dumps({"profile": PROFILE, "files": files}, indent=2)
                                           + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--ipsw", type=Path)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        if bool(args.cache) != bool(args.ipsw):
            parser.error("--cache and --ipsw must be given together")
        if args.cache:
            capture(args.cache.resolve(), args.ipsw.resolve(), args.capture.resolve())
        result = verify_captures(args.capture)
    except (ValueError, OSError, KeyError, subprocess.TimeoutExpired) as exc:
        result = {"status": "FAIL", "error": str(exc), "metal_acceleration_pass": False}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("status", "metal_acceleration_pass")}))
    return int(result["status"] == "FAIL")


if __name__ == "__main__":
    sys.exit(main())
