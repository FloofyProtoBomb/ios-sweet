#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""UNVALIDATED, UNRUN selected-SweetSurface source-overlay recipe.

Do not execute during the source-only overnight period. For deferred staging,
copy an already selected DRE SweetSurface.cpp (0046/0068/0104), retain original
bytes/hash, insert real backing bridge hooks, and copy our source into a new
overlay directory. No source tree is modified, compiler/test/recipe subprocess
is invoked, kernel provider is published, or image/phone is accessed.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
KERNEL = HERE.parent / "kernel"
ARM = Path("iokit/Kernel/arm")
BRIDGE = ARM / "dre/gpu/kernel"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise ValueError("selected SweetSurface source context changed: " + repr(old[:100]))
    return text.replace(old, new, 1)


def prepare(source: Path, out: Path) -> dict:
    if source.is_symlink() or not source.is_file() or source.name != "SweetSurface.cpp":
        raise ValueError("--source must be the actual selected ordinary SweetSurface.cpp")
    raw = source.read_bytes()
    text = raw.decode("utf-8", errors="strict")
    if "dre_surface_backing.h" in text or "dre_sweet_surface_bridge.inc" in text:
        raise ValueError("bridge already present; use a fresh selected source")
    for marker in (
        "#if defined(DRE)", "IOCoreSurfaceRoot::start(IOService *provider)",
        "sweet_surface_get(SweetSurface *s)", "sweet_surface_put(SweetSurface *s)",
        "copyObjectForPortNameInTask", "IOSurfaceRootUserClient::trapIncrementUse",
        "ref->useCount[category]", "kIOReturnUnsupported",
        "s->mapCache = kIOMapDefaultCache;",
    ):
        if marker not in text:
            raise ValueError("expected selected DRE/lifetime source marker missing: " + marker)
    include = '#include <IOKit/IOService.h>\n'
    text = replace_once(text, include, include +
        '#include "dre/gpu/kernel/dre_surface_backing.h"\n'
        'static int dre_sweet_surface_bridge_install(IOService *);\n')
    startup = '\tsweet_surface_init_lock();\n\tsetProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");'
    text = replace_once(text, startup,
        '\tsweet_surface_init_lock();\n'
        '\tint backingStatus = dre_sweet_surface_bridge_install(this);\n'
        '\tif (backingStatus != DRE_GPU_OK) {\n'
        '\t\tIOLog("DRE SURF: retained-backing bridge unavailable: %d\\n", backingStatus);\n'
        '\t}\n'
        '\tsetProperty(kIOUserClientClassKey, "IOSurfaceRootUserClient");')
    text += ('\n/* UNVALIDATED opt-in retained backing bridge; not GPU import. */\n'
        '#if defined(DRE)\n'
        '#include "dre/gpu/kernel/dre_sweet_surface_bridge.inc"\n'
        '#endif\n')
    (out / ARM).mkdir(parents=True)
    (out / "original").mkdir()
    (out / "original/SweetSurface.cpp").write_bytes(raw)
    (out / ARM / source.name).write_bytes(text.encode("utf-8"))
    (out / BRIDGE).mkdir(parents=True)
    overlays = {str(ARM / source.name): digest(text.encode("utf-8"))}
    for name in ("dre_gpu_uapi.h", "dre_surface_backing.h", "dre_surface_backing.cpp", "dre_sweet_surface_bridge.inc"):
        path = KERNEL / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("ordinary owned bridge source missing: " + name)
        destination = out / BRIDGE / name
        shutil.copyfile(path, destination)
        overlays[str(BRIDGE / name)] = digest(destination.read_bytes())
    return {"schema": 1, "status": "UNVALIDATED_SOURCE_OVERLAY_PREPARED",
        "original_selected_source_sha256": digest(raw), "overlay_sha256": overlays,
        "build_list_entry_required": str(BRIDGE / "dre_surface_backing.cpp"),
        "abi_version": 1, "new_selectors": [13, 14, 15, 16],
        "existing_selector_layouts_changed": False, "source_recipe_qualified": False,
        "validation": "DEFERRED_UNTIL_TOMORROW", "compiled": False, "executed": False,
        "surface_gpu_import_supported": False, "vulkan_enumerated": False,
        "publication_ready": False, "metal_acceleration_pass": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists(): parser.error("--out must be new; original sources/reports remain immutable")
    out.mkdir(parents=True)
    try:
        report = prepare(args.source.absolute(), out)
    except (OSError, ValueError, KeyError) as exc:
        report = {"schema": 1, "status": "FAIL", "error": str(exc),
            "source_recipe_qualified": False, "compiled": False,
            "surface_gpu_import_supported": False, "metal_acceleration_pass": False}
    (out / "surface-bridge-source-provenance.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("status", "compiled", "metal_acceleration_pass")}))
    return int(report["status"] == "FAIL")


if __name__ == "__main__":
    sys.exit(main())
