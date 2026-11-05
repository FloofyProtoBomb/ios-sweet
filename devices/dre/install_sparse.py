#!/usr/bin/env python3
"""Encode a complete raw install image as Android sparse RAW/FILL chunks.

Zero blocks are FILL(0), never DONT_CARE: a first install must not depend on
the old contents of userdata. This tool only reads/writes regular host files.
It does not access a phone, edit a partition table, or run fastboot.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import zlib

MAGIC, RAW, FILL = 0xED26FF3A, 0xCAC1, 0xCAC2


def extents(source, size, block):
    """Use sparse-file holes as known zeros; fall back to ordinary reads."""
    if not hasattr(os, 'SEEK_DATA'):
        return [(0, size, False)]
    result, pos = [], 0
    try:
        while pos < size:
            try:
                data = os.lseek(source.fileno(), pos, os.SEEK_DATA)
            except OSError as e:
                if e.errno != errno.ENXIO:
                    raise
                data = size
            data = min(data, size)
            if data % block:
                return [(0, size, False)]
            if data > pos:
                result.append((pos, data, True))
            if data == size:
                break
            hole = min(os.lseek(source.fileno(), data, os.SEEK_HOLE), size)
            if hole <= data or hole % block:
                return [(0, size, False)]
            result.append((data, hole, False))
            pos = hole
    except OSError:
        return [(0, size, False)]
    return result


def encode(source_path, output_path, block=4096, max_raw_bytes=32 << 20):
    source_path, output_path = Path(source_path), Path(output_path)
    original = source_path.stat()
    if not stat.S_ISREG(original.st_mode):
        raise ValueError('source must be a regular host file')
    if block < 512 or block > 65536 or block & (block - 1):
        raise ValueError('block size must be a power of two from 512 to 65536')
    size = original.st_size
    if not size or size % block or size // block > 0xFFFFFFFF:
        raise ValueError('source size must be a nonzero block multiple within sparse v1 limits')
    max_raw_bytes -= max_raw_bytes % block
    if not block <= max_raw_bytes <= 0xFFFFFFFF - 12:
        raise ValueError('invalid maximum RAW chunk size')
    part = output_path.with_name(output_path.name + '.part')
    if output_path.exists() or part.exists():
        raise FileExistsError('refusing to overwrite sparse output or partial file')
    output_path.parent.mkdir(parents=True, exist_ok=True)
    raw_hash, checksum = hashlib.sha256(), 0
    zeros = bytes(4 << 20)
    zero_block = bytes(block)
    chunks = raw_blocks = fill_blocks = 0
    pending_raw = bytearray()
    pending_fill = 0

    with source_path.open('rb', buffering=0) as source, part.open('xb') as out:
        out.write(bytes(28))

        def flush_raw():
            nonlocal chunks, raw_blocks
            if pending_raw:
                count = len(pending_raw) // block
                out.write(struct.pack('<HHII', RAW, 0, count, 12 + len(pending_raw)))
                out.write(pending_raw)
                chunks += 1
                raw_blocks += count
                pending_raw.clear()

        def flush_fill():
            nonlocal chunks, pending_fill, fill_blocks
            if pending_fill:
                out.write(struct.pack('<HHIII', FILL, 0, pending_fill, 16, 0))
                chunks += 1
                fill_blocks += pending_fill
                pending_fill = 0

        def consume(data, known_zero=False):
            nonlocal checksum, pending_fill
            raw_hash.update(data)
            checksum = zlib.crc32(data, checksum)
            if known_zero or data == zeros[:len(data)]:
                flush_raw()
                pending_fill += len(data) // block
                return
            for offset in range(0, len(data), block):
                value = data[offset:offset + block]
                if value == zero_block:
                    flush_raw()
                    pending_fill += 1
                else:
                    flush_fill()
                    pending_raw.extend(value)
                    if len(pending_raw) == max_raw_bytes:
                        flush_raw()

        last_report = 0
        for start, end, hole in extents(source, size, block):
            source.seek(start)
            pos = start
            while pos < end:
                count = min(len(zeros), end - pos)
                data = zeros[:count] if hole else source.read(count)
                if len(data) != count:
                    raise ValueError('source became short during encoding')
                consume(data, hole)
                pos += count
                if pos - last_report >= 1 << 30:
                    print(f'encoded {pos >> 20}/{size >> 20} MiB', flush=True)
                    last_report = pos
        flush_raw()
        flush_fill()
        final = source_path.stat()
        if (final.st_size, final.st_mtime_ns) != (original.st_size, original.st_mtime_ns):
            raise ValueError('source changed during encoding')
        assert raw_blocks + fill_blocks == size // block
        out.seek(0)
        out.write(struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, block, size // block, chunks, checksum))
        out.flush()
        os.fsync(out.fileno())
    part.replace(output_path)
    with output_path.open('rb') as f:
        sparse_hash = hashlib.file_digest(f, 'sha256').hexdigest()
    return {'schema': 1, 'format': 'Android sparse v1', 'block_size': block,
            'expanded_bytes': size, 'expanded_sha256': raw_hash.hexdigest(),
            'expanded_crc32': f'{checksum:08x}', 'chunks': chunks,
            'raw_blocks': raw_blocks, 'fill_zero_blocks': fill_blocks,
            'dont_care_blocks': 0, 'sparse_bytes': output_path.stat().st_size,
            'sparse_sha256': sparse_hash, 'max_raw_chunk_bytes': max_raw_bytes,
            'phone_accessed': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    if args.manifest.resolve() in {args.source.resolve(), args.output.resolve(),
                                  args.output.with_name(args.output.name + '.part').resolve()}:
        parser.error('manifest path must be separate from image files')
    result = encode(args.source, args.output)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result), flush=True)
