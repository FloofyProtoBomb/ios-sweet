#!/usr/bin/env python3
"""Remove legacy fsck_hfs cache padding outside an offline HFS image.

The 540.1 Linux checker can write a full 32 KiB cache line at EOF. Only
truncate when the original primary and alternate HFS headers agree exactly
and their declared allocation geometry matches the requested image size.
Never use this while the image is mounted or an appliance owns it.
"""
import argparse
import os
from pathlib import Path
import stat
import struct


def normalize(path, expected):
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or expected <= 0 or expected % 4096:
        raise ValueError('expected a regular image and a positive 4096-byte-aligned size')
    actual = info.st_size
    if actual == expected:
        return 0
    if not expected < actual < expected + 32768 or actual != (expected + 32767) // 32768 * 32768:
        raise ValueError('unexpected image-size change; refusing to truncate')
    with path.open('r+b') as image:
        image.seek(1024); primary = image.read(512)
        image.seek(expected - 1024); alternate = image.read(512)
        if len(primary) != 512 or primary != alternate:
            raise ValueError('HFS header copies disagree; refusing to truncate')
        sig, version = struct.unpack_from('>HH', primary)
        block, total = struct.unpack_from('>II', primary, 40)
        if (sig, version) not in ((0x482b, 4), (0x4858, 5)) or block < 4096 or block & (block - 1):
            raise ValueError('invalid HFS allocation geometry')
        if block * total != expected:
            raise ValueError('declared HFS volume size differs; refusing to truncate')
        image.truncate(expected)
        image.flush()
        os.fsync(image.fileno())
    return actual - expected


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('expected_bytes', type=int)
    args = parser.parse_args()
    removed = normalize(args.image, args.expected_bytes)
    if removed:
        print(f'Removed {removed} bytes of fsck cache padding outside the declared HFS volume')
