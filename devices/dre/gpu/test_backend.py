#!/usr/bin/env python3
"""Build/run the production queue tests and cross-compile the ARM64 backend.

The optional private firmware input is checked against its private manifest,
then parsed by the production C SQE-view function. No firmware is copied.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--zig',default='zig')
ap.add_argument('--out',required=True,type=Path)
ap.add_argument('--firmware',type=Path)
ap.add_argument('--firmware-manifest',type=Path)
args=ap.parse_args()
args.out.mkdir(parents=True,exist_ok=True)
binary=args.out/('test-a619-backend.exe' if sys.platform=='win32' else 'test-a619-backend')
subprocess.run([args.zig,'cc','-O2','-Wall','-Wextra','-Werror',str(HERE/'a619_backend.c'),
    str(HERE/'test_a619_backend.c'),'-o',str(binary)],check=True)
extra=[]
if args.firmware:
    if not args.firmware_manifest: ap.error('--firmware requires --firmware-manifest')
    entries=json.loads(args.firmware_manifest.read_text())
    entry=next(e for e in entries if e['name']=='a630_sqe.fw')
    assert args.firmware.stat().st_size==entry['bytes']
    assert hashlib.file_digest(args.firmware.open('rb'),'sha256').hexdigest()==entry['sha256']
    extra=[str(args.firmware)]
subprocess.run([str(binary.resolve()),*extra],check=True)
power=args.out/('test-a619-power.exe' if sys.platform=='win32' else 'test-a619-power')
subprocess.run([args.zig,'cc','-O2','-Wall','-Wextra','-Werror',str(HERE/'a619_holi_power.c'),
    str(HERE/'test_a619_holi_power.c'),'-o',str(power)],check=True)
subprocess.run([str(power.resolve())],check=True)
for target,suffix in [('aarch64-linux-musl','o'),('aarch64-macos-none','macho.o')]:
    subprocess.run([args.zig,'cc','-target',target,'-ffreestanding','-fno-builtin',
        '-mgeneral-regs-only','-mstrict-align','-O2','-Wall','-Wextra','-Werror','-c',
        str(HERE/'a619_holi_power.c'),'-o',str(args.out/('a619_holi_power.'+suffix))],check=True)
subprocess.run([args.zig,'cc','-target','aarch64-linux-musl','-ffreestanding','-fno-builtin',
    '-mgeneral-regs-only','-mstrict-align','-O2','-Wall','-Wextra','-Werror','-c',
    str(HERE/'a619_backend.c'),'-o',str(args.out/'a619_backend.o')],check=True)
subprocess.run([args.zig,'cc','-target','aarch64-macos-none','-ffreestanding','-fno-builtin',
    '-mgeneral-regs-only','-mstrict-align','-O2','-Wall','-Wextra','-Werror','-c',
    str(HERE/'a619_backend.c'),'-o',str(args.out/'a619_backend.macho.o')],check=True)
print('PASS freestanding ARM64 ELF/Mach-O backend compile; Metal/native GPU execution not established')
