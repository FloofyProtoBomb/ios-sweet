#!/usr/bin/env python3
"""Build the bounded UID2000 Android Vulkan fixture using verified official NDK.

Run inside Linux/WSL with the Linux NDK on a case-sensitive filesystem. This
builds only the Android diagnostic; no iOS driver/runtime is produced or run.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
from qualify_shaders import generated_header

HERE=Path(__file__).resolve().parent
def sha(data):return hashlib.sha256(data).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--ndk',required=True,type=Path)
    ap.add_argument('--ndk-verification',required=True,type=Path)
    ap.add_argument('--qualified',required=True,type=Path)
    ap.add_argument('--out',required=True,type=Path)
    args=ap.parse_args()
    verification=json.loads(args.ndk_verification.read_text())
    if verification['published_sha1']!='5107f898313790e449e87eee2183d9a20602dee9' or verification['bytes']!=738633529:
        raise ValueError('unexpected official NDK package provenance')
    if verification['sha256']!='753611f410d002cfcd3f3dc2ef49aad532089d3180b436c060a90bf0fcb64df2':
        raise ValueError('NDK archive differs from the recorded verified package')
    if 'Pkg.Revision = 30.0.16248370' not in (args.ndk/'source.properties').read_text():raise ValueError('NDK revision mismatch')
    report=json.loads((args.qualified/'qualification.json').read_text());code={}
    for record in report['shaders']:
        stage=record['stage'];data=(args.qualified/(stage+'.spv')).read_bytes()
        if sha(data)!=record['qualified_sha256'] or not record['validator_pass'] or not record['instruction_words_identical']:
            raise ValueError('shader qualification mismatch')
        code[stage]=data
    if list(code)!=['compute','vertex','fragment'] or (args.qualified/'qualified_shaders.h').read_text()!=generated_header(report,code):
        raise ValueError('generated header differs from qualified shader assets')
    args.out.mkdir(parents=True,exist_ok=False)
    tools=args.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin';clang=tools/'aarch64-linux-android24-clang'
    binary=args.out/'dre-android-vulkan-probe'
    command=[str(clang),'-std=c11','-O2','-Wall','-Wextra','-Werror','-fPIE','-pie','-I'+str(args.qualified),
        str(HERE/'probe.c'),'-lvulkan','-lm','-o',str(binary)]
    compiled=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (args.out/'compile.log').write_text(compiled.stdout)
    if compiled.returncode:raise RuntimeError(compiled.stdout)
    elf=binary.read_bytes()
    if elf[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<HH',elf,16)!=(3,183):raise ValueError('not an ARM64 little-endian PIE')
    dynamic=subprocess.run([str(tools/'llvm-readelf'),'--dynamic',str(binary)],check=True,stdout=subprocess.PIPE,text=True).stdout
    dependencies=re.findall(r'Shared library: \[([^]]+)\]',dynamic)
    if not dependencies or any(d not in ['libvulkan.so','libm.so','libc.so','libdl.so'] for d in dependencies):
        raise ValueError('unexpected runtime dependency')
    result={'schema':1,'operation':'build-android-vulkan-project-shader-fixture','binary_sha256':sha(elf),'bytes':len(elf),
        'source_sha256':sha((HERE/'probe.c').read_bytes()),'qualified_shader_manifest_sha256':sha((args.qualified/'qualification.json').read_bytes()),
        'ndk_archive':verification,'clang_version':subprocess.run([str(tools/'clang'),'--version'],check=True,stdout=subprocess.PIPE,text=True).stdout,
        'elf_arch':'AArch64 little-endian PIE','android_api':24,'dynamic_dependencies':dependencies,
        'required_uid':2000,'gpu_fence_deadline_seconds':5,'process_deadline_seconds':30,
        'phone_execution_performed':False,'native_xnu_metal_proven':False}
    (args.out/'build.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ['binary_sha256','bytes','elf_arch','android_api','dynamic_dependencies']}))

if __name__=='__main__':main()
