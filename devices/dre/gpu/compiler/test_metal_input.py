#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('metal_input', Path(__file__).with_name('metal_input.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def wrapped(payload=b'BC\xc0\xde\x00\x00\x00\x00'):
    return struct.pack('<5I',0x0b17c0de,0,20,len(payload),0xffffffff) + payload


def container():
    # Authored envelope fixture, not valid LLVM or an Apple shader. A semantic
    # decoder must reject its fake function data. This suite tests bounds only.
    fn = struct.pack('<I',1) + bytes(12)
    body = fn + bytes(8) + bytes(8) + wrapped()
    header = struct.pack('<4sHHHBBHHQ8Q',b'MTLB',1,2,7,0,0x82,16,0,88+len(body),
                         88,len(fn),104,8,112,8,120,len(wrapped()))
    return header + body


class InputBounds(unittest.TestCase):
    def test_owned_envelope_fixtures_do_not_claim_llvm_or_gpu_validation(self):
        for data in (wrapped(),b'BC\xc0\xde'+bytes(8),container()):
            with self.subTest(data=data[:4]):
                facts = m.preflight(data)
                self.assertFalse(facts['llvm_semantics_verified'])
                self.assertFalse(facts['gpu_execution_observed'])

    def test_bad_wrapper_ranges_magic_version_cpu_and_tail(self):
        for offset,value in ((4,1),(8,0),(8,0xffffffff),(12,0xffffffff),(16,0)):
            data=bytearray(wrapped());struct.pack_into('<I',data,offset,value)
            with self.subTest(offset=offset,value=value), self.assertRaises(ValueError):m.preflight(data)
        for data in (wrapped()[:19],wrapped()+b'not padding',wrapped()+bytes(16),b'BCno',b'MTLB'):
            with self.subTest(data=data[:8]), self.assertRaises(ValueError):m.preflight(data)

    def test_metallib_wrong_target_and_lengths(self):
        for offset,fmt,value in ((4,'H',0x8001),(6,'H',3),(8,'H',8),(10,'B',2),
                (11,'B',0x81),(12,'H',17),(14,'H',8),(16,'Q',1),(24,'Q',0xffffffffffffffff),
                (32,'Q',0xffffffffffffffff),(40,'Q',88),(72,'Q',0),(80,'Q',0)):
            data=bytearray(container());struct.pack_into('<'+fmt,data,offset,value)
            with self.subTest(offset=offset,value=value), self.assertRaises(ValueError):m.preflight(data)

    def test_function_count_budget_and_missing_bitcode(self):
        for count in (0,4097,0xffffffff):
            data=bytearray(container());struct.pack_into('<I',data,88,count)
            with self.subTest(count=count), self.assertRaises(ValueError):m.preflight(data)
        data=bytearray(container());data[120:124]=b'FAKE'
        with self.assertRaises(ValueError):m.preflight(data)

    def test_bounded_file_refuses_empty_and_large_input(self):
        with tempfile.TemporaryDirectory() as root:
            path=Path(root)/'input';path.write_bytes(b'')
            with self.assertRaises(ValueError):m.bounded_file(path)
            path.write_bytes(wrapped())
            with self.assertRaises(ValueError):m.bounded_file(path,limit=4)
            self.assertEqual(m.inspect_file(path)[1]['kind'],'wrapped-air')


if __name__ == '__main__':unittest.main(verbosity=2)
