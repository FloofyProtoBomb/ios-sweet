#!/usr/bin/env python3
"""Run native source bindings after patches in the pinned build script.

darwin-xnu-build runs *.sh before *.patch. The native binding recipe needs
the classes introduced by those patches, so the workflow moves it outside
that early script glob and installs this explicit post-patch call.
"""
import argparse
from pathlib import Path

MARKER = b'DRE_NATIVE_POSTPATCH_V1'
ANCHOR = b'''        for PATCH in "${PATCH_DIR}"/*.patch; do
            if APPLY_CHECK=$(git apply --check "$PATCH" 2>&1); then
                running "Applying patch: ${PATCH}"
                git apply "$PATCH"
            else
                warning "Skipping patch ${PATCH} (already upstream, previously applied, or malformed): ${APPLY_CHECK}"
            fi
        done
        cd "${WORK_DIR}"
'''
INSERT = b'''        # DRE_NATIVE_POSTPATCH_V1: current directory is the patched XNU tree.
        running "Running DRE native post-patch bindings"
        KDKROOT="${KDKROOT}" bash "${WORK_DIR}/dre-native-post-patches.sh"
'''


def enable(path):
    path = Path(path)
    raw = path.read_bytes()
    if MARKER in raw:
        raise ValueError('native post-patch call already present')
    if raw.count(ANCHOR) != 1:
        raise ValueError('pinned build-script patch loop changed; no mutation performed')
    before = b'        cd "${WORK_DIR}"\n'
    replacement = ANCHOR.replace(before, INSERT + before, 1)
    path.write_bytes(raw.replace(ANCHOR, replacement, 1))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('build_script', type=Path)
    args = ap.parse_args()
    enable(args.build_script)
