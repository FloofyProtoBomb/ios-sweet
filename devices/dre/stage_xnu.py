#!/usr/bin/env python3
"""Stage DRE bring-up or CPU-rendered UI/SMP patches into darwin-xnu-build."""
import argparse
import hashlib
import json
from pathlib import Path
import re

SRC = Path(__file__).resolve().parents[2] / "xnu/patches"


UI_PATCHES = {29, 31, 32, 33, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
              46, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 61, 64, 65,
              67, 68, 70, 78}


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


def stage(out, profile="bringup"):
    # Stop immediately before the Redmi UFS driver. Keep the single-CPU
    # platform, RAM-root support and framebuffer diagnostics.
    if any(out.glob("[0-9][0-9][0-9][0-9]-*")):
        raise ValueError("DRE requires a fresh build-script patch directory")
    out.mkdir(exist_ok=True, parents=True)
    for source in sorted(SRC.iterdir()):
        if not source.name[:4].isdigit():
            continue
        number = int(source.name[:4])
        if number > 27 and (profile != "ui" or number not in UI_PATCHES):
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
        for local in (Path(__file__).parent / "ui").glob("*.patch"):
            (out / local.name).write_bytes(local.read_bytes())
    manifest = {"profile": profile, "patches": {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(out.glob("[0-9][0-9][0-9][0-9]-*.patch"))}}
    (out / "dre-patches.json").write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("patch_directory", type=Path)
    ap.add_argument("--profile", choices=("bringup", "ui"), default="bringup")
    args = ap.parse_args()
    stage(args.patch_directory, args.profile)
