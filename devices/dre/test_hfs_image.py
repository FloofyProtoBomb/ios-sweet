#!/usr/bin/env python3
"""Exercise exact-size HFS image repair with real Linux hfsprogs.

Only creates synthetic regular files. No device nodes, mounts or phone access.
"""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('normalize_hfs',HERE.parents[1]/'tools/normalize-hfs-image-size.py')
normalizer=importlib.util.module_from_spec(spec);spec.loader.exec_module(normalizer)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    image=args.out/'repair.img'
    if image.exists():raise FileExistsError('refusing existing test image')
    expected=(128<<20)+4096
    with image.open('xb') as f:f.truncate(expected)
    subprocess.run(['mkfs.hfsplus','-s','-J','-v','DREBoundaryTest',str(image)],check=True,capture_output=True)
    with image.open('r+b') as f:
        f.seek(1024+48);free=struct.unpack('>I',f.read(4))[0]
        f.seek(1024+48);f.write(struct.pack('>I',free-1))
    repaired=False
    for attempt in range(3):
        result=subprocess.run(['fsck.hfsplus','-fy',str(image)],capture_output=True,text=True)
        (args.out/f'repair-{attempt}.log').write_text(result.stdout+result.stderr)
        try:normalizer.normalize(image,expected)
        except ValueError:continue
        check=subprocess.run(['fsck.hfsplus','-fn',str(image)],capture_output=True,text=True)
        (args.out/f'readonly-{attempt}.log').write_text(check.stdout+check.stderr)
        if check.returncode==0:repaired=True;break
    assert repaired and image.stat().st_size==expected
    print('PASS real hfsprogs repair and exact image boundary')
    # Reproduce a complete cache-line write past EOF without changing any
    # bytes of the declared filesystem, then verify normalization keeps them.
    with image.open('rb') as f:before=hashlib.file_digest(f,'sha256').hexdigest()
    padded=(expected+32767)//32768*32768
    with image.open('ab') as f:f.write(b'P'*(padded-expected))
    assert normalizer.normalize(image,expected)==padded-expected
    with image.open('rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==before
    print('PASS data preserved while removing outside-volume cache padding')
    assert normalizer.normalize(image,expected)==0
    print('PASS already-exact image is unchanged')
    # Clean alternate headers may have stale counters while geometry agrees.
    with image.open('r+b') as f:
        f.seek(expected-1024+48);free=struct.unpack('>I',f.read(4))[0]
        f.seek(expected-1024+48);f.write(struct.pack('>I',free-1))
    with image.open('rb') as f:stale=hashlib.file_digest(f,'sha256').hexdigest()
    with image.open('ab') as f:f.write(b'P'*(padded-expected))
    assert normalizer.normalize(image,expected)==padded-expected
    with image.open('rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==stale
    print('PASS valid stale alternate counters retain all volume bytes')
    # An arbitrary extension is not the observed cache-line condition.
    with image.open('ab') as f:f.write(b'X')
    with image.open('rb') as f:bad=hashlib.file_digest(f,'sha256').hexdigest()
    try:normalizer.normalize(image,expected)
    except ValueError:pass
    else:raise AssertionError('arbitrary extension accepted')
    with image.open('rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==bad
    print('PASS unexpected extension refused without changing data')


if __name__=='__main__':main()
