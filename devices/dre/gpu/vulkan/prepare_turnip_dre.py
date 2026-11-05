#!/usr/bin/env python3
"""UNVALIDATED draft recipe. Do not run until deferred validation is authorized.

Prepare a fresh pinned Mesa85fda source tree, apply the matching 17 Darwin
reference patches, then replace their A740 transport with the DRE ABI1 source.
No compiler, test, GPU, publication, filesystem image or phone is invoked.
This recipe itself has not been run or qualified.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
RUNTIME = HERE.parent / "runtime"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, out: Path, name: str, timeout=180) -> str:
    result = subprocess.run([str(x) for x in args], capture_output=True, text=True,
        check=False, timeout=timeout)
    (out / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise ValueError(f"{name} failed ({result.returncode}); see its retained log")
    return result.stdout.strip()


def replace_once(path: Path, old: str, new: str) -> None:
    data = path.read_text(encoding="utf-8")
    if data.count(old) != 1:
        raise ValueError(f"pinned source context changed: {path.name}: {old[:60]!r}")
    path.write_text(data.replace(old, new), encoding="utf-8", newline="\n")


def prepare(args, out: Path) -> dict:
    pins = json.loads((RUNTIME / "build_inputs.json").read_text(encoding="utf-8"))
    bridge = args.bridge_source.resolve()
    provenance = json.loads((bridge / "bridge-source-provenance.json").read_text(encoding="utf-8"))
    if provenance.get("translator") != pins["translator"] or provenance.get("mesa_recipe") != pins["mesa_recipe"]:
        raise ValueError("matching public bridge/Turnip recipe differs")
    source = out / "mesa"
    pin = pins["mesa_recipe"]
    run(["git", "init", source], out, "mesa-init")
    run(["git", "-C", source, "config", "core.autocrlf", "false"], out, "mesa-line-endings")
    origin = str(args.mesa_source.resolve()) if args.mesa_source else pin["mirror"]
    run(["git", "-C", source, "remote", "add", "origin", origin], out, "mesa-origin")
    run(["git", "-C", source, "fetch", "--filter=blob:none", "--depth", "1", "origin", pin["commit"]],
        out, "mesa-fetch", timeout=900)
    run(["git", "-C", source, "checkout", "--detach", "FETCH_HEAD"], out, "mesa-checkout", timeout=900)
    if run(["git", "-C", source, "rev-parse", "HEAD^{tree}"], out, "mesa-tree") != pin["tree"]:
        raise ValueError("Mesa tree differs from the exact recipe pin")
    applied = []
    for index, entry in enumerate(pins["mesa_patches"], 1):
        patch = bridge / "mesa-reference-patches" / Path(entry["path"]).name
        if not patch.is_file() or patch.stat().st_size != entry["bytes"] or sha(patch) != entry["sha256"]:
            raise ValueError("reference patch input differs: " + entry["path"])
        run(["git", "-C", source, "apply", "--check", patch], out, f"reference-{index:02d}-check")
        run(["git", "-C", source, "apply", patch], out, f"reference-{index:02d}-apply")
        applied.append(entry)
    vulkan = source / "src/freedreno/vulkan"
    overlays = {}
    for original, destination in (
        (HERE / "tu_knl_dre.cc", vulkan / "tu_knl_darwin.cc"),
        (HERE / "dre_darwin.c", vulkan / "dre_darwin.c"),
        (HERE / "dre_darwin.h", vulkan / "dre_darwin.h"),
        (HERE / "dre_mesa_policy.h", vulkan / "dre_mesa_policy.h"),
        (HERE.parent / "kernel/dre_gpu_uapi.h", vulkan / "dre_gpu_uapi.h"),
    ):
        shutil.copyfile(original, destination)
        overlays[str(destination.relative_to(source))] = sha(destination)
    meson = vulkan / "meson.build"
    replace_once(meson, "files('tu_knl_darwin.cc', 'tu_builtin_cache.cc')",
        "files('tu_knl_darwin.cc', 'tu_builtin_cache.cc', 'dre_darwin.c')")
    replace_once(meson, "  tu_link_args += ['-Wl,-undefined,dynamic_lookup']\n", "")
    header = vulkan / "tu_device.h"
    replace_once(header, "struct tu_physical_device\n{\n",
        "struct tu_physical_device\n{\n   uint64_t dre_reset_generation; /* DRE native epoch, never a fabricated ID */\n")
    knl_header = vulkan / "tu_knl.h"
    replace_once(knl_header, "VkResult tu_knl_darwin_load(struct tu_instance *instance);",
        "VkResult tu_knl_darwin_load(struct tu_instance *instance);\n"
        "void tu_dre_bo_sync_cache(struct tu_device *, struct tu_bo *, VkDeviceSize, VkDeviceSize, enum tu_mem_sync_op);")
    knl = vulkan / "tu_knl.cc"
    replace_once(knl, "   char *start = (char *) bo->map + offset;", """#ifdef TU_HAS_DARWIN
   if (!strcmp(dev->instance->knl->name, "dre-darwin")) {
      tu_dre_bo_sync_cache(dev, bo, offset, size, op);
      return;
   }
#endif
   char *start = (char *) bo->map + offset;""")
    device = vulkan / "tu_device.cc"
    replace_once(device, '#include "tu_device.h"', '#include "tu_device.h"\n#include "dre_mesa_policy.h"')
    replace_once(device, """   get_device_extensions(device, &device->vk.supported_extensions);
   tu_get_features(device, &device->vk.supported_features);
   tu_get_properties(device, &device->vk.properties);""", """   get_device_extensions(device, &device->vk.supported_extensions);
   if (tu_is_dre_darwin(instance)) tu_dre_filter_extensions(&device->vk.supported_extensions);
   tu_get_features(device, &device->vk.supported_features);
   if (tu_is_dre_darwin(instance)) tu_dre_filter_features(&device->vk.supported_features);
   tu_get_properties(device, &device->vk.properties);
   if (tu_is_dre_darwin(instance)) tu_dre_filter_properties(&device->vk.properties);""")
    replace_once(device, """         if (pdevice->emulate_second_queue == (int) i)
            p->queueFamilyProperties.queueCount = 2;""", """         if (pdevice->emulate_second_queue == (int) i)
            p->queueFamilyProperties.queueCount = 2;
         if (tu_is_dre_darwin(pdevice->instance)) {
            p->queueFamilyProperties.queueCount = 1;
            p->queueFamilyProperties.timestampValidBits = 0;
         }""")
    run(["git", "-C", source, "add", "-A"], out, "source-index")
    tree = run(["git", "-C", source, "write-tree"], out, "prepared-tree")
    return {"schema": 1, "status": "UNVALIDATED_SOURCE_PREPARED",
        "validation": "DEFERRED_UNTIL_TOMORROW", "mesa": pin,
        "matching_reference_patches": applied, "dre_overlay_sha256": overlays,
        "prepared_index_tree": tree, "required_build_platforms": [],
        "reference_a740_transport_replaced": True, "weak_undefined_link_waiver_removed": True,
        "raw_pm4_validator_present": False, "persistent_native_mappings_qualified": False,
        "compiled": False, "linked": False, "runtime_executed": False,
        "physical_device_enumerated": False, "publication_ready": False, "metal_acceleration_pass": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-source", required=True, type=Path)
    parser.add_argument("--mesa-source", type=Path, help="optional local pinned repository; copied through a new checkout")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists(): parser.error("--out must be new; earlier evidence stays immutable")
    out.mkdir(parents=True)
    try:
        result = prepare(args, out)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        result = {"status": "FAIL", "error": str(exc), "compiled": False,
            "publication_ready": False, "metal_acceleration_pass": False}
    (out / "turnip-source-provenance.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("status", "publication_ready", "metal_acceleration_pass")}))
    return int(result["status"] == "FAIL")


if __name__ == "__main__":
    sys.exit(main())
