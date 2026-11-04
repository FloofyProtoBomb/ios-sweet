#!/usr/bin/env python3
"""Replace normal boot art in the verified DE2118 DDPH logo container, offline."""
import argparse
import gzip
import hashlib
from pathlib import Path
import struct

ORIGINAL = "0c2c62faca3a4e65ca2943fa429264b9a7adf90e610b125514f266a833ca6aa9"
NORMAL = {"boot", "boot_without_android", "ExpSellLogo", "cmcc_with_android"}


def replace(original, bitmap, fastboot=None, low_battery=None):
    if hashlib.sha256(original).hexdigest() != ORIGINAL:
        raise ValueError("unrecognized logo partition layout")
    image = bytearray(original)
    count, = struct.unpack_from("<I", original, 0x410c)
    matched = set()
    replacements = dict.fromkeys(NORMAL, bitmap)
    if fastboot is not None:
        replacements["fastboot"] = fastboot
    if low_battery is not None:
        replacements["boot_charger_low_battery3"] = low_battery
    payload = bytearray()
    old_end = 0
    for i in range(count):
        pos = 0x4120 + 128 * i
        offset, raw_size, capacity = struct.unpack_from("<III", original, pos)
        start = 0x8000 + offset
        name = image[pos + 12:pos + 128].split(b"\0")[0].decode()
        member = original[start:start + capacity]
        old_end = max(old_end, offset + capacity)
        if name not in replacements:
            struct.pack_into("<I", image, pos, len(payload))
            payload.extend(member)
            continue
        old = gzip.decompress(member)
        bitmap = replacements[name]
        if len(old) != raw_size or len(bitmap) != raw_size or old[:2] != b"BM":
            raise ValueError("unexpected BMP size")
        # Preserve the original BMP metadata and the bottom-up BGR24 layout.
        if struct.unpack_from("<iiHHI", bitmap, 18) != (1080, 2400, 1, 24, 0):
            raise ValueError("expected a 1080 x 2400 uncompressed 24-bit BMP")
        pixel_offset, = struct.unpack_from("<I", old, 10)
        if struct.unpack_from("<I", bitmap, 10)[0] != pixel_offset:
            raise ValueError("BMP pixel offsets do not match")
        new = old[:pixel_offset] + bitmap[pixel_offset:]
        stream = gzip.compress(new, compresslevel=9, mtime=0)
        if gzip.decompress(stream) != new:
            raise ValueError("repacked splash failed verification")
        struct.pack_into("<III", image, pos, len(payload), raw_size, len(stream))
        payload.extend(stream)
        matched.add(name)
    if matched != set(replacements):
        raise ValueError("missing a requested boot resource")
    extent = max(len(payload), old_end)
    if 0x8000 + extent > len(original) or any(original[0x8000 + old_end:0x8000 + extent]):
        raise ValueError("replacement would exceed available zero padding")
    image[0x8000:0x8000 + extent] = payload.ljust(extent, b"\0")
    return image


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    for arg in ("original", "bitmap", "output"):
        ap.add_argument(arg, type=Path)
    ap.add_argument("--fastboot", type=Path)
    ap.add_argument("--low-battery", type=Path)
    args = ap.parse_args()
    if args.original.resolve() == args.output.resolve():
        ap.error("preserve the backup and choose a different output path")
    args.output.write_bytes(replace(args.original.read_bytes(), args.bitmap.read_bytes(),
                                    args.fastboot.read_bytes() if args.fastboot else None,
                                    args.low_battery.read_bytes() if args.low_battery else None))
