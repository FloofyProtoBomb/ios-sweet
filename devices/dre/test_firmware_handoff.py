#!/usr/bin/env python3
"""Check Python/ADT v1/v2 firmware handoff contracts without firmware or a phone."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import xnu


def node(raw):
    root, used = xnu.mkadt.adt.parse(raw)
    assert used == len(raw)
    return next((c for c in root.children if c.name == 'dre-gpu-firmware'), None)


class FirmwareHandoff(unittest.TestCase):
    def test_default_has_no_firmware_handoff(self):
        self.assertIsNone(node(xnu.tree()))

    def test_legacy_layout_remains_64_bytes(self):
        n = node(xnu.tree(ui=True, native_ufs=True, gpu_firmware=True))
        self.assertEqual(n.prop('compatible'), b'oneplus,dre-gpu-firmware-v1\0')
        self.assertEqual(n.prop('dre,firmware-handoff'), bytes(64))

    def test_explicit_relocated_layout_is_96_bytes(self):
        n = node(xnu.tree(ui=True, native_ufs=True, gpu_firmware=True,
                          gpu_firmware_relocated=True))
        self.assertEqual(n.prop('compatible'), b'oneplus,dre-gpu-firmware-v2\0')
        self.assertEqual(n.prop('dre,firmware-handoff'), bytes(96))

    def test_explicit_false_preserves_legacy_bytes(self):
        args = dict(ui=True, native_ufs=True, gpu_firmware=True)
        # Only the serializer contract is compared. Production boot entropy
        # remains random; fix it solely within this deterministic fixture.
        with patch.object(xnu.mkadt.os, 'urandom', side_effect=lambda n: bytes(n)):
            self.assertEqual(xnu.tree(**args), xnu.tree(**args, gpu_firmware_relocated=False))

    def test_relocation_does_not_implicitly_enable_firmware(self):
        with self.assertRaises(ValueError):
            xnu.tree(ui=True, native_ufs=True, gpu_firmware_relocated=True)

    def test_both_layouts_require_owned_ui_profile(self):
        for relocated in (False, True):
            for ui, storage in ((False,False), (True,False), (False,True)):
                with self.subTest(relocated=relocated, ui=ui, storage=storage):
                    with self.assertRaises(ValueError):
                        xnu.tree(ui=ui, native_ufs=storage, gpu_firmware=True,
                                 gpu_firmware_relocated=relocated)

    def test_cli_rejects_missing_provisioning_before_output(self):
        parent = Path(tempfile.mkdtemp(prefix='dre-fw-cli-'))
        target = parent/'must-not-be-created'
        try:
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('build.py')),
                '--gpu-firmware-relocated', '--out', str(target)],
                text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('requires --gpu-firmware-provisioning', result.stderr)
            self.assertFalse(target.exists())
        finally:
            parent.rmdir()  # No recursive deletion; failed CLI must create nothing.


if __name__ == '__main__':
    unittest.main()
