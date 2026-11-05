#!/usr/bin/env python3
"""Fetch, hash-check and install the exact LLVM21 bottle on ARM64 macOS15.

No source fallback or cross-OS bottle substitution. The committed pin is
metadata, not an installation claim. Actual receipt/tool proof is emitted only
after this helper runs successfully on the matching host.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

from build_frontend import HERE, write_json
from verify_bridge_exports import digest


def validate_host(system: str, arch: str, version: str, pin: dict) -> None:
    if (system != "Darwin" or arch != pin["host_arch"] or
        not re.fullmatch(r"15(?:\.\d+){0,2}", version)):
        raise ValueError("this pin requires actual ARM64 macOS15 Sequoia")


def validate_formula(info: dict, pin: dict) -> dict:
    formulae = info.get("formulae", [])
    if len(formulae) != 1:
        raise ValueError("expected one Homebrew formula")
    f = formulae[0]
    bottle = f.get("bottle", {}).get("stable", {})
    actual = bottle.get("files", {}).get(pin["bottle_tag"], {})
    if (f.get("name") != pin["formula"] or f.get("versions", {}).get("stable") != pin["version"] or
        f.get("revision") != pin["revision"] or f.get("version_scheme") != pin["version_scheme"] or
        bottle.get("rebuild") != pin["bottle_rebuild"] or
        actual.get("sha256") != pin["sha256"] or actual.get("url") != pin["url"]):
        raise ValueError("Homebrew formula/bottle changed from the qualified metadata pin")
    return f


def validate_installed_formula(formula: dict, pin: dict) -> dict:
    expected = pin["version"] + "_" + str(pin["revision"])
    installed = [entry for entry in formula.get("installed", []) if entry.get("version") == expected]
    if (len(installed) != 1 or installed[0].get("built_as_bottle") is not True or
        installed[0].get("poured_from_bottle") is not True or installed[0].get("used_options") != []):
        raise ValueError("formula does not report the exact installed bottle package version")
    return installed[0]


def validate_receipt(receipt: dict, prefix: Path, pin: dict) -> None:
    if (prefix.name != pin["version"] + "_" + str(pin["revision"]) or
        prefix.parent.name != pin["formula"] or receipt.get("built_as_bottle") is not True or
        receipt.get("poured_from_bottle") is not True or receipt.get("arch") != pin["host_arch"]):
        raise ValueError("installed keg is not the matching ARM64 bottle revision")
    source = receipt.get("source", {})
    # Homebrew Tab.create stores source.versions.stable/version_scheme, spec
    # and tap, NOT source.revision. The exact revision is checked through the
    # real formula metadata, installed package version, resolved keg and bottle
    # digest. Do not invent a receipt field or silently accept source drift.
    versions = source.get("versions", {})
    if (versions.get("stable") != pin["version"] or versions.get("version_scheme") != pin["version_scheme"] or
        source.get("spec") != "stable" or source.get("tap") != "homebrew/core" or
        ("revision" in source and source["revision"] != pin["revision"])):
        observed = {"stable": versions.get("stable"), "version_scheme": versions.get("version_scheme"),
            "spec": source.get("spec"), "tap": source.get("tap"), "revision": source.get("revision")}
        raise ValueError("installed bottle receipt source differs: " + json.dumps(observed, sort_keys=True))


def acquire(args, out: Path) -> dict:
    pin = json.loads((HERE / "llvm21-macos.json").read_text(encoding="utf-8"))
    if sys.platform != "darwin":
        raise ValueError("actual macOS is required; this host cannot qualify the bottle")
    env = dict(os.environ, HOMEBREW_NO_AUTO_UPDATE="1", HOMEBREW_NO_INSTALL_CLEANUP="1")

    def run(command, name, timeout=180):
        result = subprocess.run([str(x) for x in command], env=env, capture_output=True,
            text=True, check=False, timeout=timeout)
        (out / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            raise ValueError(f"{name} failed ({result.returncode}); see its log")
        return result.stdout.strip()

    version = run(["sw_vers", "-productVersion"], "macos-version")
    validate_host(platform.system(), platform.machine(), version, pin)
    brew = str(args.brew)
    run([brew, "--version"], "homebrew-version")
    validate_formula(json.loads(run([brew, "info", "--json=v2", pin["formula"]], "formula-before")), pin)
    # --bottle-tag already selects a bottle and is mutually exclusive with
    # --force-bottle in the real Homebrew CLI. Never drop the exact tag/hash.
    run([brew, "fetch", "--bottle-tag=" + pin["bottle_tag"], pin["formula"]],
        "fetch-bottle", timeout=600)
    cache = Path(run([brew, "--cache", "--bottle-tag=" + pin["bottle_tag"],
        pin["formula"]], "bottle-cache"))
    if not cache.is_file() or digest(cache) != pin["sha256"]:
        raise ValueError("actual cached bottle SHA256 differs; installation refused")
    # Homebrew performs relocation and dependency handling. This never requests
    # a source build. Check the installed receipt rather than assuming success.
    run([brew, "install", "--force-bottle", pin["formula"]], "install-bottle", timeout=900)
    installed_formula = validate_formula(json.loads(run([brew, "info", "--json=v2", pin["formula"]], "formula-after")), pin)
    installed_package = validate_installed_formula(installed_formula, pin)
    prefix = Path(run([brew, "--prefix", pin["formula"]], "installed-prefix")).resolve()
    rack = Path(run([brew, "--cellar", pin["formula"]], "installed-cellar")).resolve()
    if prefix.parent != rack:
        raise ValueError("resolved selected keg is outside the actual formula cellar")
    receipt_file = prefix / "INSTALL_RECEIPT.json"
    # Retain the actual receipt BEFORE validating so a schema/source mismatch
    # can be diagnosed from evidence instead of guessed after the runner exits.
    receipt_copy = out / "installed-receipt.json"
    receipt_copy.write_bytes(receipt_file.read_bytes())
    receipt_sha = digest(receipt_copy)
    receipt = json.loads(receipt_copy.read_text(encoding="utf-8"))
    write_json(out / "installation-observed.json", {"resolved_keg": str(prefix), "formula_cellar": str(rack),
        "installed_package": installed_package, "bottle_sha256": digest(cache),
        "receipt_sha256": receipt_sha, "assembler_executed": False,
        "macos_installation_qualified": False, "metal_acceleration_pass": False})
    validate_receipt(receipt, prefix, pin)
    assembler = prefix / "bin/llvm-as"
    version_text = run([assembler, "--version"], "llvm-version")
    if not re.search(r"LLVM version 21\.1\.8\b", version_text):
        raise ValueError("installed assembler version differs")
    for name in ("llvm-ar", "llvm-nm"):
        if not (prefix / "bin" / name).is_file():
            raise ValueError("matching archive tools are missing")
    return {"status": "PINNED_MACOS_LLVM_INSTALLED", "pin": pin,
        "host": {"arch": platform.machine(), "macos_version": version},
        "bottle_sha256": digest(cache), "bottle_bytes": cache.stat().st_size,
        "installed_receipt_sha256": receipt_sha, "llvm_as": str(assembler),
        "resolved_keg": str(prefix), "installed_package_version": installed_package["version"],
        "receipt_source": receipt["source"],
        "llvm_as_sha256": digest(assembler), "llvm_version": version_text,
        "macos_installation_qualified": True, "rust_compiled": False,
        "publication_ready": False, "metal_acceleration_pass": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--brew", type=Path, default=Path("/opt/homebrew/bin/brew"))
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    if args.out.exists():
        parser.error("--out must be new")
    args.out.mkdir(parents=True)
    try:
        result = acquire(args, args.out)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        result = {"status": "FAIL", "error": str(exc), "macos_installation_qualified": False,
            "publication_ready": False, "metal_acceleration_pass": False}
    write_json(args.out / "llvm-acquisition.json", result)
    print(json.dumps(result))
    return int(result["status"] == "FAIL")


if __name__ == "__main__":
    sys.exit(main())
