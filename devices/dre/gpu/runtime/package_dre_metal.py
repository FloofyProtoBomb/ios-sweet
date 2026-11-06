#!/usr/bin/env python3
"""UNVALIDATED future SDK link/package recipe; not run overnight.

Consumes actual SDK frontend objects, a true iOS Rust archive and actual
Turnip dylib. Produces a local development bundle only, never copies into an
iOS image/phone or registers a service. Source drafts and link success are
separate from hardware, factory and Metal acceptance.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

from build_frontend import HERE, write_json
from verify_bridge_exports import digest, verify as verify_exports
from sdk_link import qualify_sdk_linker


def run(argv, out: Path, name: str, timeout=180) -> str:
    with (out / (name + ".log")).open("w", encoding="utf-8") as stream:
        stream.write(json.dumps([str(x) for x in argv]) + "\n"); stream.flush()
        result = subprocess.run([str(x) for x in argv], stdout=stream,
            stderr=subprocess.STDOUT, timeout=timeout, check=False)
    text = (out / (name + ".log")).read_text(encoding="utf-8", errors="replace")
    if result.returncode: raise ValueError(f"{name} failed ({result.returncode}); see its retained log")
    return "\n".join(text.splitlines()[1:]).strip()


def frontend_license(provenance: dict) -> bytes:
    text = provenance.get("third_party_license_text")
    if not isinstance(text, str) or not text.strip():
        raise ValueError("frontend license is missing from the build receipt")
    data = text.encode("utf-8")
    expected = provenance.get("prepared_source_sha256", {}).get("LICENSE")
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError("frontend license differs from the prepared source")
    return data


def package(args, out: Path) -> dict:
    if sys.platform != "darwin": raise ValueError("actual macOS/Xcode is required for the link")
    sys.path.insert(0, str(HERE.parent / "vulkan"))
    from build_turnip_dre import check_ios_dylib
    frontend, bridge, turnip = (p.resolve() for p in (args.frontend_build, args.bridge_build, args.turnip_build))
    front = json.loads((frontend / "build-result.json").read_text(encoding="utf-8"))
    rust = json.loads((bridge / "build-result.json").read_text(encoding="utf-8"))
    vk = json.loads((turnip / "turnip-build-result.json").read_text(encoding="utf-8"))
    if front.get("status") != "SDK_TRANSLATION_UNITS_COMPILED" or not front.get("official_iphoneos_sdk_compile"):
        raise ValueError("actual official SDK frontend object proof missing")
    if rust.get("status") != "RUST_IOS_STATIC_LIBRARY_COMPILED" or not rust.get("official_iphoneos_sdk_used"):
        raise ValueError("a host archive cannot satisfy the actual iOS Rust link input")
    if vk.get("status") != "COMPILED_IPHONEOS_TURNIP_UNVALIDATED" or vk.get("target") != "arm64-apple-ios16.0":
        raise ValueError("actual DRE iPhoneOS Turnip build proof missing")
    if vk.get("strict_undefined_link") is not True or vk.get("unresolved_import_waiver") is not False or \
        (vk.get("strict_default_linker_proof") or {}).get("missing_symbol_rejected") is not True:
        raise ValueError("strict DRE iPhoneOS Turnip link proof missing")
    front_proof = json.loads((frontend / "source-provenance.json").read_text(encoding="utf-8"))
    license_bytes = frontend_license(front_proof)
    connection = front_proof.get("dre_compiler_connection")
    if not connection or connection.get("status") != "UNVALIDATED_COMPILER306_SOURCE_CONNECTED" or \
        connection.get("accepted_request_types") != [13] or connection.get("request14_supported"):
        raise ValueError("selected306 compiler connection is required; unchecked legacy compiler consumers cannot be packaged")
    if front.get("dre_compiler_connection") != connection:
        raise ValueError("compiled frontend/compiler-connection provenance differs")
    policy = front_proof.get("dre_device_policy")
    if not policy or policy.get("status") != "UNVALIDATED_DEVICE_POLICY_SOURCE_CONNECTED" or \
        policy.get("apple_family_equivalence_claimed") or not policy.get("inherited_device_defaults_blocked"):
        raise ValueError("explicit selected306 queried-facts/refusal policy required")
    if front.get("dre_device_policy") != policy:
        raise ValueError("compiled device-policy provenance differs")
    bridge_inputs = json.loads((bridge / "build-inputs.json").read_text(encoding="utf-8"))
    matching = bridge_inputs["source"]["bridge_sources_sha256"]
    if bridge_inputs["source"].get("dre_device_policy") != policy:
        raise ValueError("matching Rust bridge is missing the exact device-policy overlay")
    for name in ("bridge/NativeMetalBridge.rs", "bridge/NativeMetalObjects.rs"):
        if matching[name] != front_proof["prepared_source_sha256"][name]:
            raise ValueError("frontend/Rust source correspondence changed: " + name)
    if matching.get("bridge/dre_device_facts.rs.inc") != front_proof["prepared_source_sha256"].get("bridge/dre_device_facts.rs.inc"):
        raise ValueError("matching Rust facts export source differs")
    objects = []
    for name, proof in front["objects"].items():
        if Path(name).name != name: raise ValueError("unsafe frontend object name")
        obj = frontend / "objects" / name
        if digest(obj) != proof["sha256"] or obj.stat().st_size != proof["bytes"]:
            raise ValueError("frontend object changed: " + name)
        objects.append(obj)
    if {p.name for p in objects} != {"NativeBridgeMetalFrontend.o", "compiler_request.o", "compiler_service.o", "device_policy.o"}:
        raise ValueError("unexpected/missing SDK frontend translation units")
    if Path(rust["archive"]["file"]).name != rust["archive"]["file"]:
        raise ValueError("unsafe Rust archive path")
    archive = bridge / "objects" / rust["archive"]["file"]
    if digest(archive) != rust["archive"]["sha256"] or archive.stat().st_size != rust["archive"]["bytes"]:
        raise ValueError("Rust archive changed")
    if Path(vk["dylib"]["file"]).name != vk["dylib"]["file"]:
        raise ValueError("unsafe Turnip library path")
    dylib = turnip / "objects" / vk["dylib"]["file"]
    if digest(dylib) != vk["dylib"]["sha256"] or dylib.stat().st_size != vk["dylib"]["bytes"]:
        raise ValueError("Turnip library changed")
    check_ios_dylib(dylib)
    # The UAPI is a real source dependency. A bundle and driver built against
    # different ABI1 layouts may not be combined just because version==1.
    uapi = HERE.parent / "kernel/dre_gpu_uapi.h"
    expected_uapi = vk["source"]["dre_overlay_sha256"]["src/freedreno/vulkan/dre_gpu_uapi.h"]
    if digest(uapi) != expected_uapi: raise ValueError("kernel/user ABI header changed since the Turnip build")
    symbol_proof = verify_exports(archive, frontend / "bridge-symbol-contract.json", args.llvm_nm, out)
    write_json(out / "bridge-export-verification.json", symbol_proof)
    sdk = run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], out, "sdk-path")
    compiler = run(["xcrun", "--sdk", "iphoneos", "--find", "clang++"], out, "compiler-path")
    flags = ["-target", "arm64-apple-ios16.0", "-isysroot", sdk, "-O2", "-g", "-std=c++17", "-fblocks"]
    bootstrap = out / "dre_metal_factory.o"
    run([compiler, *flags, "-fno-objc-arc", "-I", uapi.parent, "-I", HERE.parent / "vulkan",
        "-c", HERE / "dre_metal_factory.mm", "-o", bootstrap],
        out, "factory-compile")
    bundle = out / "DREMetal.bundle"; bundle.mkdir()
    executable = bundle / "DREMetal"
    frameworks = []
    for name in ("Foundation", "Metal", "IOSurface", "QuartzCore", "CoreGraphics", "CoreFoundation", "Security"):
        frameworks += ["-framework", name]
    # Observe the actual SDK's strict default, without the deprecated
    # '-undefined error' spelling. Genuine missing symbols must stop the link.
    linker_proof = qualify_sdk_linker(compiler, flags, out)
    run([compiler, *flags, "-dynamiclib", "-ObjC", "-Wl,-dead_strip", "-Wl,-fatal_warnings",
        "-Wl,-install_name,@rpath/DREMetal.bundle/DREMetal", bootstrap, *objects, archive,
        *frameworks, "-o", executable], out, "frontend-rust-link", timeout=600)
    check_ios_dylib(executable)
    bundled_turnip = bundle / dylib.name; shutil.copyfile(dylib, bundled_turnip)
    run(["xcrun", "install_name_tool", "-id", "@loader_path/" + bundled_turnip.name, bundled_turnip], out, "turnip-install-name")
    with (bundle / "Info.plist").open("wb") as stream:
        plistlib.dump({"CFBundleIdentifier": "org.iossweet.dre.metal306", "CFBundleName": "DREMetal",
            "CFBundleExecutable": "DREMetal", "CFBundlePackageType": "BNDL", "CFBundleVersion": "0.0.0",
            "CFBundleSupportedPlatforms": ["iPhoneOS"], "MinimumOSVersion": "16.0",
            "NSPrincipalClass": "DREMetalDevice", "DREUnvalidatedDraft": True}, stream)
    # Source and redistribution obligations remain an explicit separate audit.
    # This private development package is not labeled distribution-ready.
    notices = bundle / "Notices"; notices.mkdir()
    (notices / "Frontend-MIT.txt").write_bytes(license_bytes)
    (notices / "Sources.json").write_text(json.dumps({"frontend": front_proof["frontend_repository"],
        "runtime": bridge_inputs["source"]["translator"], "runtime_license": "GPL-2.0-or-later OR LGPL-3.0-or-later",
        "mesa": vk["source"]["mesa"], "relink_objects_retained_in_build_inputs": True,
        "redistribution_license_audit_complete": False}, indent=2) + "\n", encoding="utf-8")
    run(["codesign", "--force", "--sign", "-", "--timestamp=none", bundled_turnip], out, "sign-turnip")
    run(["codesign", "--force", "--sign", "-", "--timestamp=none", bundle], out, "sign-bundle")
    run(["codesign", "--verify", "--strict", bundled_turnip], out, "verify-turnip-signature")
    run(["codesign", "--verify", "--strict", bundle], out, "verify-local-signature")
    files = {str(p.relative_to(bundle)): {"bytes": p.stat().st_size, "sha256": digest(p)}
        for p in bundle.rglob("*") if p.is_file()}
    return {"status": "LINKED_DRAFT_BUNDLE_NOT_NATIVE_QUALIFIED", "source_validation": "UNVALIDATED_OVERNIGHT_DRAFT",
        "bundle": bundle.name, "files": files, "target": "arm64-apple-ios16.0",
        "factory_abi_selected": "20H392_Metal306_528_byte_MTLDevice", "original_metal_factory_replaced": False,
        "compiler_connection": connection, "compiler_runtime_exercised": False,
        "strict_default_linker_proof": linker_proof,
        "device_policy": policy, "device_policy_runtime_exercised": False,
        "kernel_service_registered": False, "image_or_phone_modified": False,
        "private_apple_headers_used": False, "native_factory_exercised": False,
        "native_metal_probe_passed": False, "redistribution_license_audit_complete": False,
        "publication_ready": False, "metal_acceleration_pass": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontend-build", required=True, type=Path)
    parser.add_argument("--bridge-build", required=True, type=Path)
    parser.add_argument("--turnip-build", required=True, type=Path)
    parser.add_argument("--llvm-nm", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists(): parser.error("--out must be new")
    out.mkdir(parents=True)
    try:
        result = package(args, out)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        result = {"status": "FAIL", "error": str(exc), "publication_ready": False, "metal_acceleration_pass": False}
    write_json(out / "package-result.json", result)
    print(json.dumps({key: result[key] for key in ("status", "publication_ready", "metal_acceleration_pass")}))
    return int(result["status"] == "FAIL")


if __name__ == "__main__": sys.exit(main())
