#!/usr/bin/env python3
"""Build the real matching Rust static library, with exact fixtures and lock.

--target host proves a host library/API compilation, not iOS compatibility.
--target ios requires macOS and a real Xcode iPhoneOS SDK. Neither links the
frontend, opens Vulkan/IOKit, or performs GPU work. Inputs come from the verified
prepare_bridge_source.py output; no stand-in fixture bytes are accepted.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from build_frontend import HERE, sha, write_json
from verify_bridge_exports import digest, verify as verify_exports


def run(args, out: Path, name: str, env: dict, *, timeout=180) -> str:
    args = [str(a) for a in args]
    path = out / f"{name}.log"
    with path.open("w", encoding="utf-8") as stream:
        stream.write(json.dumps(args) + "\n")
        stream.flush()
        result = subprocess.run(args, env=env, stdout=stream,
            stderr=subprocess.STDOUT, timeout=timeout, check=False)
    text = path.read_text(encoding="utf-8", errors="replace")
    if result.returncode:
        errors = [x for x in text.splitlines() if "error" in x.lower()]
        print("\n".join((errors or text.splitlines()[-30:])[:45])[:8000], file=sys.stderr)
        raise ValueError(f"{name} exited {result.returncode}; see {path.name}")
    return "\n".join(text.splitlines()[1:])


def verify_source(source: Path, out: Path, env: dict) -> dict:
    profile = json.loads((source / "bridge-source-provenance.json").read_text(encoding="utf-8"))
    inputs = json.loads((HERE / "build_inputs.json").read_text(encoding="utf-8"))
    if profile.get("status") != "MATCHING_BRIDGE_SOURCES_PREPARED_NOT_COMPILED" or profile["translator"] != inputs["translator"]:
        raise ValueError("matching source preparation is missing or differs")
    if profile["bridge_patches_applied"] != len(inputs["bridge_patches"]):
        raise ValueError("incomplete bridge patch stack")
    for name, digest in profile["bridge_sources_sha256"].items():
        if Path(name).is_absolute() or ".." in Path(name).parts or sha(source / "bridge" / name) != digest:
            raise ValueError(f"bridge source differs: {name}")
    for name, digest in profile["fixture_sources_sha256"].items():
        if Path(name).name != name or sha(source / "fixtures" / name) != digest:
            raise ValueError(f"fixture IR differs: {name}")
    if sha(source / "Cargo.toml") != profile["cargo_manifest_sha256"]:
        raise ValueError("generated Cargo manifest differs")
    translator = source / "metal2vulkan"
    head = run(["git", "-C", translator, "rev-parse", "HEAD"], out, "source-head", env).strip()
    tree = run(["git", "-C", translator, "write-tree"], out, "source-tree", env).strip()
    if head != inputs["translator"]["commit"] or tree != profile["translator_patched_tree"]:
        raise ValueError("translator source/index tree differs from verified patch result")
    run(["git", "-C", translator, "diff", "--quiet"], out, "source-unindexed-check", env)
    extras = run(["git", "-C", translator, "ls-files", "--others", "--exclude-standard"], out, "source-extra-check", env)
    if extras.strip():
        raise ValueError("unrecorded translator source additions")
    return profile


def build(args, out: Path) -> dict:
    env = dict(os.environ, CARGO_BUILD_JOBS="2", CARGO_TARGET_DIR=str(out / "target"))
    source = args.source.resolve()
    profile = verify_source(source, out, env)
    assembler = args.llvm_as.resolve()
    llvm_version = run([assembler, "--version"], out, "assembler-version", env)
    found = re.search(r"LLVM version (\d+)\.(\d+)\.(\d+)", llvm_version)
    if not found or tuple(map(int, found.groups())) != (21, 1, 8):
        raise ValueError("requires qualified LLVM21.1.8; do not substitute dummy fixtures")
    fixture_dir = out / "fixtures"
    fixture_dir.mkdir()
    fixtures = {}
    for name in ("runtime_examples", "function_constant", "scanout_convert"):
        ir, air = source / "fixtures" / f"{name}.ll", fixture_dir / f"{name}.air"
        run([assembler, ir, "-o", air], out, f"assemble-{name}", env)
        if air.stat().st_size < 16 or air.read_bytes()[:4] not in (b"BC\xc0\xde", b"\xde\xc0\x17\x0b"):
            raise ValueError("assembler output is not real LLVM bitcode")
        fixtures[name] = {"ir_sha256": sha(ir), "air_sha256": sha(air), "bytes": air.stat().st_size}
    identity = {"translator_commit": profile["translator"]["commit"],
        "translator_tree": profile["translator_patched_tree"], "fixtures": fixtures,
        "assembler_sha256": sha(assembler),
        "bridge_sources_sha256": profile["bridge_sources_sha256"],
        "dre_device_policy": profile.get("dre_device_policy")}
    runtime_id = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:32]
    env.update(NMB_RUNTIME_ID=runtime_id, NMB_FIXTURE_AIR=str(fixture_dir / "runtime_examples.air"),
        NMB_FC_AIR=str(fixture_dir / "function_constant.air"), NMB_SCANOUT_AIR=str(fixture_dir / "scanout_convert.air"))
    manifest = out / "Cargo.toml"
    shutil.copyfile(source / "Cargo.toml", manifest)
    # rustup selects its proxy by argv[0]. Resolving cargo's symlink to rustup
    # would invoke the manager rather than the pinned cargo tool.
    cargo = args.cargo.absolute()
    toolchain = "+1.98.0"
    cargo_version = run([cargo, toolchain, "--version"], out, "cargo-version", env)
    if not re.search(r"cargo 1\.98\.0\b", cargo_version):
        raise ValueError("requires the official Rust1.98.0 toolchain")
    sdk = None
    target = None
    if args.target == "ios":
        if sys.platform != "darwin":
            raise ValueError("true iOS archive requires macOS with Xcode/iPhoneOS SDK; host archive is separate")
        sdk = run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], out, "sdk-path", env).strip()
        if not Path(sdk).is_dir():
            raise ValueError("xcrun did not provide an actual SDK directory")
        env.update(SDKROOT=sdk, IPHONEOS_DEPLOYMENT_TARGET="16.0")
        target = "aarch64-apple-ios"
    lock = args.lockfile or (HERE / "bridge-Cargo.lock")
    if lock.is_file():
        expected = json.loads((HERE / "bridge_lock.json").read_text(encoding="utf-8"))
        if sha(lock) != expected["sha256"]:
            raise ValueError("Cargo.lock differs from the resolved matching bridge pin")
        shutil.copyfile(lock, out / "Cargo.lock")
    elif args.resolve_lock:
        run([cargo, toolchain, "generate-lockfile", "--manifest-path", manifest], out, "cargo-lock", env)
    else:
        raise ValueError("a qualified bridge Cargo.lock is required (initial host resolution uses --resolve-lock)")
    lock_sha = sha(out / "Cargo.lock")
    write_json(out / "build-inputs.json", {"source": profile, "runtime_id": runtime_id,
        "target": args.target, "sdk": sdk, "cargo_version": cargo_version,
        "assembler_version": llvm_version, "assembler_sha256": sha(assembler),
        "fixtures": fixtures, "cargo_lock_sha256": lock_sha})
    cargo_args = [cargo, toolchain, "build", "--release", "--locked", "--manifest-path", manifest, "-j", "2"]
    if target:
        cargo_args += ["--target", target]
    run(cargo_args, out, "cargo-build", env, timeout=1200)
    if sha(out / "Cargo.lock") != lock_sha:
        raise ValueError("locked Cargo build changed the lockfile")
    produced = out / "target" / (target or "") / "release/libdre_metal306_bridge.a"
    if not produced.is_file():
        raise ValueError("Cargo did not produce a real static archive")
    with produced.open("rb") as stream:
        if stream.read(8) != b"!<arch>\n":
            raise ValueError("Cargo output is not a real static archive")
    objects = out / "objects"
    objects.mkdir()
    archive = objects / "libdre_metal306_bridge.a"
    shutil.copyfile(produced, archive)
    ar = assembler.with_name("llvm-ar")
    members = run([ar, "t", archive], out, "archive-members", env).splitlines()
    if not members:
        raise ValueError("static archive has no object members")
    first = subprocess.check_output([str(ar), "p", str(archive), members[0]], env=env, timeout=60)
    if args.target == "ios":
        if len(first) < 16 or struct.unpack_from("<3I", first) != (0xFEEDFACF, 0x0100000C, 0):
            raise ValueError("first iOS archive object is not plain ARM64 Mach-O")
    elif not (first.startswith(b"\x7fELF\x02") or first[:4] in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe")):
        raise ValueError("first host archive member is not a native object")
    exports = None
    if args.frontend_contract:
        exports = verify_exports(archive, args.frontend_contract, assembler.with_name("llvm-nm"), out)
        write_json(out / "bridge-export-verification.json", exports)
    return {"status": "RUST_IOS_STATIC_LIBRARY_COMPILED" if target else "RUST_HOST_STATIC_LIBRARY_COMPILED",
        "target": target or "host", "official_iphoneos_sdk_used": bool(sdk),
        "archive": {"file": archive.name, "bytes": archive.stat().st_size, "sha256": digest(archive), "members": len(members)},
        "cargo_lock_sha256": lock_sha, "runtime_id": runtime_id, "fixtures": fixtures,
        "frontend_symbol_names_checked": exports is not None,
        "frontend_link_performed": False, "vulkan_or_iokit_opened": False,
        "runtime_executed": False, "publication_ready": False, "metal_acceleration_pass": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cargo", required=True, type=Path, help="official rustup cargo proxy with1.98.0 installed")
    parser.add_argument("--llvm-as", required=True, type=Path)
    parser.add_argument("--target", choices=("host", "ios"), default="host")
    parser.add_argument("--lockfile", type=Path)
    parser.add_argument("--resolve-lock", action="store_true", help="initial dependency resolution; preserve generated lock")
    parser.add_argument("--frontend-contract", type=Path,
        help="actual build_frontend bridge-symbol-contract.json for binary export-name checking")
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists():
        parser.error("--out must be new; existing evidence is preserved")
    out.mkdir(parents=True)
    try:
        result = build(args, out)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        result = {"status": "FAIL", "error": str(exc), "publication_ready": False, "metal_acceleration_pass": False}
        print("FAIL: " + str(exc), file=sys.stderr)
    write_json(out / "build-result.json", result)
    print(json.dumps({k: result[k] for k in ("status", "publication_ready", "metal_acceleration_pass")}))
    return int(result["status"] == "FAIL")


if __name__ == "__main__":
    sys.exit(main())
