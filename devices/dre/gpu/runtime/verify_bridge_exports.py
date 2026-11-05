#!/usr/bin/env python3
"""Check a real static archive against actual SDK frontend imports (names only)."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def compare(contract: dict, symbols: str) -> dict:
    required = contract.get("imported_bridge_symbol_names")
    if (not isinstance(required, list) or not required or
        any(not isinstance(x, str) or not re.fullmatch(r"nmb_[A-Za-z0-9_]+", x) for x in required) or
        len(required) != len(set(required))):
        raise ValueError("invalid/empty actual frontend import contract")
    # --defined-only means U records are absent. Require external text symbols;
    # a data object with the same name cannot satisfy a C function import.
    exported = set()
    duplicates = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[1] != "T":
            continue
        name = fields[0]
        if name.startswith("_nmb_"):
            name = name[1:]  # Mach-O C decoration, not arbitrary underscoring.
        if not re.fullmatch(r"nmb_[A-Za-z0-9_]+", name):
            continue
        if name in exported:
            duplicates.add(name)
        exported.add(name)
    missing = sorted(set(required) - exported)
    if missing or duplicates:
        raise ValueError(f"archive exports mismatch: missing={missing}, duplicates={sorted(duplicates)}")
    return {"status": "BINARY_BRIDGE_SYMBOL_NAMES_MATCH", "required_import_count": len(required),
        "exported_bridge_function_count": len(exported), "missing": [],
        "extra_bridge_function_names": sorted(exported - set(required)),
        "symbol_names_only_checked": True, "binary_abi_link_checked": False,
        "runtime_executed": False, "publication_ready": False, "metal_acceleration_pass": False}


def verify(archive: Path, contract: Path, nm: Path, out: Path) -> dict:
    with archive.open("rb") as stream:
        if stream.read(8) != b"!<arch>\n":
            raise ValueError("input is not a real static archive")
    args = [str(nm), "--extern-only", "--defined-only", "--format=posix", str(archive)]
    result = subprocess.run(args, capture_output=True, text=True, timeout=120, check=True)
    (out / "bridge-exports.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    report = compare(json.loads(contract.read_text(encoding="utf-8")), result.stdout)
    report.update(archive_sha256=digest(archive), archive_bytes=archive.stat().st_size,
        frontend_contract_sha256=digest(contract), nm_sha256=digest(nm))
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--contract", required=True, type=Path)
    parser.add_argument("--llvm-nm", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    if args.out.exists():
        parser.error("--out must be new to preserve previous evidence")
    args.out.mkdir(parents=True)
    try:
        report = verify(args.archive, args.contract, args.llvm_nm, args.out)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        report = {"status": "FAIL", "error": str(exc), "publication_ready": False,
            "metal_acceleration_pass": False}
    (args.out / "bridge-export-verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))
    return int(report["status"] == "FAIL")


if __name__ == "__main__":
    sys.exit(main())
