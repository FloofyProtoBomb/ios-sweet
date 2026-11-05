#!/usr/bin/env python3
"""Prepare the exact matching Rust bridge/Turnip reference inputs, not a driver.

Uses a completed build_frontend source-preparation directory, verifies its
public inputs and applies all 30 pinned bridge patches to a new translator
checkout. It never builds, links, loads or publishes a GPU device. The Mesa
patches are staged for audit only: their A740 transport is not a DRE backend.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request

from build_frontend import HERE, command, sha, write_json


def verify_prepared(build: Path) -> dict:
    provenance = json.loads((build / "source-provenance.json").read_text(encoding="utf-8"))
    pin = json.loads((HERE / "upstream.json").read_text(encoding="utf-8"))
    if provenance["frontend_repository"] != pin:
        raise ValueError("frontend source revision differs from the reviewed pin")
    if provenance.get("publication_ready") or provenance.get("metal_acceleration_pass"):
        raise ValueError("source-preparation input must not claim hardware readiness")
    for name, digest in provenance["prepared_source_sha256"].items():
        if Path(name).is_absolute() or ".." in Path(name).parts:
            raise ValueError("unsafe prepared source filename")
        if sha(build / "prepared" / name) != digest:
            raise ValueError(f"prepared source changed: {name}")
    return provenance


def stage(build: Path, out: Path) -> dict:
    prepared = verify_prepared(build)
    inputs = json.loads((HERE / "build_inputs.json").read_text(encoding="utf-8"))
    if prepared["matching_dependency_inputs"] != inputs:
        raise ValueError("matching dependency recipe differs")
    bridge = out / "bridge"
    bridge.mkdir()
    copied = {}
    sources = ["bridge/NativeMetalBridge.rs", "bridge/NativeMetalObjects.rs",
                 "bridge/NativeMetalBridge.h", "bridge/NativeMetalObjects.h", "LICENSE",
                 "shaders/scanout_convert.ll"]
    if prepared.get("dre_device_policy"):
        sources.append("bridge/dre_device_facts.rs.inc")
    for name in sources:
        source = build / "prepared" / name
        destination = bridge / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        copied[name] = sha(destination)
    translator = out / "metal2vulkan"
    command(["git", "init", translator], out, "translator-init")
    command(["git", "-C", translator, "config", "core.autocrlf", "false"], out, "translator-line-endings")
    command(["git", "-C", translator, "remote", "add", "origin", inputs["translator"]["repository"]], out, "translator-origin")
    command(["git", "-C", translator, "fetch", "--depth", "1", "origin", inputs["translator"]["commit"]], out, "translator-fetch")
    command(["git", "-C", translator, "checkout", "--detach", "FETCH_HEAD"], out, "translator-checkout")
    for name, digest in inputs["translator"]["files"].items():
        if sha(translator / name) != digest:
            raise ValueError(f"translator pin differs: {name}")
    for index, entry in enumerate(inputs["bridge_patches"], 1):
        patch = build / "runtime-inputs" / entry["path"]
        if sha(patch) != entry["sha256"] or patch.stat().st_size != entry["bytes"]:
            raise ValueError(f"bridge patch differs: {entry['path']}")
        command(["git", "-C", translator, "apply", "--check", patch], out, f"patch-{index:02d}-check")
        command(["git", "-C", translator, "apply", patch], out, f"patch-{index:02d}-apply")
    command(["git", "-C", translator, "add", "-A"], out, "translator-tree-index")
    tree = command(["git", "-C", translator, "write-tree"], out, "translator-patched-tree").strip()
    if len(tree) != 40:
        raise ValueError("invalid patched translator tree identity")
    fixtures = out / "fixtures"
    fixtures.mkdir()
    for name, source in (
        ("runtime_examples.ll", translator / "crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"),
        ("function_constant.ll", translator / "crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"),
        ("scanout_convert.ll", bridge / "shaders/scanout_convert.ll")):
        shutil.copyfile(source, fixtures / name)
    manifest = out / "Cargo.toml"
    manifest.write_text("\n".join([
        '[package]', 'name = "dre-metal306-bridge"', 'version = "0.1.0"',
        'edition = "2024"', 'rust-version = "1.98"', '[workspace]', '[lib]',
        'crate-type = ["staticlib"]', 'path = ' + json.dumps(str(bridge / "bridge/NativeMetalBridge.rs")),
        '[dependencies]',
        'metal2vulkan = { path = ' + json.dumps(str(translator / "crates/metal2vulkan")) + ', features = ["ash"] }',
        'metal2vulkan-runtime = { path = ' + json.dumps(str(translator / "crates/metal2vulkan-runtime")) + ' }',
        'libc = "0.2"', 'sha2 = { version = "0.10", features = ["asm"] }',
        '[profile.release]', 'panic = "unwind"', 'debug = 1', 'lto = "fat"', 'codegen-units = 1', ''
    ]), encoding="utf-8", newline="\n")
    upstream_lock = translator / "Cargo.lock"
    # A different top-level package and feature set must be resolved/qualified
    # explicitly. An unrelated lock file is evidence, not a fabricated ready lock.
    locks = {"translator_upstream_lock_sha256": sha(upstream_lock) if upstream_lock.is_file() else None,
             "bridge_lock_qualified": False}
    mesa_dir = out / "mesa-reference-patches"
    mesa_dir.mkdir()
    recipe = inputs["orchestrator"]
    prefix = f"https://raw.githubusercontent.com/UniXNU/UniXNU/{recipe['commit']}/"

    def fetch(entry):
        with urllib.request.urlopen(prefix + entry["path"], timeout=30) as response:
            raw = response.read(8 * 1024 * 1024 + 1)
        if len(raw) != entry["bytes"] or hashlib.sha256(raw).hexdigest() != entry["sha256"]:
            raise ValueError(f"Mesa reference patch differs: {entry['path']}")
        (mesa_dir / Path(entry["path"]).name).write_bytes(raw)
        return entry

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        mesa_patches = list(pool.map(fetch, inputs["mesa_patches"]))
    runtime_id = hashlib.sha256((inputs["translator"]["commit"] + "\n" + tree).encode()).hexdigest()[:32]
    environment = {"NMB_RUNTIME_ID": runtime_id, "IPHONEOS_DEPLOYMENT_TARGET": "16.0",
        "NMB_FIXTURE_AIR": str(fixtures / "runtime_examples.air"),
        "NMB_FC_AIR": str(fixtures / "function_constant.air"),
        "NMB_SCANOUT_AIR": str(fixtures / "scanout_convert.air")}
    write_json(out / "bridge-build-environment.json", environment)
    return {
        "schema": 1, "status": "MATCHING_BRIDGE_SOURCES_PREPARED_NOT_COMPILED",
        "translator": inputs["translator"], "translator_patched_tree": tree,
        "bridge_patches_applied": len(inputs["bridge_patches"]), "bridge_sources_sha256": copied,
        "dre_device_policy": prepared.get("dre_device_policy"),
        "fixture_sources_sha256": {p.name: sha(p) for p in fixtures.iterdir()},
        "cargo_manifest_sha256": sha(manifest), "locks": locks,
        "rust_toolchain": inputs["rust"]["toolchain"], "rust_target": inputs["rust"]["target"],
        "mesa_recipe": inputs["mesa_recipe"], "mesa_reference_patches": mesa_patches,
        "mesa_patches_applied": False, "mesa_checkout_present": False,
        "source_licenses": inputs["licenses"], "rust_library_built": False,
        "ios_link_performed": False, "publication_ready": False, "metal_acceleration_pass": False,
        "blockers": ["Assemble the actual fixture IR with the qualified LLVM assembler and record hashes",
            "Resolve and qualify the generated bridge Cargo.lock with the selected toolchain",
            "Compile the matching Rust static library for aarch64-apple-ios16.0 using the official SDK",
            "Provide a real DRE Darwin Vulkan BO/address-space/queue/fence backend; the reference Mesa transport is A740-specific",
            "Audit every upstream runtime patch behavior before device publication"]
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontend-build", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists():
        parser.error("--out must be a new directory")
    out.mkdir(parents=True)
    try:
        result = stage(args.frontend_build.resolve(), out)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        result = {"status": "FAIL", "error": str(exc), "publication_ready": False,
                  "metal_acceleration_pass": False}
    write_json(out / "bridge-source-provenance.json", result)
    print(json.dumps({key: result[key] for key in ("status", "publication_ready", "metal_acceleration_pass")}))
    return int(result["status"] == "FAIL")


if __name__ == "__main__":
    sys.exit(main())
