#!/usr/bin/env python3
"""Convert the iPhone X 20H392 iBootIm resources into N200 logo bitmaps."""
import argparse
from pathlib import Path
import struct
import sys

from PIL import Image, ImageChops

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import adt


def lzss(data, expected):
    ring = bytearray(b" " * 4078 + bytes(18))
    write, pos, flags = 4078, 0, 0
    out = bytearray()
    while pos < len(data):
        flags >>= 1
        if flags < 0x100:
            flags = data[pos] | 0xff00
            pos += 1
        if flags & 1:
            byte = data[pos]
            pos += 1
            out.append(byte)
            ring[write] = byte
            write = (write + 1) & 4095
        else:
            lo, hi = data[pos:pos + 2]
            pos += 2
            start = lo | ((hi & 0xf0) << 4)
            for index in range((hi & 15) + 3):
                byte = ring[(start + index) & 4095]
                out.append(byte)
                ring[write] = byte
                write = (write + 1) & 4095
        if len(out) > expected:
            raise ValueError("iBootIm exceeds declared dimensions")
    if len(out) != expected:
        raise ValueError("truncated iBootIm")
    return bytes(out)


def layer(file):
    payload, info = adt.im4p_payload(file.read_bytes())
    if info["encrypted"] or payload[:8] != b"iBootIm\0" or payload[12:16] != b"sszl":
        raise ValueError("expected raw iBootIm/LZSS")
    width, height, x, y, size = struct.unpack_from("<HHhhI", payload, 20)
    mode = {b"yerg": "LA", b"bgra": "RGBA"}[payload[16:20]]
    if size != len(payload) - 64 or not 0 < width <= 1125 or not 0 < height <= 2436:
        raise ValueError("unexpected iBootIm extent")
    raw = lzss(payload[64:], width * height * len(mode))
    if mode == "LA":
        img = Image.frombytes(mode, (width, height), raw).convert("RGBA")
    else:
        img = Image.frombytes(mode, (width, height), raw, "raw", "BGRA")
    img.putalpha(ImageChops.invert(img.getchannel("A")))
    return img, ((1125 - width) // 2 + x, (2436 - height) // 2 + y)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("all_flash", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    assets = {
        "power-on": ["applelogo@3x~iphone"],
        "fastboot": ["recoverymode@2436~iphone-lightning"],
        "low-battery": ["batterylow0@3x~iphone", "batterylow1@3x~iphone",
                        "glyphplugin@2436~iphone-lightning"],
    }
    for name, resources in assets.items():
        canvas = Image.new("RGB", (1125, 2436))
        for resource in resources:
            picture, origin = layer(args.all_flash / (resource + ".im4p"))
            canvas.paste(picture, origin, picture.getchannel("A"))
        canvas.thumbnail((1080, 2400), Image.Resampling.LANCZOS)
        output = Image.new("RGB", (1080, 2400))
        output.paste(canvas, ((1080 - canvas.width) // 2, (2400 - canvas.height) // 2))
        output.save(args.output / (name + ".bmp"))
