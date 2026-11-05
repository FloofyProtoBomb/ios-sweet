#!/usr/bin/env python3
"""Attempt a real iPhoneOS Turnip build; native execution remains unqualified.

Requires an actual macOS/Xcode SDK plus working Meson,
Ninja and glslangValidator. No dependency installer, source fallback, GPU,
simulated physical device, image modification or publication is performed.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from prepare_turnip_dre import HERE, sha


def command(argv, out: Path, name: str, env: dict, timeout=180) -> str:
    log = out / (name + ".log")
    with log.open("w", encoding="utf-8") as stream:
        stream.write(json.dumps([str(x) for x in argv]) + "\n"); stream.flush()
        result = subprocess.run([str(x) for x in argv], env=env, stdout=stream,
            stderr=subprocess.STDOUT, timeout=timeout, check=False)
    text = log.read_text(encoding="utf-8", errors="replace")
    if result.returncode:
        lines = text.splitlines()
        errors = [line for line in lines if "error" in line.lower()]
        print("\n".join((errors or lines[-30:])[:45])[:8000], file=sys.stderr)
        raise ValueError(f"{name} failed ({result.returncode}); see {log.name}")
    return "\n".join(text.splitlines()[1:]).strip()


def check_ios_dylib(path: Path) -> None:
    raw = path.read_bytes()
    if len(raw) < 32 or struct.unpack_from("<4I", raw) != (0xFEEDFACF, 0x0100000C, 0, 6):
        raise ValueError("product is not a plain ARM64 Mach-O dylib")
    commands, command_bytes = struct.unpack_from("<2I", raw, 16)
    at, ios = 32, False
    if command_bytes > len(raw) - at: raise ValueError("truncated Mach-O commands")
    end = at + command_bytes
    for _ in range(commands):
        if at + 8 > end: raise ValueError("truncated Mach-O command")
        kind, size = struct.unpack_from("<2I", raw, at)
        if size < 8 or size > end - at: raise ValueError("invalid Mach-O command range")
        if kind == 0x32:
            if size < 24: raise ValueError("truncated build version")
            platform, minimum = struct.unpack_from("<2I", raw, at + 8)
            if platform != 2 or minimum > 0x00100000: raise ValueError("not iPhoneOS16.0-compatible load metadata")
            ios = True
        at += size
    if at != end or not ios: raise ValueError("missing/invalid iPhoneOS build metadata")


def build(args, out: Path) -> dict:
    if sys.platform != "darwin": raise ValueError("real macOS/Xcode iPhoneOS SDK required")
    prepared = args.prepared.resolve()
    profile = json.loads((prepared / "turnip-source-provenance.json").read_text(encoding="utf-8"))
    if profile.get("status") != "UNVALIDATED_SOURCE_PREPARED" or profile.get("required_build_platforms") != []:
        raise ValueError("exact DRE source-preparation profile required")
    source = prepared / "mesa"
    env = {k: v for k, v in os.environ.items() if not k.startswith(("IR3_", "FD_MESA_", "NIR_", "MESA_SHADER_", "TU_"))}
    if command(["git", "-C", source, "rev-parse", "HEAD"], out, "source-revision", env) != profile["mesa"]["commit"]:
        raise ValueError("Mesa revision changed")
    if command(["git", "-C", source, "write-tree"], out, "source-tree", env) != profile["prepared_index_tree"]:
        raise ValueError("prepared source index changed")
    command(["git", "-C", source, "diff", "--quiet"], out, "source-unindexed", env)
    extras = command(["git", "-C", source, "ls-files", "--others", "--exclude-standard"], out, "source-extra", env)
    if extras: raise ValueError("unrecorded source additions")
    sdk = command(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], out, "sdk-path", env)
    if not Path(sdk).is_dir(): raise ValueError("actual iPhoneOS SDK directory missing")
    tools = {name: command(["xcrun", "--sdk", "iphoneos", "--find", name], out, "find-" + name, env)
        for name in ("clang", "clang++", "ar", "strip")}
    glslang = shutil.which("glslangValidator")
    if not glslang: raise ValueError("host glslangValidator prerequisite is missing; no unqualified installer fallback")
    command([glslang, "--version"], out, "glslang-version", env)
    command([args.meson, "--version"], out, "meson-version", env)
    command([args.ninja, "--version"], out, "ninja-version", env)
    flags = ["-target", "arm64-apple-ios16.0", "-isysroot", sdk, "-fstack-protector-strong"]
    cross = out / "iphoneos.cross"
    cross.write_text("[binaries]\n" + "\n".join(
        name + " = " + repr(value) for name, value in
        (("c", tools["clang"]), ("cpp", tools["clang++"]), ("objc", tools["clang"]),
         ("ar", tools["ar"]), ("strip", tools["strip"]), ("pkg-config", "/usr/bin/false"))) +
        "\n[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n"
        "[properties]\nneeds_exe_wrapper = true\n[built-in options]\n" + "".join(
            k + " = " + repr(flags + (["-Wl,-undefined,error"] if "link" in k else [])) + "\n" for k in
            ("c_args", "cpp_args", "objc_args", "c_link_args", "cpp_link_args", "objc_link_args")), encoding="utf-8")
    options = ["-Dgallium-drivers=", "-Dvulkan-drivers=freedreno", "-Dfreedreno-kmds=darwin",
        "-Dtu-darwin-abi-dir=" + str(source / "src/freedreno/vulkan"), "-Dplatforms=[]", "-Dglx=disabled",
        "-Degl=disabled", "-Dgbm=disabled", "-Dopengl=false", "-Dgles1=disabled", "-Dgles2=disabled",
        "-Dllvm=disabled", "-Dshared-glapi=disabled", "-Dxmlconfig=disabled", "-Dshader-cache=enabled",
        "-Dtools=", "-Dbuild-tests=false", "-Dvideo-codecs=", "-Dvulkan-layers=", "-Dlibunwind=disabled",
        "-Dlmsensors=disabled", "-Dvalgrind=disabled", "-Dexpat=disabled", "-Dzstd=disabled",
        "-Dbuildtype=release", "-Db_ndebug=true"]
    compiler = command([tools["clang++"], "--version"], out, "compiler-version", env)
    identity = {"prepared_tree": profile["prepared_index_tree"], "compiler": compiler,
        "target": "arm64-apple-ios16.0", "options": options,
        "build_recipe_sha256": sha(Path(__file__)), "kernel_uapi_sha256":
            sha(source / "src/freedreno/vulkan/dre_gpu_uapi.h")}
    source_id = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    pack_sha, pack_header = None, ""
    if args.builtin_pack:
        pack = args.builtin_pack.read_bytes()
        if (not 112 <= len(pack) <= 4 * 1024 * 1024 or pack[:8] != b"TUBUILT1" or
            pack[16:80] != source_id.encode() or struct.unpack_from("<I", pack, 80)[0] != 619 or
            struct.unpack_from("<Q", pack, 84)[0] != 0x06010900):
            raise ValueError("builtin pack is not for this exact source/compiler profile and A619 chip; never relabel an A740 pack")
        pack_sha = hashlib.sha256(pack).hexdigest()
        header = out / "tu_builtin_pack.generated.h"
        header.write_text("/* Own compiler output. Not an Apple artifact. */\nstatic const unsigned char tu_builtin_pack_bytes[] = {\n" +
            "".join(",".join(hex(b) for b in pack[i:i + 16]) + ",\n" for i in range(0, len(pack), 16)) + "};\n", encoding="utf-8")
        pack_header = str(header)
    options += ["-Dtu-builtin-source-id=" + source_id, "-Dtu-builtin-pack-header=" + pack_header,
        "-Dtu-build-id=" + source_id[:40]]
    (out / "builtin-build-profile.json").write_text(json.dumps({"source_id": source_id,
        "profile": identity, "embedded_pack_sha256": pack_sha}, indent=2) + "\n", encoding="utf-8")
    env.update(CC=tools["clang"], CXX=tools["clang++"], CFLAGS="", CXXFLAGS="", CPPFLAGS="", LDFLAGS="",
        SDKROOT=sdk, IPHONEOS_DEPLOYMENT_TARGET="16.0")
    build_dir = out / "build"
    command([args.meson, "setup", build_dir, source, "--cross-file", cross, "--wrap-mode=nodownload"] + options,
        out, "configure", env, timeout=600)
    command([args.ninja, "-C", build_dir, "-j2"], out, "build", env, timeout=3600)
    dylib = build_dir / "src/freedreno/vulkan/libvulkan_freedreno.dylib"
    check_ios_dylib(dylib)
    exports = command(["nm", "-gU", dylib], out, "exports", env)
    for name in ("_vkGetInstanceProcAddr", "_vk_icdGetInstanceProcAddr"):
        if not any(line.split()[-1:] == [name] for line in exports.splitlines()):
            raise ValueError("real Turnip loader export missing: " + name)
    dependencies = command(["otool", "-L", dylib], out, "dependencies", env)
    objects = out / "objects"; objects.mkdir()
    copied = objects / dylib.name; shutil.copyfile(dylib, copied)
    return {"status": "COMPILED_IPHONEOS_TURNIP_UNVALIDATED", "validation": "COMPILE_ONLY_NOT_NATIVE_QUALIFICATION",
        "target": "arm64-apple-ios16.0", "source": profile, "source_id": source_id,
        "dylib": {"file": copied.name, "bytes": copied.stat().st_size, "sha256": sha(copied)},
        "official_iphoneos_sdk": sdk, "dependencies": dependencies,
        "strict_undefined_link": True, "unresolved_import_waiver": False,
        "builtin_pack_sha256": pack_sha, "native_provider_exercised": False,
        "physical_device_enumerated": False, "runtime_executed": False,
        "publication_ready": False, "metal_acceleration_pass": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepared", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--meson", required=True, type=Path)
    parser.add_argument("--ninja", required=True, type=Path)
    parser.add_argument("--builtin-pack", type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists(): parser.error("--out must be new")
    out.mkdir(parents=True)
    try:
        result = build(args, out)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        result = {"status": "FAIL", "error": str(exc), "publication_ready": False, "metal_acceleration_pass": False}
    (out / "turnip-build-result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("status", "publication_ready", "metal_acceleration_pass")}))
    return int(result["status"] == "FAIL")


if __name__ == "__main__": sys.exit(main())
