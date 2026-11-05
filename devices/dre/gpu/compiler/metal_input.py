#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded envelope checks before the real AIR decoder sees a shader.

This checks containers, not LLVM instructions or Metal semantics. The pinned
translator must still decode/verify AIR and spirv-val must check its output.
No Apple input, shader body, device identity or firmware belongs in git.
"""
import hashlib
import os
from pathlib import Path
import stat
import struct

MAX_INPUT_BYTES = 64 * 1024 * 1024
MAX_FUNCTIONS = 4096
LLVM_MAGIC = b'BC\xc0\xde'
WRAPPER_MAGIC = b'\xde\xc0\x17\x0b'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def bounded_file(path, limit=MAX_INPUT_BYTES):
    path = Path(path)
    with path.open('rb') as stream:
        info = stream.fileno()
        before = os.fstat(info)
        require(stat.S_ISREG(before.st_mode), 'input must be a regular file')
        require(0 < before.st_size <= limit, 'input exceeds the fixed byte budget or is empty')
        data = stream.read(limit + 1)
        after = os.fstat(info)
        require(len(data) == before.st_size and
                (before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns),
                'input changed during the bounded read')
    return data


def air_envelope(data):
    require(4 <= len(data) <= MAX_INPUT_BYTES, 'invalid AIR input size')
    if data.startswith(LLVM_MAGIC):
        return {'kind':'llvm-bitcode', 'bitcode_offset':0, 'bitcode_bytes':len(data)}
    require(data.startswith(WRAPPER_MAGIC) and len(data) >= 20, 'expected LLVM bitcode or its wrapper')
    _, version, offset, size, cpu = struct.unpack_from('<5I', data)
    require(version == 0 and cpu == 0xffffffff, 'unsupported AIR wrapper version/CPU field')
    require(offset >= 20 and offset <= len(data) and 4 <= size <= len(data) - offset,
            'AIR wrapper bitcode range exceeds its input')
    require(data[offset:offset+4] == LLVM_MAGIC, 'wrapper does not contain LLVM bitcode')
    tail = data[offset+size:]
    require(len(tail) <= 15 and not any(tail), 'unexpected data after wrapped bitcode')
    return {'kind':'wrapped-air', 'bitcode_offset':offset, 'bitcode_bytes':size}


def preflight(data):
    require(4 <= len(data) <= MAX_INPUT_BYTES, 'invalid shader input size')
    if not data.startswith(b'MTLB'):
        facts = air_envelope(data)
    else:
        require(len(data) >= 88, 'truncated metallib header')
        (magic, platform, major, minor, library_type, os_type, os_major, os_minor,
         declared, *ranges) = struct.unpack_from('<4sHHHBBHHQ8Q', data)
        require(magic == b'MTLB' and major == 2 and minor <= 7, 'unsupported metallib container version')
        require(platform == 1 and library_type == 0 and os_type == 0x82,
                'only executable iOS metallib inputs are enabled')
        require(os_major == 16 and os_minor <= 7, 'metallib deployment target is outside iOS 16.0–16.7')
        require(declared == len(data), 'metallib declared length differs from its input')
        sections, occupied = {}, []
        for index, name in enumerate(('functions','public_metadata','private_metadata','bitcode')):
            offset, size = ranges[index*2:index*2+2]
            require(offset <= len(data) and size <= len(data) - offset, 'metallib section exceeds input')
            if size:
                require(offset >= 88, 'metallib section overlaps the fixed header')
                require(all(offset + size <= lo or offset >= hi for lo,hi in occupied),
                        'metallib sections overlap')
                occupied.append((offset,offset+size))
            sections[name] = {'offset':offset,'bytes':size}
        fn = sections['functions']
        require(fn['bytes'] >= 4 and fn['offset'] <= len(data)-4, 'missing metallib function list')
        count, = struct.unpack_from('<I',data,fn['offset'])
        require(0 < count <= MAX_FUNCTIONS, 'metallib function count exceeds fixed budget')
        bc = sections['bitcode']
        require(bc['bytes'] >= 4 and data[bc['offset']:bc['offset']+4] in (LLVM_MAGIC, WRAPPER_MAGIC),
                'metallib bitcode section lacks a supported bitcode envelope')
        # Function tags, per-function hashes/offsets and full LLVM records are
        # deliberately left to the real Rust AIR decoder, not guessed here.
        facts = {'kind':'ios-metallib','container_version':[major,minor],
                 'deployment_target':[os_major,os_minor], 'function_count':count,'sections':sections}
    return dict(facts, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                llvm_semantics_verified=False, gpu_execution_observed=False)


def inspect_file(path):
    data = bounded_file(path)
    return data, preflight(data)
