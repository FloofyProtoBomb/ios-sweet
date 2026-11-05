#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parent))
import translate_offline as t


class OutputAndPinChecks(unittest.TestCase):
    def test_spirv_header_rejects_bad_bounds_versions_and_truncation(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'shader.spv'
            good=struct.pack('<5I',0x07230203,0x10500,0,4,0)
            path.write_bytes(good)
            self.assertEqual(t.spirv_header(path)['id_bound'],4)
            for value in (good[:19],good+b'x',b'FAKE'+good[4:],
                    struct.pack('<5I',0x07230203,0x10700,0,4,0),
                    struct.pack('<5I',0x07230203,0x10500,0,0,0),
                    struct.pack('<5I',0x07230203,0x10500,0,0xffffffff,0),
                    struct.pack('<5I',0x07230203,0x10500,0,4,1)):
                path.write_bytes(value)
                with self.subTest(value=value.hex()), self.assertRaises(ValueError):t.spirv_header(path)

    def test_wrong_binary_pin_refuses_before_creating_output(self):
        with tempfile.TemporaryDirectory() as root:
            out=Path(root)/'out'
            args=argparse.Namespace(translator=Path(sys.executable),translator_sha256='0'*64,
                spirv_val=Path(sys.executable),out=out)
            with self.assertRaisesRegex(ValueError,'SHA256 pin differs'):t.translate(args)
            self.assertFalse(out.exists())

    def test_existing_directory_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as root:
            root=Path(root);source=root/'input.air';source.write_bytes(b'BC\xc0\xde'+bytes(4))
            profile=root/'profile.json';profile.write_text('{}')
            out=root/'out';out.mkdir();(out/'keep').write_text('unchanged')
            args=argparse.Namespace(translator=Path(sys.executable),
                translator_sha256=t.digest(sys.executable),spirv_val=Path(sys.executable),out=out,
                input=source,profile=profile,stage='compute',function='owned_test',fragment=None,color_format=None)
            with self.assertRaises(FileExistsError):t.translate(args)
            self.assertEqual((out/'keep').read_text(),'unchanged')


@unittest.skipUnless(sys.platform.startswith('linux'),'hard resource-limit cases require Linux')
class WorkerLimits(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='dre-compiler-worker-')
        self.addCleanup(self.temporary.cleanup)
        self.root=Path(self.temporary.name)

    def run_worker(self,code,seconds=2):
        t.run_bounded([sys.executable,'-c',code],self.root/'worker.log',
                      time.monotonic()+seconds,self.root)

    def test_successful_worker_log(self):
        self.run_worker('print("owned worker fixture")')
        self.assertEqual((self.root/'worker.log').read_text(),'owned worker fixture\n')

    def test_nonzero_worker_is_failure(self):
        with self.assertRaisesRegex(ValueError,'exited 17'):self.run_worker('raise SystemExit(17)')

    def test_wall_clock_limit_terminates_worker(self):
        with self.assertRaisesRegex(ValueError,'wall-clock budget'):
            self.run_worker('import time;time.sleep(5)',seconds=0.1)

    def test_address_space_limit_is_enforced(self):
        with self.assertRaisesRegex(ValueError,'external compiler/validator exited'):
            self.run_worker('value=bytearray(600*1024*1024)')


if __name__ == '__main__':unittest.main(verbosity=2)
