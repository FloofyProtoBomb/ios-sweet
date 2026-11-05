#!/usr/bin/env python3
"""Make a new journaled HFSX QEMU fixture with a supplied static PID 1.

Linux host only, no Apple files, no device nodes or phone access. Never reuses
an output directory or image. Compile hfs-root-init.S and wrap with mkinit.py
first; this keeps the image maker independent of the host compiler platform.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from install_readiness import digest, inspect_hfs, macho_arm64, require

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--init', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--size-mib', type=int, default=64)
    args = parser.parse_args()
    require(sys.platform.startswith('linux'), 'requires Linux guestfish/hfsprogs')
    require(32 <= args.size_mib <= 512, 'fixture size must be between 32 and 512 MiB')
    macho_arm64(args.init)
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    image = out / 'hfs-fixture.img'; size = args.size_mib << 20
    with image.open('xb') as stream:
        stream.truncate(size)
    def run(command, log):
        result = subprocess.run([str(value) for value in command], capture_output=True, check=False)
        (out / log).write_bytes(result.stdout + result.stderr)
        return result.returncode
    require(run(['mkfs.hfsplus', '-s', '-J', '-v', 'DRE-QEMU-HFS', image], 'mkfs.log') == 0,
            'fixture formatting failed')
    require(run(['guestfish', '--rw', '--format=raw', '-a', image, 'run', ':',
                 'mount-options', 'rw,force', '/dev/sda', '/', ':', 'mkdir-p', '/sbin', ':',
                 'mkdir-p', '/dev', ':', 'upload', args.init.resolve(), '/sbin/launchd', ':',
                 'chmod', '0755', '/sbin/launchd', ':', 'chown', '0', '0', '/sbin/launchd', ':',
                 'umount-all', ':', 'sync'], 'guestfish.log') == 0, 'fixture upload failed')
    clean = False
    for attempt in range(3):
        require(run(['fsck.hfsplus', '-fy', image], f'fsck-repair-{attempt}.log') in (0, 1),
                'fixture filesystem repair failed')
        if run(['fsck.hfsplus', '-fn', image], f'fsck-readonly-{attempt}.log') == 0:
            clean = True; break
    require(clean, 'fixture failed readonly filesystem verification')
    geometry = inspect_hfs(image, size)
    result = {'schema_version':1, 'environment':'QEMU-only disposable fixture',
              'image_sha256':digest(image), 'image_bytes':size, 'pid1_sha256':digest(args.init),
              'hfs':geometry, 'apple_files_included':False, 'phone_accessed':False}
    (out / 'fixture.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
