#!/usr/bin/env python3
"""Check sparse clone bytes, checksum and source preservation on host files."""
import argparse
import hashlib
from pathlib import Path
import tempfile

from qemu_root import sparse_copy, allocated_ranges
from test_qemu_disk import sparse_file


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--large', action='store_true', help='also exercise an offset above 4 GiB')
    args = parser.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='root-copy-', dir=args.out) as temporary:
        directory = Path(temporary)
        for size in ([8 << 20, (1 << 32) + 4096] if args.large else [8 << 20]):
            source = directory / f'source-{size}.img'; sparse_file(source, size)
            segments = ((0, b'head' * 1024), (size // 2, b'middle' * 600), (size - 4096, b'end!' * 1024))
            with source.open('r+b') as stream:
                for offset, value in segments:
                    stream.seek(offset); stream.write(value)
            with source.open('rb') as stream:
                before = hashlib.file_digest(stream, 'sha256').hexdigest()
                ranges = allocated_ranges(stream, size)
            clone = directory / f'clone-{size}.img'
            assert sparse_copy(source, clone) == before
            with clone.open('rb') as stream:
                assert hashlib.file_digest(stream, 'sha256').hexdigest() == before
                for offset, value in segments:
                    stream.seek(offset); assert stream.read(len(value)) == value
            with source.open('rb') as stream:
                assert hashlib.file_digest(stream, 'sha256').hexdigest() == before
            assert clone.stat().st_size == source.stat().st_size == size
            try:
                sparse_copy(source, clone)
            except FileExistsError:
                pass
            else:
                raise AssertionError('overwrote an existing clone')
            print(f'PASS {size}-byte sparse clone, SHA/source preservation, known extents={ranges}', flush=True)
