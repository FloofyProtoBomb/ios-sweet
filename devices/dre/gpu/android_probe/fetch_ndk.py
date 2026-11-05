#!/usr/bin/env python3
"""Fetch/check official NDKr30 for this Android-only fixture; extract on Linux.

The archive can live on /mnt/d; --extract must be a fresh case-sensitive Linux
directory. The published size/SHA1 are from developer.android.com/ndk/downloads;
SHA256 is additionally pinned to our locally recorded downloaded bytes.
No SDK/toolchain binaries are copied into the repository.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import urllib.request
import zipfile

URL='https://dl.google.com/android/repository/android-ndk-r30-linux.zip'
SIZE=738633529
SHA1='5107f898313790e449e87eee2183d9a20602dee9'
SHA256='753611f410d002cfcd3f3dc2ef49aad532089d3180b436c060a90bf0fcb64df2'
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--download',required=True,type=Path)
    ap.add_argument('--extract',required=True,type=Path)
    args=ap.parse_args()
    if sys.platform!='linux':raise ValueError('use Linux/WSL ext4; NTFS case folding cannot preserve all NDK headers')
    args.download.mkdir(parents=True,exist_ok=True);archive=args.download/'android-ndk-r30-linux.zip'
    if args.extract.exists():raise ValueError('extraction directory must be new')
    if not archive.exists():
        partial=archive.with_suffix('.zip.part')
        with urllib.request.urlopen(URL,timeout=60) as src,partial.open('xb') as dst:
            while True:
                chunk=src.read(8<<20)
                if not chunk:break
                dst.write(chunk)
        partial.rename(archive)
    h1=hashlib.sha1();h256=hashlib.sha256()
    with archive.open('rb') as file:
        for chunk in iter(lambda:file.read(8<<20),b''):h1.update(chunk);h256.update(chunk)
    if archive.stat().st_size!=SIZE or h1.hexdigest()!=SHA1 or h256.hexdigest()!=SHA256:
        raise ValueError('NDK archive size/digest mismatch; extraction refused')
    verification={'url':URL,'official_source':'https://developer.android.com/ndk/downloads','bytes':SIZE,
        'published_sha1':SHA1,'sha256':SHA256}
    (args.download/'ndk-verified.json').write_text(json.dumps(verification,indent=2)+'\n')
    root=args.extract.resolve();root.mkdir(parents=True,exist_ok=False)
    check=root/'.case-check';check.write_bytes(b'case')
    sensitive=not (root/'.CASE-CHECK').exists();check.unlink()
    if not sensitive:
        root.rmdir();raise ValueError('extract on case-sensitive Linux ext4, not NTFS/DrvFS')
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            if not (info.filename.startswith('android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/') or info.filename in [
                    'android-ndk-r30/source.properties','android-ndk-r30/NOTICE','android-ndk-r30/NOTICE.toolchain']):continue
            path=root/info.filename
            if root not in path.resolve().parents:raise ValueError('archive path escape')
            if info.is_dir():path.mkdir(parents=True,exist_ok=True);continue
            path.parent.mkdir(parents=True,exist_ok=True);mode=info.external_attr>>16
            if stat.S_ISLNK(mode):
                target=z.read(info).decode()
                if root not in (path.parent/target).resolve().parents:raise ValueError('archive symlink escape')
                path.symlink_to(target);continue
            with z.open(info) as src,path.open('xb') as dst:
                for chunk in iter(lambda:src.read(8<<20),b''):dst.write(chunk)
            os.chmod(path,mode&0o777 or 0o644)
    print(json.dumps({'ndk_verified':True,**verification,'extracted':str(root)}))

if __name__=='__main__':main()
