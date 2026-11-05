#!/usr/bin/env python3
"""UNVALIDATED: prepare private pinned GPU inputs for a FUTURE opt-in build.

This source has not been run. It neither builds nor flashes any image.
The output contains private firmware and MUST stay outside the repository.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]
ALIGN = 16384
CHIP = 0x06010900
PACKAGE_MAGIC = 0x3146555047455244
TRUST_MAGIC = 0x3152545746455244
MAX_FILE = 1024 * 1024


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def round_up(value: int) -> int:
    return (value + ALIGN - 1) & -ALIGN


def digest(text: object) -> bytes:
    if not isinstance(text, str) or not re.fullmatch(r"[0-9a-f]{64}", text) or text == "0" * 64:
        raise ValueError("a nonzero independently pinned lowercase SHA256 is required")
    return bytes.fromhex(text)


def private_output(path: Path) -> Path:
    path = path.resolve()
    if path == REPO or path.is_relative_to(REPO):
        raise ValueError("private firmware output must be outside the source repository")
    return path


def read_pinned(base: Path, item: dict) -> tuple[bytes, bytes]:
    if set(item) != {"path", "bytes", "sha256"}:
        raise ValueError("firmware descriptor requires exactly path, bytes, sha256")
    path = Path(item["path"])
    if not path.is_absolute():
        path = base / path
    count = item["bytes"]
    if type(count) is not int or not 0 < count <= MAX_FILE or path.stat().st_size != count:
        raise ValueError("firmware length does not match its pinned descriptor")
    expected = digest(item["sha256"])
    data = path.read_bytes()
    if len(data) != count or hashlib.sha256(data).digest() != expected:
        raise ValueError("private firmware digest mismatch")
    return data, expected


def package(sqe: bytes, sqe_hash: bytes, zap: bytes, zap_hash: bytes) -> bytes:
    sqe_off = ALIGN
    zap_off = round_up(sqe_off + len(sqe))
    total = round_up(zap_off + len(zap))
    out = bytearray(total)
    struct.pack_into("<QIIQII", out, 0, PACKAGE_MAGIC, 1, 256, total, CHIP, 2)
    for number, off, data, file_hash in ((1, sqe_off, sqe, sqe_hash), (2, zap_off, zap, zap_hash)):
        pos = 64 + (number - 1) * 80
        struct.pack_into("<IIQQ", out, pos, number, 0, off, len(data))
        out[pos + 24:pos + 56] = file_hash
        out[off:off + len(data)] = data
    return bytes(out)


def cbytes(data: bytes) -> str:
    return "{" + ",".join(f"0x{x:02x}" for x in data) + "}"


def prepare(profile_path: Path, directory: Path) -> None:
    if profile_path.stat().st_size > 65536:
        raise ValueError("private firmware profile is unexpectedly large")
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    if set(profile) != {"schema", "chip_id", "sqe", "zap"} or profile["schema"] != 1:
        raise ValueError("unsupported private firmware profile")
    chip = profile["chip_id"]
    if (int(chip, 0) if isinstance(chip, str) else chip) != CHIP:
        raise ValueError("private firmware profile must target actual A619v1")
    sqe, sqe_hash = read_pinned(profile_path.resolve().parent, profile["sqe"])
    zap, zap_hash = read_pinned(profile_path.resolve().parent, profile["zap"])
    if len(sqe) < 16 or len(sqe) % 4 or not zap.startswith(b"\x7fELF\x01\x01\x01"):
        raise ValueError("expected full SQE file and full ELF32 little-endian ZAP, not split MDT")
    packed = package(sqe, sqe_hash, zap, zap_hash)
    packed_hash = hashlib.sha256(packed).digest()
    anchor = struct.pack("<QIIIIQQQ", TRUST_MAGIC, 1, 144, CHIP, 0,
                         len(packed), len(sqe), len(zap)) + packed_hash + sqe_hash + zap_hash
    header = ("/* PRIVATE GENERATED INPUT. Do not commit or upload firmware/anchor files. */\n"
              "#define DRE_GPU_FW_TRUST_INITIALIZER { \\\n"
              f"UINT64_C(0x{TRUST_MAGIC:016x}),1,144,UINT32_C(0x{CHIP:08x}),0, \\\n"
              f"UINT64_C({len(packed)}),UINT64_C({len(sqe)}),UINT64_C({len(zap)}), \\\n"
              f"{cbytes(packed_hash)}, \\\n{cbytes(sqe_hash)}, \\\n{cbytes(zap_hash)} }}\n").encode("ascii")
    out = private_output(directory)
    # A fresh directory prevents accidental mutation of previous prepared
    # firmware, image, root or device-backup artifacts.
    out.mkdir(parents=True, exist_ok=False)
    (out / "gpu-firmware.package.bin").write_bytes(packed)
    (out / "gpu-firmware.anchor.bin").write_bytes(anchor)
    (out / "gpu-firmware.trust.private.h").write_bytes(header)
    report = {"schema": 1, "chip_id": CHIP, "contains_private_firmware": True,
              "validation": "UNVALIDATED_SOURCE_RECIPE", "build_executed": False,
              "native_gpu_execution": False, "metal_ready": False,
              "package_bytes": len(packed), "package_sha256": sha(packed),
              "anchor_bytes": len(anchor), "anchor_sha256": sha(anchor),
              "header_sha256": sha(header), "sqe_bytes": len(sqe), "zap_bytes": len(zap),
              "sqe_sha256": sqe_hash.hex(), "zap_sha256": zap_hash.hex()}
    (out / "provisioning.private.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def load_for_build(directory: Path, kernel: Path, out: Path) -> bytes:
    """Called only by future explicitly opted-in build.py execution.

    A matching private anchor must ALREADY be linked into the actual XNU.
    Loading a self-describing firmware package never creates a kernel trust
    anchor or authorizes PAS13, clocks, reserved RAM, or any device access.
    """
    private_output(out)
    private_output(directory)
    if (directory / "provisioning.private.json").stat().st_size > 65536 or \
       (directory / "gpu-firmware.package.bin").stat().st_size > 3 * 1024 * 1024 or \
       (directory / "gpu-firmware.trust.private.h").stat().st_size > 65536:
        raise ValueError("private provisioning artifacts exceed bounded sizes")
    p = json.loads((directory / "provisioning.private.json").read_text(encoding="utf-8"))
    if p.get("schema") != 1 or p.get("chip_id") != CHIP:
        raise ValueError("unsupported private provisioning record")
    packed = (directory / "gpu-firmware.package.bin").read_bytes()
    anchor = (directory / "gpu-firmware.anchor.bin").read_bytes()
    header = (directory / "gpu-firmware.trust.private.h").read_bytes()
    if len(packed) != p["package_bytes"] or sha(packed) != p["package_sha256"] or \
       len(anchor) != 144 or sha(anchor) != p["anchor_sha256"] or sha(header) != p["header_sha256"]:
        raise ValueError("private provisioning artifacts changed")
    fields = struct.unpack_from("<QIIIIQQQ", anchor)
    if fields != (TRUST_MAGIC, 1, 144, CHIP, 0, len(packed), p["sqe_bytes"], p["zap_bytes"]) or \
       anchor[48:80] != hashlib.sha256(packed).digest():
        raise ValueError("private anchor does not match package")
    if kernel.read_bytes().count(anchor) != 1:
        raise ValueError("actual XNU must contain exactly one matching privately compiled firmware anchor")
    return packed


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--profile", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    prepare(args.profile, args.out)


if __name__ == "__main__":
    main()
