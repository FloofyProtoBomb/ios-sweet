#!/usr/bin/env python3
"""Check complete-image sparse transport with Android's independent simg2img."""
import argparse
import hashlib
import random
from pathlib import Path
import struct
import subprocess
from install_sparse import encode, RAW, FILL


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--simg2img', default='simg2img')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(619)
    blocks = [bytes(4096), b'Z' * 4096, rng.randbytes(4096)]
    cases = [('zero', bytes(4096 * 17)), ('dense', rng.randbytes(4096 * 40)),
             ('mixed', b''.join(rng.choice(blocks) for _ in range(257))),
             ('edge', b'X' * 4096 + bytes(4096 * 1025) + b'Y' * 4096)]
    for name, content in cases:
        raw, sparse, decoded = (args.out / (name + suffix) for suffix in ('.img', '.simg', '.decoded'))
        raw.write_bytes(content)
        result = encode(raw, sparse, max_raw_bytes=4096 * 7)
        subprocess.run([args.simg2img, str(sparse), str(decoded)], check=True)
        assert decoded.read_bytes() == content
        assert result['expanded_sha256'] == hashlib.sha256(content).hexdigest()
        assert result['dont_care_blocks'] == 0
        with sparse.open('rb') as f:
            header = struct.unpack('<IHHHHIIII', f.read(28))
            for _ in range(header[7]):
                kind, reserved, count, size = struct.unpack('<HHII', f.read(12))
                assert reserved == 0 and kind in (RAW, FILL)
                assert count > 0
                if kind == RAW:
                    assert size == 12 + count * 4096 and count <= 7
                else:
                    assert size == 16
                f.seek(size - 12, 1)
        print('PASS Android decoder', name)
    # Extent-aware path: the file has real holes, not merely allocated zero bytes.
    raw, sparse, decoded = (args.out / ('holes' + suffix) for suffix in ('.img', '.simg', '.decoded'))
    with raw.open('wb') as f:
        f.write(b'A' * 4096); f.seek(8 << 20); f.write(b'B' * 4096)
    result = encode(raw, sparse)
    subprocess.run([args.simg2img, str(sparse), str(decoded)], check=True)
    assert decoded.read_bytes() == raw.read_bytes()
    assert result['fill_zero_blocks'] >= 2047
    print('PASS Android decoder actual sparse input')
    for name, content in [('unaligned', b'a'), ('empty', b'')]:
        raw = args.out / (name + '.img'); raw.write_bytes(content)
        try:
            encode(raw, args.out / (name + '.simg'))
        except ValueError:
            pass
        else:
            raise AssertionError('invalid source accepted')
        print('PASS refused', name)
    try:
        encode(args.out/'zero.img', args.out/'zero.simg')
    except FileExistsError:
        pass
    else:
        raise AssertionError('existing output overwritten')
    print('PASS refused existing output')


if __name__ == '__main__':
    main()
