#!/usr/bin/env python3
"""Compile/link genuine Darwin transport and a read-only iOS metadata observer.

Requires the real macOS Xcode/iPhoneOS SDK. Executes no iOS program, opens no
connection, and supplies no fake frontend/Rust link symbol. Factory remains an
object until the real matching runtime dependencies can satisfy its imports.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import sys
import subprocess

from build_frontend import HERE, command, sha, write_json
from test_runtime import check_ios_object


def build(out: Path) -> dict:
    if sys.platform != "darwin":
        raise ValueError("actual macOS/Xcode iPhoneOS SDK required")
    sys.path.insert(0, str(HERE.parent / "vulkan"))
    from build_turnip_dre import check_ios_dylib
    sdk = command(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], out, "sdk-path").strip()
    if not Path(sdk).is_dir():
        raise ValueError("actual SDK directory absent")
    cc = command(["xcrun", "--sdk", "iphoneos", "--find", "clang"], out, "clang-path").strip()
    cpp = command(["xcrun", "--sdk", "iphoneos", "--find", "clang++"], out, "clangpp-path").strip()
    flags = ["-target", "arm64-apple-ios16.0", "-isysroot", sdk, "-O2", "-g", "-Wall", "-Wextra", "-Werror"]
    vulkan, kernel = HERE.parent / "vulkan", HERE.parent / "kernel"
    include = ["-I", vulkan, "-I", kernel]
    objects = out / "objects"; objects.mkdir()
    # Complete the independent read-only artifact first. A later transport
    # compilation failure must not hide a genuinely linked observer.
    observer = objects / "dre-metal306-metadata-probe"
    command([cpp, *flags, "-std=c++17", "-fobjc-arc", "-fblocks",
             HERE / "base_init/base_metadata.mm", HERE / "base_init/metadata_probe.mm",
             "-framework", "Foundation", "-framework", "Metal", "-lobjc", "-o", observer],
            out, "link-metadata-observer")
    command(["codesign", "--force", "--sign", "-", "--timestamp=none", observer], out, "sign-metadata-observer")
    command(["codesign", "--verify", "--strict", observer], out, "verify-metadata-observer")
    write_json(out / "metadata-observer-result.json", {
        "status": "OFFICIAL_IOS_METADATA_OBSERVER_LINKED_ONLY", "target": "arm64-apple-ios16.0",
        "file": observer.name, "bytes": observer.stat().st_size, "sha256": sha(observer),
        "metadata_observer_executed": False, "device_or_gpu_executed": False,
        "publication_ready": False, "metal_acceleration_pass": False})
    (out / "SHA256SUMS").write_text(sha(observer) + "  objects/" + observer.name + "\n")
    compiled = []
    for name in ("dre_darwin", "dre_surface_iosurface"):
        target = objects / (name + ".o")
        command([cc, *flags, *include, "-std=c11", "-c", vulkan / (name + ".c"), "-o", target],
                out, "compile-" + name)
        check_ios_object(target); compiled.append(target)
    factory = objects / "dre_metal_factory.o"
    command([cpp, *flags, *include, "-std=c++17", "-fno-objc-arc", "-fblocks", "-c",
             HERE / "dre_metal_factory.mm", "-o", factory], out, "compile-factory")
    check_ios_object(factory); compiled.append(factory)
    factory_imports = command(["xcrun", "nm", "-u", factory], out, "factory-imports")
    if "_native_bridge_metal_open" not in {line.split()[-1] for line in factory_imports.splitlines() if line.split()}:
        raise ValueError("factory no longer requires its real matching frontend entrypoint")
    transport = objects / "libdre_native_transport.dylib"
    command([cc, *flags, "-dynamiclib", "-Wl,-undefined,error", "-Wl,-fatal_warnings",
             "-Wl,-install_name,@rpath/libdre_native_transport.dylib", *compiled[:2],
             "-framework", "CoreFoundation", "-framework", "IOSurface", "-o", transport],
            out, "link-native-transport")
    check_ios_dylib(transport)
    imports = command(["xcrun", "nm", "-u", transport], out, "transport-imports")
    public_reference=json.loads((vulkan / "iokituser-reference.json").read_text())
    unavailable={"_"+name for name in public_reference["symbols"]}
    if any(line.split()[-1] in unavailable for line in imports.splitlines() if line.split()):
        raise ValueError("native transport still has a direct unavailable IOKitUser import")
    # IOSurfaceCreateMachPort is an actual SDK-linked IOSurface import. Only
    # IOKitUser operations use the exact real runtime framework resolver.
    command(["codesign", "--force", "--sign", "-", "--timestamp=none", transport], out, "sign-transport")
    command(["codesign", "--verify", "--strict", transport], out, "verify-transport-signature")
    files = compiled + [transport, observer]
    (out / "SHA256SUMS").write_text("".join(sha(p) + "  objects/" + p.name + "\n" for p in files))
    source_files = [HERE / "dre_metal_factory.mm", HERE / "base_init/base_metadata.mm",
                    HERE / "base_init/base_metadata.h", HERE / "base_init/metadata_probe.mm",
                    vulkan / "dre_darwin.c", vulkan / "dre_darwin.h", vulkan / "dre_iokit_loader.h",
                    vulkan / "dre_surface_iosurface.c", vulkan / "dre_surface_iosurface.h",
                    vulkan / "dre_iosurface_abi.h",
                    kernel / "dre_gpu_uapi.h"]
    return {"status": "OFFICIAL_IOS_NATIVE_BOUNDARY_COMPILED_LINKED_ONLY",
        "target": "arm64-apple-ios16.0", "official_iphoneos_sdk_used": True, "sdk_path": sdk,
        "sdk_build": command(["xcrun", "--sdk", "iphoneos", "--show-sdk-build-version"], out, "sdk-build").strip(),
        "files": {p.name: {"bytes": p.stat().st_size, "sha256": sha(p)} for p in files},
        "source_sha256": {str(p.relative_to(HERE.parent)): sha(p) for p in source_files},
        "public_iokit_reference": public_reference,
        "native_transport_dylib_strict_link": True, "factory_object_compiled": True,
        "factory_full_link": False, "factory_requires_real_frontend_and_ios_rust": True,
        "metadata_observer_linked": True, "metadata_observer_executed": False,
        "iokit_entrypoints_runtime_resolved": False, "private_apple_headers_used": False,
        "unresolved_import_waiver": False, "device_or_gpu_executed": False,
        "publication_ready": False, "metal_acceleration_pass": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(); out = args.out.resolve()
    if out.exists(): parser.error("--out must be new; previous evidence is preserved")
    out.mkdir(parents=True)
    try:
        result = build(out)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        result = {"status": "FAIL", "error": str(error), "publication_ready": False, "metal_acceleration_pass": False}
        print("FAIL: " + str(error), file=sys.stderr)
    write_json(out / "native-sdk-result.json", result)
    return int(result["status"] == "FAIL")


if __name__ == "__main__":
    raise SystemExit(main())
