#!/usr/bin/env python3
"""Bounded private evidence recipe for the selected Metal306 base.

This is NOT a device initializer or qualification program. On a future explicitly
authorized investigation it collects missing instruction ranges into a new
private directory outside this repository. It neither publishes nor executes
Apple code. No source generation, compiler, GPU or service operation is included.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import uuid

# Facts from the existing pinned factory captures/inventoried next symbols.
# End boundaries delimit collection only, not proof of complete control flow.
CACHE_UUID = "8F970452-C9E3-3E88-B114-FEB1864BC9E0"
METAL_UUID = "0F0B7B96-90BF-3A07-B7E1-85E2FB6160E6"
METAL_PATH = "/System/Library/Frameworks/Metal.framework/Metal"
RANGES = {
    "base_init": (0x181A3634C, 0x181A364C8),
    "init_limits": (0x181A1B4BC, 0x181A1BD38),
    "init_feature_queries": (0x181A1BD38, 0x181A1BD70),
    "feature_queries_init_device": (0x181A1C070, 0x181A1E9B4),
    "init_gpu_family_support": (0x181A1EA28, 0x181A1FC18),
    "init_serialization_paths": (0x181A1A3A4, 0x181A1A49C),
    "cxx_construct": (0x181A47964, 0x181A47970),
    "library_builder_init": (0x181A39DF4, 0x181A39E9C),
}
LINE = re.compile(r"^0x([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+){4})", re.I)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def existing_profile(directory: Path) -> dict:
    metadata = json.loads((directory / "capture.json").read_text(encoding="utf-8"))
    profile = metadata.get("profile", {})
    if (profile.get("cache_uuid") != CACHE_UUID or profile.get("metal_uuid") != METAL_UUID
            or profile.get("product_build") != "20H392" or profile.get("class_instance_size") != 528):
        raise ValueError("existing capture is not the exact selected20H392 base profile")
    files = metadata.get("files")
    if not isinstance(files, dict) or not 1 <= len(files) <= 64 or "metal.txt" not in files:
        raise ValueError("existing capture file manifest is absent/unbounded")
    for name, expected in files.items():
        if (not isinstance(name, str) or Path(name).name != name or "/" in name or "\\" in name
                or not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected)):
            raise ValueError("invalid private capture manifest entry")
        path = directory / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 8 << 20:
            raise ValueError("invalid or oversized existing capture")
        if digest(path) != expected:
            raise ValueError("existing private capture changed")
    return profile


def run_private(tool: Path, arguments: list[str], output: Path) -> None:
    # File-backed output avoids accumulating inspector output in memory.
    # --quiet is mandatory below: annotated stub-island discovery previously
    # tried a huge allocation on this cache/tool and is deliberately excluded.
    stderr = output.with_suffix(".stderr")
    with output.open("xb") as out, stderr.open("xb") as err:
        process = subprocess.Popen([str(tool), "dyld", *arguments, "--no-color"],
                                   stdin=subprocess.DEVNULL, stdout=out, stderr=err)
        try:
            status = process.wait(timeout=90)
        except BaseException:
            process.kill()
            process.wait()
            raise
    if status or output.stat().st_size > 8 << 20 or stderr.stat().st_size > 256 << 10:
        raise ValueError("private inspector failed or exceeded bounded output; inspect private logs")


def instruction_observations(path: Path, start: int, end: int) -> dict:
    instructions: dict[int, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = LINE.match(line)
        if not match:
            continue
        address = int(match[1], 16)
        if address in instructions or not start <= address < end or (address - start) % 4:
            raise ValueError("duplicate/out-of-range private instruction output")
        instructions[address] = int.from_bytes(bytes.fromhex(match[2]), "little")
    if len(instructions) != (end - start) // 4:
        raise ValueError("private instruction collection is incomplete")
    calls = []
    for address, word in sorted(instructions.items()):
        if word & 0xFC000000 == 0x94000000:
            displacement = word & 0x3FFFFFF
            if displacement & 0x2000000:
                displacement -= 0x4000000
            calls.append({"instruction": hex(address), "destination": hex(address + displacement * 4)})
    return {"start": hex(start), "end": hex(end), "instruction_count": len(instructions),
            "direct_bl_observations": calls,
            "indirect_calls_selectors_callees_and_cleanup_qualified": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--ipsw", type=Path, required=True)
    parser.add_argument("--existing-capture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    try:
        # Never export Apple captures into git or a checked-in artifact path.
        repository = Path(__file__).resolve().parents[5]
        if out == repository or repository in out.parents or out.exists():
            raise ValueError("output must be a NEW private directory outside the repository")
        cache, tool, capture = args.cache.resolve(), args.ipsw.resolve(), args.existing_capture.resolve()
        profile = existing_profile(capture)
        with cache.open("rb") as stream:
            header = stream.read(104)
        if (len(header) != 104 or header[:16] != b"dyld_v1   arm64\0"
                or str(uuid.UUID(bytes=header[88:104])).upper() != CACHE_UUID):
            raise ValueError("wrong cache: inventoried instruction addresses must not be reused")
        if not tool.is_file():
            raise ValueError("provide the existing independently verified official inspector binary")
        out.mkdir(parents=True, exist_ok=False)
        report = {"schema": 1, "status": "PRIVATE_EVIDENCE_COLLECTION_ONLY",
                  "profile": profile, "tool_sha256_observed": digest(tool),
                  "existing_capture_manifest_sha256": digest(capture / "capture.json"),
                  "ranges": {}, "files": {}, "base_initializer_semantics_qualified": False,
                  "private_tables_qualified": False, "device_constructed": False,
                  "metal_acceleration_pass": False}
        # Recollect selected metadata so private code references have one exact
        # image context. No --stubs, recursive discovery or generated C headers.
        metadata = out / "metal-metadata.txt"
        run_private(tool, ["macho", str(cache), METAL_PATH, "--loads", "--symbols",
                           "--objc", "--objc-refs"], metadata)
        if METAL_UUID not in metadata.read_text(encoding="utf-8"):
            raise ValueError("selected Metal UUID absent from private metadata")
        for name, (start, end) in RANGES.items():
            if not 0 < end - start <= 16 << 10 or (end - start) % 4:
                raise ValueError("invalid bounded source range")
            destination = out / (name + ".txt")
            run_private(tool, ["disass", str(cache), "--vaddr", hex(start),
                               "--count", str((end - start) // 4), "--quiet"], destination)
            report["ranges"][name] = instruction_observations(destination, start, end)
        report["files"] = {p.name: digest(p) for p in out.iterdir() if p.is_file()}
        temporary = out / "collection.json.pending"
        temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        temporary.replace(out / "collection.json")
        print("Collected private observations only; initializer/callee/table semantics remain unqualified.")
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("Base evidence collection refused: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
