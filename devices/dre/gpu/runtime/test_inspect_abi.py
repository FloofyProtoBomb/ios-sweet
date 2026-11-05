#!/usr/bin/env python3
"""Guard tests use constructed instructions/metadata, never Apple bytes."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

import inspect_abi as abi


def bl(source, destination):
    displacement = destination - source
    assert displacement % 4 == 0 and -(1 << 27) <= displacement < (1 << 27)
    return 0x94000000 | ((displacement >> 2) & 0x3FFFFFF)


def line(address, instruction):
    return f"{address:#x}:  {instruction.to_bytes(4, 'little').hex(' ')}   opaque\n"


class DecoderTests(unittest.TestCase):
    def test_positive_and_negative_displacement(self):
        for source, destination in ((0x1000, 0x2000), (0x2000, 0x1000)):
            self.assertEqual(abi.branch_target(source, bl(source, destination)), destination)

    def test_reject_b_instead_of_bl(self):
        with self.assertRaisesRegex(ValueError, "not BL"):
            abi.branch_target(0x1000, 0x14000001)

    def test_hex_endianness_and_duplicate_addresses(self):
        decoded = abi.instructions(line(0x1000, bl(0x1000, 0x1400)))
        self.assertEqual(decoded[0x1000][0], 0x94000100)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            abi.instructions(line(0x1000, 0) + line(0x1000, 1))

    def test_mutated_factory_call_is_rejected(self):
        expected = abi.CALLS["enumeration"]
        original = "".join(line(a, bl(a, b)) for a, b in expected.items())
        abi.require_calls("enumeration", original)
        source = next(iter(expected))
        wrong = original.replace(line(source, bl(source, expected[source])),
                                 line(source, bl(source, expected[source] + 4)))
        with self.assertRaisesRegex(ValueError, "differs"):
            abi.require_calls("enumeration", wrong)

    def test_missing_call_is_not_a_pass(self):
        with self.assertRaisesRegex(ValueError, "differs"):
            abi.require_calls("enumeration", "")

    def test_exact_class_extent(self):
        decoded = "0x1000\n0x2000\n0x3000\n0x4000\n0x1d59e51a8\n"
        result = abi.verify_class_layout(decoded, struct.pack("<4I", 4, 8, 528, 0))
        self.assertEqual(result["instance_size"], 528)
        with self.assertRaisesRegex(ValueError, "extent"):
            abi.verify_class_layout(decoded, struct.pack("<4I", 4, 8, 536, 0))

    def test_encoded_pointer_is_not_masked_into_match(self):
        with self.assertRaisesRegex(ValueError, "pointer"):
            abi.verify_class_layout("0x1\n0x2\n0x3\n0x4\n0x201d59e51a8\n",
                                    struct.pack("<4I", 4, 8, 528, 0))

    def test_truncated_or_annotated_class_metadata(self):
        for decoded, raw in (("0x1\n", b""), ("pretend decoded", bytes(16))):
            with self.assertRaises(ValueError):
                abi.verify_class_layout(decoded, raw)

    def test_metadata_omission_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            abi.require_tokens("MetalPluginName", ("MetalPluginName", "MetalPluginClassName"), "keys")

    def test_foreign_profile_and_hash_changes_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "capture.json").write_text(json.dumps({"profile": {}, "files": {}}))
            with self.assertRaisesRegex(ValueError, "profile"):
                abi.verify_captures(directory)
            (directory / "metal.txt").write_text("changed")
            (directory / "capture.json").write_text(json.dumps({
                "profile": abi.PROFILE, "files": {"metal.txt": "0" * 64}}))
            with self.assertRaisesRegex(ValueError, "hash mismatch"):
                abi.verify_captures(directory)

    def test_missing_complete_inventory_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "capture.json").write_text(json.dumps({"profile": abi.PROFILE, "files": {}}))
            with self.assertRaisesRegex(ValueError, "missing or unexpected"):
                abi.verify_captures(directory)


if __name__ == "__main__":
    unittest.main()
