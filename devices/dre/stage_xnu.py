#!/usr/bin/env python3
"""Stage DRE bring-up or CPU-rendered UI/SMP patches into darwin-xnu-build."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import difflib
import importlib.util

SRC = Path(__file__).resolve().parents[2] / "xnu/patches"


UI_PATCHES = {29, 31, 32, 33, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
              46, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 61, 64, 65,
              67, 68, 70, 78}


def native_storage_assets():
    """Generate additions from reviewed shared sources, avoiding forked engines."""
    here = Path(__file__).parent
    common = ('native_ufs.c', 'storage_abi.c', 'storage_abi.h', 'storage_live.h', 'ufs_read.c', 'ufs_read.h',
              'ufs_handoff_dma.c', 'ufs_handoff_dma.h', 'ufs_clock.c', 'ufs_clock.h',
              'dma_route.c', 'dma_route.h', 'dma_context.c', 'dma_context.h',
              'dma_memory.c', 'dma_memory.h', 'qemu_disk.c', 'qemu_disk.h')
    libfdt = ('fdt.c', 'fdt_ro.c', 'fdt_check.c', 'fdt_addresses.c',
              'libfdt.h', 'fdt.h', 'libfdt_env.h', 'libfdt_internal.h')
    result = []
    for name in (*common, *(f'libfdt/{n}' for n in libfdt)):
        content = (here / name).read_text()
        if name.startswith('libfdt/'):
            content = content.replace('<libfdt.h>', '"libfdt.h"').replace('<fdt.h>', '"fdt.h"')
            content = content.replace('<libfdt_env.h>', '"libfdt_env.h"')
            if name.endswith('libfdt_env.h'):
                content = content.replace('#include <stdlib.h>\n#include <string.h>', '#include <libkern/libkern.h>')
                content = content.replace('#include <limits.h>', '#include <machine/limits.h>')
            if name.endswith('fdt_addresses.c'):
                # Kernel reads only; exclude the writer and its dependencies.
                content = content.split('/* This function assumes')[0].rstrip() + '\n'
        else:
            content = content.replace('<libfdt.h>', '"libfdt/libfdt.h"')
            if name.endswith('.c'):
                content = '#define DRE_UFS_KERNEL 1\n' + content
        target = 'bsd/dev/dre/' + name
        result.extend(difflib.unified_diff([], content.splitlines(True),
                                         fromfile='/dev/null', tofile='b/' + target))
    return ''.join(result)


def select_files(patch, paths):
    """Keep complete unified-diff file sections; never alter hunk counts."""
    starts = [m.start() for m in re.finditer(r'^--- ', patch, re.M)]
    kept = []
    for i, start in enumerate(starts):
        part = patch[start:starts[i + 1] if i + 1 < len(starts) else len(patch)]
        part = part.split('\ndiff --git ', 1)[0]
        target = re.search(r'^\+\+\+ b/([^\n\t]+)', part, re.M)
        if target and target[1] in paths:
            # A final context line containing one space is still part of
            # the hunk. Strip line endings only, never that marker.
            kept.append(part.rstrip('\r\n') + '\n')
    if not kept:
        raise ValueError('selected patch section is missing')
    return ''.join(kept)


def stage(out, profile="bringup", native_drafts=(), native_gpu_nc_proof=False,
          native_gpu_firmware_draft=False, native_gpu_fw_nc_proof=False):
    # UNVALIDATED overnight feature integration. Existing profiles do not
    # import, prepare or include these sources without an explicit selection.
    native_drafts = tuple(dict.fromkeys(native_drafts))
    if native_drafts and (profile != "ui" or
                          any(feature not in ("gpu", "touch") for feature in native_drafts)):
        raise ValueError("native drafts require the UI profile and explicit gpu/touch selections")
    if native_gpu_nc_proof and 'gpu' not in native_drafts:
        raise ValueError("native GPU NC proof requires the explicit GPU draft")
    if native_gpu_firmware_draft and 'gpu' not in native_drafts:
        raise ValueError("native firmware sources require the explicit GPU draft")
    if native_gpu_fw_nc_proof and not native_gpu_firmware_draft:
        raise ValueError("boot-owned NC proof requires the explicit firmware draft")
    # Stop immediately before the Redmi UFS driver. Keep the single-CPU
    # platform, RAM-root support and framebuffer diagnostics.
    if any(out.glob("[0-9][0-9][0-9][0-9]-*")):
        raise ValueError("DRE requires a fresh build-script patch directory")
    out.mkdir(exist_ok=True, parents=True)
    # Native batching uses the real edge-contact state introduced by0071.
    # Keep this additional prerequisite opt-in; ordinary UI/bringup selection
    # does not acquire a new native input producer or alter its patch list.
    selected_ui = UI_PATCHES | ({71} if 'touch' in native_drafts else set())
    for source in sorted(SRC.iterdir()):
        if not source.name[:4].isdigit():
            continue
        number = int(source.name[:4])
        if number > 27 and (profile != "ui" or number not in selected_ui):
            continue
        # The imported IOHIDFamily patch retains legacy bytes in comments.
        # Preserve them while changing only ASCII machine/profile strings.
        patch = source.read_bytes().decode("utf-8", errors="surrogateescape")
        if profile == "ui":
            # These upstream patches mix generic UI fixes with Redmi UFS
            # logging/sampling. Retain only the needed, hardware-free files.
            if number == 54:
                patch = select_files(patch, {"iokit/Kernel/arm/SweetFramebuffer.cpp"})
            elif number == 55:
                patch = select_files(patch, {"iokit/Kernel/arm/SweetKeyStore.cpp"})
            elif number == 67:
                patch = select_files(patch, {"iokit/Kernel/arm/SweetFramebuffer.cpp"})
        for old, new in (("SWEET", "DRE"), ("MASTER.arm64.sweet", "MASTER.arm64.dre"),
                         ("xiaomi,sweet", "oneplus,dre"), ("qcom,kryo470", "qcom,kryo460"),
                         ("Kryo 470", "Kryo 460"), ('"sweet"', '"dre"'),
                         ("Redmi Note 10 Pro", "OnePlus Nord N200"), ("SM7150", "SM4350")):
            patch = patch.replace(old, new)
        # This shared filesystem option selects "optional sweet_hfs" entries
        # in bsd/conf/files. Renaming only its uppercase spelling enables the
        # bsd_init call but silently omits every HFS object from the link.
        patch = patch.replace("DRE_HFS", "SWEET_HFS")
        if profile == "ui":
            patch = patch.replace("sweet,display", "dre,display")
        # The panel begins 18 MiB into the 32 MiB block at 0x84000000.
        # Keep the virtual and physical *pixel* pointers distinct from PTEs.
        if source.name.startswith(("0010-", "0012-")):
            patch = patch.replace("0x9c000000", "0x85200000")
            patch = patch.replace("MOV64\tx10, 0x85200000", "MOV64\tx10, 0x84000000")
            patch = patch.replace("*slot = 0x85200000ULL", "*slot = 0x84000000ULL")
            patch = patch.replace("sweet_early_relocate(DRE_EARLY_FB_VA)",
                                  "sweet_early_relocate(DRE_EARLY_FB_VA + 0x01200000)")
        (out / source.name).write_bytes(patch.encode("utf-8", errors="surrogateescape"))
    for local in Path(__file__).parent.glob("*.patch"):
        if profile == "ui" and local.name.startswith("0059-"):
            # The UI profile has the original 0059, whose context includes
            # the arm-io nubs from 0046. The early backport targets 0027.
            continue
        (out / local.name).write_bytes(local.read_bytes())
    if profile == "ui":
        here = Path(__file__).parent
        ui_patches = list((here / "ui").glob("*.patch"))
        # This reviewed runtime patch fixes the common IOSurface implementation;
        # it neither starts Adreno nor advertises a native Metal provider.
        ui_patches.append(here / "gpu/0104-dre-iosurface-lifetime.patch")
        for local in sorted(ui_patches):
            content = local.read_bytes()
            if local.name.startswith('0103-'):
                content += native_storage_assets().encode()
            (out / local.name).write_bytes(content)
    draft_manifest = None
    if native_drafts:
        helper = Path(__file__).with_name('native_drafts.py')
        spec = importlib.util.spec_from_file_location('dre_native_draft_source_recipe', helper)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        draft_manifest = module.stage_native_drafts(Path(__file__).parent, out, native_drafts,
                                                    native_gpu_nc_proof,
                                                    native_gpu_firmware_draft,
                                                    native_gpu_fw_nc_proof)
    manifest = {"profile": profile, "patches": {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(out.glob("[0-9][0-9][0-9][0-9]-*.patch"))}}
    if draft_manifest is not None:
        manifest['native_drafts'] = draft_manifest
    (out / "dre-patches.json").write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("patch_directory", type=Path)
    ap.add_argument("--profile", choices=("bringup", "ui"), default="bringup")
    ap.add_argument("--native-draft", choices=("gpu", "touch"), action="append", default=[],
                    help="UNVALIDATED opt-in source integration; repeat for both (UI only)")
    ap.add_argument("--native-gpu-nc-proof", action="store_true",
                    help="UNVALIDATED XNU8796 private pmap observation source (GPU draft only)")
    ap.add_argument("--native-gpu-firmware-draft", action="store_true",
                    help="UNVALIDATED firmware source only; supplies no private anchor or firmware")
    ap.add_argument("--native-gpu-fw-nc-proof", action="store_true",
                    help="UNVALIDATED boot-owned low-DDR pmap observation (firmware draft only)")
    args = ap.parse_args()
    stage(args.patch_directory, args.profile, args.native_draft, args.native_gpu_nc_proof,
          args.native_gpu_firmware_draft, args.native_gpu_fw_nc_proof)
