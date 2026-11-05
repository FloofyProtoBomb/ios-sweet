#!/usr/bin/env python3
"""Artifact/ownership guards; optional real guestfish HFS install on scratch only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

import install_metal_probe as probe


def artifacts(directory):
    directory.mkdir()
    executable = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 1, 24, 0, 0)
    executable += struct.pack('<6I', 0x32, 24, 2, 16 << 16, 16 << 16, 0)
    (directory / probe.FILES[0]).write_bytes(executable)
    (directory / probe.FILES[1]).write_bytes(b'MTLB' + bytes(28))
    (directory / probe.FILES[2]).write_bytes((probe.HERE / 'gpu' / 'metal_probe.metal').read_bytes())
    checksums(directory)


def checksums(directory):
    lines = [hashlib.sha256((directory / name).read_bytes()).hexdigest() + '  ' + name for name in probe.FILES]
    (directory / 'SHA256SUMS').write_text('\n'.join(lines) + '\n', encoding='ascii')


class Guards(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.artifacts = self.root / 'artifacts'
        artifacts(self.artifacts)

    def tearDown(self):
        self.temporary.cleanup()

    def test_accept_exact_arm64_ios_artifacts(self):
        self.assertEqual(set(probe.validate_artifacts(self.artifacts)), set(probe.FILES))

    def test_reject_corrupted_download(self):
        (self.artifacts / probe.FILES[1]).write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            probe.validate_artifacts(self.artifacts)

    def test_reject_macos_or_simulator(self):
        for platform in (1, 7):
            with self.subTest(platform=platform):
                path = self.artifacts / probe.FILES[0]
                value = bytearray(path.read_bytes()); struct.pack_into('<I', value, 40, platform)
                path.write_bytes(value); checksums(self.artifacts)
                with self.assertRaisesRegex(ValueError, 'compatible'):
                    probe.validate_artifacts(self.artifacts)

    def test_reject_arm64e(self):
        path = self.artifacts / probe.FILES[0]
        value = bytearray(path.read_bytes()); struct.pack_into('<I', value, 8, 2)
        path.write_bytes(value); checksums(self.artifacts)
        with self.assertRaisesRegex(ValueError, 'plain-arm64'):
            probe.validate_artifacts(self.artifacts)

    def test_reject_bad_load_bounds(self):
        path = self.artifacts / probe.FILES[0]
        value = bytearray(path.read_bytes()); struct.pack_into('<I', value, 36, 32)
        path.write_bytes(value); checksums(self.artifacts)
        with self.assertRaisesRegex(ValueError, 'command length'):
            probe.validate_artifacts(self.artifacts)

    def test_reject_newer_ios_target(self):
        path = self.artifacts / probe.FILES[0]
        value = bytearray(path.read_bytes()); struct.pack_into('<I', value, 44, 17 << 16)
        path.write_bytes(value); checksums(self.artifacts)
        with self.assertRaisesRegex(ValueError, 'compatible'):
            probe.validate_artifacts(self.artifacts)

    def test_reject_non_metallib(self):
        (self.artifacts / probe.FILES[1]).write_bytes(bytes(32)); checksums(self.artifacts)
        with self.assertRaisesRegex(ValueError, 'Metal library header'):
            probe.validate_artifacts(self.artifacts)

    def test_reject_foreign_msl(self):
        (self.artifacts / probe.FILES[2]).write_bytes(b'foreign shader'); checksums(self.artifacts)
        with self.assertRaisesRegex(ValueError, 'project-owned'):
            probe.validate_artifacts(self.artifacts)

    def test_reject_manifest_traversal_and_duplicate(self):
        sums = self.artifacts / 'SHA256SUMS'
        original = sums.read_text(encoding='ascii')
        for value in (original.replace(probe.FILES[0], '../outside'), original + original.splitlines()[0] + '\n'):
            with self.subTest(value=value):
                sums.write_text(value, encoding='ascii')
                with self.assertRaises(ValueError):
                    probe.validate_artifacts(self.artifacts)

    @unittest.skipUnless(os.name == 'posix', 'Linux descriptor guard')
    def test_find_actual_open_inode_even_when_path_is_replaced(self):
        image = self.root / 'root.img'; image.write_bytes(b'fixture')
        proc = self.root / 'proc'; fd = proc / '1234' / 'fd'; fd.mkdir(parents=True)
        os.symlink(image, fd / '8')
        self.assertEqual(probe.active_image_owners(image, proc, 9999), [{'pid':1234, 'fd':'8'}])
        self.assertEqual(probe.active_image_owners(image, proc, 1234), [])

    @unittest.skipUnless(os.name == 'posix', 'Linux descriptor guard')
    def test_live_other_process_blocks_image_use(self):
        image = self.root / 'root.img'; image.write_bytes(b'fixture')
        process = subprocess.Popen([sys.executable, '-c',
                                    'import sys,time; f=open(sys.argv[1],"rb"); print("ready",flush=True); time.sleep(30)',
                                    str(image)], stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), 'ready')
            self.assertTrue(any(owner['pid'] == process.pid for owner in probe.active_image_owners(image)))
            with self.assertRaisesRegex(ValueError, 'in use'):
                probe.require_offline(image)
        finally:
            process.terminate(); process.wait(); process.stdout.close()


def integration(directory):
    """Real guestfs transaction on a fresh 64 MiB scratch volume; never phone data."""
    directory.mkdir(parents=True, exist_ok=False)
    source = directory / 'artifacts'; artifacts(source)
    image = directory / 'scratch-hfs.img'
    size = 64 << 20
    with image.open('wb') as stream:
        stream.truncate(size)
    subprocess.run(['mkfs.hfsplus', '-s', '-J', '-v', 'DRE-probe-fixture', str(image)], check=True)
    # Production install always uses the fixed 54,431,395,840-byte userdata;
    # only this scratch test substitutes a smaller known partition geometry.
    original_inspect = probe.inspect_hfs
    probe.USERDATA_BYTES = size
    probe.inspect_hfs = lambda path: original_inspect(path, expected_bytes=size)
    result = probe.install(image, source, directory / 'reports')
    if not result['completed'] or result['metal_support_observed'] or result['auto_start']:
        raise AssertionError('invalid scratch installation result')
    print(json.dumps({'scratch_integration':'PASS', 'reports':str(directory / 'reports')}, indent=2))


if __name__ == '__main__':
    if '--integration' in sys.argv:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument('--integration', type=Path, required=True)
        integration(parser.parse_args().integration)
    else:
        unittest.main()
