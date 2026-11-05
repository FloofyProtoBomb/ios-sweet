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
ap.add_argument('--zap',type=Path)
ap.add_argument('--zap-mdt',type=Path)
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
zap=args.out/('test-a619-zap.exe' if sys.platform=='win32' else 'test-a619-zap')
subprocess.run([args.zig,'cc','-O2','-Wall','-Wextra','-Werror',str(HERE/'a619_zap.c'),
    str(HERE/'test_a619_zap.c'),'-o',str(zap)],check=True)
zap_extra=[]
if args.zap or args.zap_mdt:
    if not args.zap or not args.zap_mdt or not args.firmware_manifest:
        ap.error('--zap and --zap-mdt require each other and --firmware-manifest')
    entries=json.loads(args.firmware_manifest.read_text())
    for name,path in [('a615_zap.elf',args.zap),('a615_zap.mdt',args.zap_mdt)]:
        entry=next(e for e in entries if e['name']==name)
        assert path.stat().st_size==entry['bytes']
        assert hashlib.file_digest(path.open('rb'),'sha256').hexdigest()==entry['sha256']
    zap_extra=[str(args.zap),str(args.zap_mdt)]
subprocess.run([str(zap.resolve()),*zap_extra],check=True)
pas=args.out/('test-a619-pas13.exe' if sys.platform=='win32' else 'test-a619-pas13')
subprocess.run([args.zig,'cc','-O2','-Wall','-Wextra','-Werror',str(HERE/'a619_zap.c'),
    str(HERE/'a619_pas13.c'),str(HERE/'test_a619_pas13.c'),'-o',str(pas)],check=True)
subprocess.run([str(pas.resolve())],check=True)
smmu=args.out/('test-a619-smmu.exe' if sys.platform=='win32' else 'test-a619-smmu')
subprocess.run([args.zig,'cc','-O2','-Wall','-Wextra','-Werror',str(HERE/'a619_smmu.c'),
    str(HERE/'test_a619_smmu.c'),'-o',str(smmu)],check=True)
subprocess.run([str(smmu.resolve())],check=True)
cp=args.out/('test-a619-cp.exe' if sys.platform=='win32' else 'test-a619-cp')
subprocess.run([args.zig,'cc','-O2','-Wall','-Wextra','-Werror',str(HERE/'a619_backend.c'),
    str(HERE/'a619_cp.c'),str(HERE/'test_a619_cp.c'),'-o',str(cp)],check=True)
subprocess.run([str(cp.resolve()),*extra],check=True)
rbbm=args.out/('test-a619-rbbm.exe' if sys.platform=='win32' else 'test-a619-rbbm')
subprocess.run([args.zig,'cc','-O2','-Wall','-Wextra','-Werror',str(HERE/'a619_rbbm.c'),
    str(HERE/'test_a619_rbbm.c'),'-o',str(rbbm)],check=True)
subprocess.run([str(rbbm.resolve())],check=True)
vm=args.out/('test-a619-vm.exe' if sys.platform=='win32' else 'test-a619-vm')
subprocess.run([args.zig,'cc','-O2','-Wall','-Wextra','-Werror',str(HERE/'a619_smmu.c'),
    str(HERE/'a619_vm.c'),str(HERE/'test_a619_vm.c'),'-o',str(vm)],check=True)
subprocess.run([str(vm.resolve())],check=True)
for target,suffix in [('aarch64-linux-musl','o'),('aarch64-macos-none','macho.o')]:
    subprocess.run([args.zig,'cc','-target',target,'-ffreestanding','-fno-builtin',
        '-mgeneral-regs-only','-mstrict-align','-O2','-Wall','-Wextra','-Werror','-c',
        str(HERE/'a619_smmu.c'),'-o',str(args.out/('a619_smmu.'+suffix))],check=True)
    for source in ['a619_pas13','a619_smc64','a619_cp','a619_rbbm','a619_vm']:
        subprocess.run([args.zig,'cc','-target',target,'-ffreestanding','-fno-builtin',
            '-mgeneral-regs-only','-mstrict-align','-O2','-Wall','-Wextra','-Werror','-c',
            str(HERE/(source+'.c')),'-o',str(args.out/(source+'.'+suffix))],check=True)
    subprocess.run([args.zig,'cc','-target',target,'-ffreestanding','-fno-builtin',
        '-mgeneral-regs-only','-mstrict-align','-O2','-Wall','-Wextra','-Werror','-c',
        str(HERE/'a619_zap.c'),'-o',str(args.out/('a619_zap.'+suffix))],check=True)
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
