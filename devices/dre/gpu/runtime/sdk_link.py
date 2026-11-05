#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Observe strict unresolved-symbol behavior of the actual selected SDK driver.

Modern Apple ld deprecates the explicit '-undefined error' spelling. Its strict
default is used only after a positive link and a real missing-symbol negative
control with the same compiler/base flags. Never adds dynamic_lookup, weak
imports, flat namespaces, fabricated symbol providers or warning waivers.
"""
from __future__ import annotations
import json
from pathlib import Path
import subprocess
import sys


def qualify_sdk_linker(compiler, flags: list, out: Path) -> dict:
    if sys.platform != "darwin":
        raise ValueError("strict SDK linker proof requires the actual macOS driver")
    work = out / "strict-link-controls"
    work.mkdir()
    caller, definition = work / "caller.c", work / "definition.c"
    caller.write_text("extern int dre_sdk_link_required_symbol(void);\n"
        "int dre_sdk_link_control(void) { return dre_sdk_link_required_symbol(); }\n")
    definition.write_text("int dre_sdk_link_required_symbol(void) { return 17; }\n")

    def run(argv, name, *, missing=False):
        command = [str(x) for x in argv]
        result = subprocess.run(command, capture_output=True, text=True,
            check=False, timeout=120)
        log = out / ("strict-link-" + name + ".log")
        text = result.stdout + result.stderr
        log.write_text(json.dumps(command) + "\n" + text, encoding="utf-8")
        if missing:
            if (result.returncode == 0 or "dre_sdk_link_required_symbol" not in text or
                not any(value in text.lower() for value in ("undefined symbols", "undefined symbol"))):
                raise ValueError("actual SDK did not reject the real unresolved-symbol control")
        elif result.returncode:
            print(text[-8000:], file=sys.stderr)
            raise ValueError("actual SDK linker control failed; see " + log.name)
        return result.returncode

    a, b = work / "caller.o", work / "definition.o"
    for source, target in ((caller, a), (definition, b)):
        run([compiler, *flags, "-x", "c", "-std=c11", "-c", source, "-o", target],
            "compile-" + source.stem)
    link = [compiler, *flags, "-dynamiclib", "-Wl,-fatal_warnings"]
    positive, negative = work / "positive.dylib", work / "negative.dylib"
    run([*link, a, b, "-o", positive], "positive")
    if not positive.is_file():
        raise ValueError("actual SDK positive link did not produce its library")
    missing_code = run([*link, a, "-o", negative], "negative", missing=True)
    # No undefined output is ever accepted as an input, even if a linker left
    # a partial file behind after correctly rejecting the negative control.
    proof = {"status": "ACTUAL_SDK_STRICT_DEFAULT_LINK_OBSERVED",
        "compiler": str(compiler), "base_flags": [str(x) for x in flags],
        "positive_control_linked": True, "missing_symbol_rejected": True,
        "negative_control_exit_code": missing_code, "deprecated_undefined_error_option_used": False,
        "fatal_warnings": True, "unresolved_import_waiver": False,
        "ios_program_executed": False, "metal_acceleration_pass": False}
    (out / "strict-link-proof.json").write_text(json.dumps(proof, indent=2) + "\n")
    return proof
