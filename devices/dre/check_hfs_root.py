#!/usr/bin/env python3
"""Independently read the fixture's file from its stopped QEMU scratch image.

Uses a read-only libguestfs appliance; never repairs or mutates the image.
This checks host file persistence, not physical UFS or host fsync durability.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from install_readiness import digest, require
from install_metal_probe import require_offline
from qemu_root import CHECK_CONTENT


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    require(not args.out.exists(), 'refusing to overwrite a previous proof')
    require_offline(args.image)
    before = digest(args.image)
    with tempfile.TemporaryDirectory(prefix='dre-hfs-readback-') as temporary:
        copied = Path(temporary) / 'readback.bin'
        subprocess.run(['guestfish', '--ro', '--format=raw', '-a', str(args.image.resolve()), 'run', ':',
                        'mount-options', 'ro,force', '/dev/sda', '/', ':', 'download', '/dre-hfs-rw-check',
                        str(copied), ':', 'umount-all'], check=True)
        contents = copied.read_bytes()
        require(contents == CHECK_CONTENT, 'persisted file differs from the PID1 test contents')
    require(digest(args.image) == before, 'readonly verification changed the scratch image')
    result = {'status':'PASS', 'environment':'QEMU disposable fixture, independent readonly appliance',
              'scratch_image_sha256':before, 'file':'/dre-hfs-rw-check',
              'file_sha256':hashlib.sha256(contents).hexdigest(), 'file_bytes':len(contents),
              'image_modified_by_verifier':False, 'phone_tested':False,
              'host_hardware_durability_proven':False}
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
