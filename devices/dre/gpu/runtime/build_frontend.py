#!/usr/bin/env python3
"""Compile actual adapted Metal306 frontend TUs with the official iPhoneOS SDK.

Outputs are objects and provenance only. No plugin/executable is linked or
published. Public source inputs are pinned; Apple captures are neither inputs
nor outputs. --prepare-only permits source checks on a non-macOS host.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from prepare_reference import HERE, prepare
from test_runtime import check_ios_object


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def command(args, out: Path, name: str, *, timeout=180) -> str:
    args = [str(x) for x in args]
    log = out / f"{name}.log"
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    with log.open("w", encoding="utf-8") as stream:
        stream.write(json.dumps(args) + "\n")
        stream.flush()
        try:
            result = subprocess.run(args, env=env, stdout=stream,
                stderr=subprocess.STDOUT, timeout=timeout, check=False)
        except subprocess.TimeoutExpired:
            stream.write(f"\nTIMEOUT after {timeout}s\n")
            raise ValueError(f"{name} timed out; see {log.name}")
    text = log.read_text(encoding="utf-8", errors="replace")
    with (out / "build.log").open("a", encoding="utf-8") as combined:
        combined.write(text + f"\nexit={result.returncode}\n")
    if result.returncode:
        # The full artifact log remains available; console output stays bounded.
        errors = [line for line in text.splitlines() if "error:" in line or "fatal:" in line]
        print("\n".join((errors or text.splitlines())[:35])[:7000], file=sys.stderr)
        raise ValueError(f"{name} exited {result.returncode}; see {log.name}")
    return "\n".join(text.splitlines()[1:])


def fetch_frontend(out: Path, pin: dict) -> Path:
    source = out / "sources/AdrenoMetal"
    source.parent.mkdir(parents=True)
    command(["git", "init", source], out, "git-init")
    command(["git", "-C", source, "config", "core.autocrlf", "false"], out, "git-line-endings")
    command(["git", "-C", source, "remote", "add", "origin", pin["repository"]], out, "git-origin")
    command(["git", "-C", source, "fetch", "--depth", "1", "origin", pin["commit"]], out, "git-fetch")
    command(["git", "-C", source, "checkout", "--detach", "FETCH_HEAD"], out, "git-checkout")
    return source


def stage_inputs(source: Path, out: Path, inputs: dict) -> dict:
    staged = out / "prepared"
    readiness = prepare(source, None, staged, profile_only=True)
    assets = {}
    for name, digest in inputs["frontend_assets"].items():
        raw = (source / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError(f"matching bridge source changed: {name}")
        destination = staged / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
        assets[name] = digest
    patches = []
    for entry in inputs["bridge_patches"]:
        path = source / entry["path"]
        if sha(path) != entry["sha256"] or path.stat().st_size != entry["bytes"]:
            raise ValueError(f"matching bridge patch changed: {entry['path']}")
        destination = out / "runtime-inputs" / entry["path"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
        patches.append(entry)
    frontend = (staged / "NativeBridgeMetalFrontend.mm").read_text(encoding="utf-8")
    bridge = (staged / "bridge/NativeMetalObjects.rs").read_text(encoding="utf-8")
    if "gpuCoreCount {return 8;}" in frontend or "familyForCaller" in frontend or "TARGET_FAMILIES.iter().any" in bridge:
        raise ValueError("an unqualified A740 count/family policy survived adaptation")
    if 'signatureWithObjCTypes:"@@:"' in frontend:
        raise ValueError("fabricated unknown-method ABI survived adaptation")
    return {
        "qualified_source_profile": readiness["profile"], "private_cache_rechecked": False,
        "frontend_repository": json.loads((HERE / "upstream.json").read_text()),
        "prepared_source_sha256": {str(p.relative_to(staged)).replace(os.sep, "/"): sha(p)
            for p in sorted(staged.rglob("*")) if p.is_file()},
        "matching_bridge_assets": assets, "matching_bridge_patches": patches,
        "matching_dependency_inputs": inputs,
        "third_party_license_text": (source / "LICENSE").read_text(encoding="utf-8"),
        "turnip_sources_fetched": False, "rust_bridge_compiled": False,
        "rust_lockfile_qualified": False, "mesa_darwin_transport_qualified_for_dre": False,
        "publication_ready": False, "metal_acceleration_pass": False,
    }


def official_sdk_compile(out: Path, provenance: dict) -> dict:
    if sys.platform != "darwin":
        raise ValueError("official iPhoneOS SDK compilation requires a macOS Xcode runner")
    sdk = command(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], out, "sdk-path").strip()
    clang = command(["xcrun", "--sdk", "iphoneos", "--find", "clang++"], out, "clang-path").strip()
    profile = {
        "sdk_path": sdk,
        "sdk_build": command(["xcrun", "--sdk", "iphoneos", "--show-sdk-build-version"], out, "sdk-build").strip(),
        "compiler": command([clang, "--version"], out, "clang-version").strip(),
        "target": "arm64-apple-ios16.0", "private_headers_used": False,
        "availability_runtime_qualified": False,
    }
    write_json(out / "sdk-profile.json", profile)
    objects = out / "objects"
    objects.mkdir()
    staged = out / "prepared"
    common = ["-target", profile["target"], "-isysroot", sdk, "-O2", "-g", "-Wall", "-Wextra", "-Werror"]
    frontend = objects / "NativeBridgeMetalFrontend.o"
    command([clang, *common, "-std=c++17", "-fobjc-arc", "-fblocks",
        "-DNATIVE_GPU_TRACE_OPTIONAL=1", "-Wno-deprecated-declarations",
        "-Wno-unguarded-availability-new", "-c", staged / "NativeBridgeMetalFrontend.mm", "-o", frontend],
        out, "frontend-compile")
    codec = objects / "compiler_request.o"
    command(["xcrun", "--sdk", "iphoneos", "clang", *common, "-std=c11", "-c",
             staged / "compiler_request.c", "-o", codec], out, "codec-compile")
    for path in (frontend, codec):
        check_ios_object(path)
    imports = command(["xcrun", "nm", "-u", frontend], out, "frontend-imports")
    (objects / "frontend-imports.txt").write_text(imports + "\n", encoding="utf-8")
    names = {line.split()[-1] for line in imports.splitlines() if line.split()}
    bridge_imports = sorted(name[1:] for name in names if name.startswith("_nmb_"))
    rust = "\n".join((staged / name).read_text(encoding="utf-8") for name in (
        "bridge/NativeMetalObjects.rs", "bridge/NativeMetalBridge.rs"))
    exports = set(re.findall(r'pub\s+(?:unsafe\s+)?extern\s+"C"\s+fn\s+(\w+)', rust))
    absent = sorted(set(bridge_imports) - exports)
    linkage = {"imported_bridge_symbol_names": bridge_imports,
        "names_absent_from_matching_rust_source": absent,
        "symbol_names_only_checked": True, "binary_abi_link_checked": False}
    write_json(out / "bridge-symbol-contract.json", linkage)
    if absent:
        raise ValueError("frontend imports symbols absent from its matching Rust sources: " + ", ".join(absent))
    return {"status": "SDK_TRANSLATION_UNITS_COMPILED", "official_iphoneos_sdk_compile": True,
        "objects": {p.name: {"bytes": p.stat().st_size, "sha256": sha(p)} for p in (frontend, codec)},
        "matching_bridge_symbol_names_checked": True,
        "link_performed": False, "runtime_executed": False,
        "publication_ready": False, "metal_acceleration_pass": False,
        "remaining_link_inputs": ["Matching Rust static library and qualified Cargo lock/toolchain/fixtures",
            "DRE native Darwin Vulkan BO/address-space/queue/fence provider and Turnip library",
            "Qualified platform/cache/event/IOSurface synchronization imports"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--source-cache", type=Path, help="read-only pinned AdrenoMetal checkout")
    parser.add_argument("--prepare-only", action="store_true", help="stage/check public inputs; no SDK compile")
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists():
        parser.error("--out must be a new directory; existing results are preserved")
    out.mkdir(parents=True)
    result = {"status": "FAIL", "stage": "source_preparation", "official_iphoneos_sdk_compile": False,
              "publication_ready": False, "metal_acceleration_pass": False}
    try:
        pin = json.loads((HERE / "upstream.json").read_text(encoding="utf-8"))
        inputs = json.loads((HERE / "build_inputs.json").read_text(encoding="utf-8"))
        print("Preparing exact public Metal frontend/bridge source inputs", flush=True)
        source = args.source_cache.resolve() if args.source_cache else fetch_frontend(out, pin)
        provenance = stage_inputs(source, out, inputs)
        write_json(out / "source-provenance.json", provenance)
        result["stage"] = "sdk_compilation"
        if args.prepare_only:
            result.update(status="SOURCE_PREPARED_NOT_COMPILED", stage="source_preparation")
        else:
            result = official_sdk_compile(out, provenance)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        result["error"] = str(exc)
        print("FAIL: " + str(exc), file=sys.stderr, flush=True)
    write_json(out / "build-result.json", result)
    print(json.dumps({key: result[key] for key in ("status", "publication_ready", "metal_acceleration_pass")}))
    return int(result["status"] == "FAIL")


if __name__ == "__main__":
    sys.exit(main())
