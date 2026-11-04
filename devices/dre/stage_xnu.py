#!/usr/bin/env python3
"""Stage the single-core DRE kernel patch set into darwin-xnu-build/patches."""
import argparse
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "xnu/patches"


def stage(out):
    # Stop immediately before the Redmi UFS driver. Keep the single-CPU
    # platform, RAM-root support and framebuffer diagnostics.
    if any(out.glob("[0-9][0-9][0-9][0-9]-*")):
        raise ValueError("DRE requires a fresh build-script patch directory")
    out.mkdir(exist_ok=True, parents=True)
    for source in sorted(SRC.iterdir()):
        if not source.name[:4].isdigit() or int(source.name[:4]) > 27:
            continue
        patch = source.read_text(encoding="utf-8")
        for old, new in (("SWEET", "DRE"), ("MASTER.arm64.sweet", "MASTER.arm64.dre"),
                         ("xiaomi,sweet", "oneplus,dre"), ("qcom,kryo470", "qcom,kryo460"),
                         ("Kryo 470", "Kryo 460"), ('"sweet"', '"dre"'),
                         ("Redmi Note 10 Pro", "OnePlus Nord N200"), ("SM7150", "SM4350")):
            patch = patch.replace(old, new)
        # This shared filesystem option selects "optional sweet_hfs" entries
        # in bsd/conf/files. Renaming only its uppercase spelling enables the
        # bsd_init call but silently omits every HFS object from the link.
        patch = patch.replace("DRE_HFS", "SWEET_HFS")
        # The panel begins 18 MiB into the 32 MiB block at 0x84000000.
        # Keep the virtual and physical *pixel* pointers distinct from PTEs.
        if source.name.startswith(("0010-", "0012-")):
            patch = patch.replace("0x9c000000", "0x85200000")
            patch = patch.replace("MOV64\tx10, 0x85200000", "MOV64\tx10, 0x84000000")
            patch = patch.replace("*slot = 0x85200000ULL", "*slot = 0x84000000ULL")
            patch = patch.replace("sweet_early_relocate(DRE_EARLY_FB_VA)",
                                  "sweet_early_relocate(DRE_EARLY_FB_VA + 0x01200000)")
        (out / source.name).write_bytes(patch.encode("utf-8"))
    for local in Path(__file__).parent.glob("*.patch"):
        (out / local.name).write_bytes(local.read_bytes())


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("patch_directory", type=Path)
    stage(ap.parse_args().patch_directory)
