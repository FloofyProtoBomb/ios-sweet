#!/usr/bin/env python3
"""Execute the real pinned patch-loop ordering in an isolated build fixture."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('build_script', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('postpatch', Path(__file__).with_name('enable-dre-native-postpatch.py'))
    hook = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hook)
    original = args.build_script.read_bytes()
    target = out/'build.sh'
    target.write_bytes(original)
    hook.enable(target)
    prepared = target.read_bytes()
    try:
        hook.enable(target)
    except ValueError:
        assert target.read_bytes() == prepared
    else:
        raise AssertionError('duplicate post-patch call accepted')
    changed = out/'changed-build.sh'
    changed.write_bytes(original.replace(b'for PATCH in', b'for ALTERED in', 1))
    before = changed.read_bytes()
    try:
        hook.enable(changed)
    except ValueError:
        assert changed.read_bytes() == before
    else:
        raise AssertionError('unknown build layout accepted')

    # Exercise the actual patched function, with only unrelated BSD sed calls
    # suppressed. Real git apply creates the late recipe's prerequisite file.
    text = prepared.decode()
    body = 'patches() {' + text.split('patches() {', 1)[1].split('\n}\n', 1)[0] + '\n}\n'
    (out/'xnu').mkdir()
    (out/'patches').mkdir()
    (out/'patches/0001-early.sh').write_text('set -eu\ntest ! -e required-source\nprintf "early\\n" >> "$WORK_DIR/order.txt"\n', newline='\n')
    (out/'patches/0001-source.patch').write_text('''diff --git a/required-source b/required-source
new file mode 100644
--- /dev/null
+++ b/required-source
@@ -0,0 +1 @@
+actual source prerequisite
''', newline='\n')
    (out/'dre-native-post-patches.sh').write_text('set -eu\ntest -f required-source\nprintf "late\\n" >> "$WORK_DIR/order.txt"\n', newline='\n')
    exercise = out/'exercise.sh'
    exercise.write_text('''#!/bin/bash
set -eu
WORK_DIR=$(cd "$(dirname "$0")" && pwd)
export WORK_DIR
CODEQL=0
MACOS_VERSION=13.5
KDKROOT=fixture
running() { :; }
warning() { printf '%s\\n' "$*" >&2; exit 1; }
error() { printf '%s\\n' "$*" >&2; exit 1; }
sed() { :; }
git -C "$WORK_DIR/xnu" init -q
''' + body + '\npatches\n', encoding='utf-8', newline='\n')
    if os.name == 'nt':
        drive, rest = exercise.as_posix().split(':/', 1)
        command = ['wsl', '-d', 'CodexDreBuild', '--', 'bash', '/mnt/'+drive.lower()+'/'+rest]
    else:
        command = ['bash', str(exercise)]
    subprocess.run(command, check=True)
    assert (out/'order.txt').read_text().splitlines() == ['early','late']
    print('PASS actual early-script / real git patch / native-late ordering and no-mutation refusals')


if __name__ == '__main__':
    main()
