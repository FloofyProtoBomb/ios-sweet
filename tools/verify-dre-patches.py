#!/usr/bin/env python3
"""Refuse a DRE artifact if darwin-xnu-build silently skipped a staged patch."""
import argparse
import json
from pathlib import Path
import re
import sys


def verify(manifest, log):
    text = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', log)
    lines = text.splitlines()
    failed = []
    for name in manifest['patches']:
        applied = any('Applying patch:' in line and line.rstrip().endswith('/' + name) for line in lines)
        skipped = any('Skipping patch ' + name in line or
                      ('Skipping patch ' in line and '/' + name in line) for line in lines)
        if not applied or skipped:
            failed.append(name)
    return failed


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('manifest', type=Path)
    ap.add_argument('log', type=Path)
    args = ap.parse_args()
    manifest = json.loads(args.manifest.read_text())
    failed = verify(manifest, args.log.read_text(errors='replace'))
    if failed:
        print('DRE patches missing or skipped:', ', '.join(failed), file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS {len(manifest['patches'])} {manifest['profile']} patches applied")
