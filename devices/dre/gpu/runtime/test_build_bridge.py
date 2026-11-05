#!/usr/bin/env python3
"""Guard regressions for binary import matching and host/bottle selection."""
import copy
import json
from pathlib import Path
import unittest

from acquire_llvm_macos import validate_formula, validate_host, validate_receipt
from verify_bridge_exports import compare, digest

HERE = Path(__file__).resolve().parent


class BridgeGuards(unittest.TestCase):
    def test_actual_lock_pin(self):
        pin = json.loads((HERE / "bridge_lock.json").read_text())
        self.assertEqual(digest(HERE / pin["file"]), pin["sha256"])

    def test_binary_function_names(self):
        contract = {"imported_bridge_symbol_names": ["nmb_a", "nmb_b"]}
        result = compare(contract, "a.o:\nnmb_a T 0 4\n_nmb_b T 4 8\nnmb_extra T c 1\n")
        self.assertEqual(result["required_import_count"], 2)
        self.assertFalse(result["binary_abi_link_checked"])
        self.assertFalse(result["metal_acceleration_pass"])

    def test_undefined_data_duplicate_missing_refused(self):
        contract = {"imported_bridge_symbol_names": ["nmb_a"]}
        for symbols in ("nmb_a U", "nmb_a D 0 8", "nmb_other T 0 4", "nmb_a T 0 4\n_nmb_a T 0 4"):
            with self.subTest(symbols=symbols), self.assertRaises(ValueError):
                compare(contract, symbols)

    def test_malformed_empty_contract_refused(self):
        for names in ([], ["not_bridge"], ["nmb_a", "nmb_a"], [None]):
            with self.subTest(names=names), self.assertRaises(ValueError):
                compare({"imported_bridge_symbol_names": names}, "nmb_a T 0 4")

    def test_bottle_platform_guards(self):
        pin = json.loads((HERE / "llvm21-macos.json").read_text())
        validate_host("Darwin", "arm64", "15.7.2", pin)
        for host in (("Linux", "arm64", "15"), ("Darwin", "x86_64", "15"),
                     ("Darwin", "arm64", "14.7"), ("Darwin", "arm64", "26.1")):
            with self.subTest(host=host), self.assertRaises(ValueError):
                validate_host(*host, pin)

    def test_metadata_drift_refused(self):
        pin = json.loads((HERE / "llvm21-macos.json").read_text())
        f = {"name": pin["formula"], "versions": {"stable": pin["version"]}, "revision": 1,
            "bottle": {"stable": {"rebuild": 0, "files": {pin["bottle_tag"]:
                {"sha256": pin["sha256"], "url": pin["url"]}}}}}
        validate_formula({"formulae": [f]}, pin)
        changed = copy.deepcopy(f)
        changed["bottle"]["stable"]["files"][pin["bottle_tag"]]["sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            validate_formula({"formulae": [changed]}, pin)

    def test_bottle_receipt_source_build_refused(self):
        pin = json.loads((HERE / "llvm21-macos.json").read_text())
        receipt = {"poured_from_bottle": True, "arch": "arm64", "source":
            {"versions": {"stable": "21.1.8"}, "revision": 1}}
        validate_receipt(receipt, Path("/tool/21.1.8_1"), pin)
        receipt["poured_from_bottle"] = False
        with self.assertRaises(ValueError):
            validate_receipt(receipt, Path("/tool/21.1.8_1"), pin)


if __name__ == "__main__":
    unittest.main()
