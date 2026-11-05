#!/usr/bin/env python3
"""Exercise actual patched SweetSurface methods against instrumented IOKit mocks.

Requires git and a C++17 compiler with pthreads and AddressSanitizer. Run in
Linux/macOS (including the task's WSL distro), with --out outside the repo.
No Apple framework, firmware or handset is used or copied into the tests.
"""
import argparse
from pathlib import Path
import re
import resource
import subprocess

HERE=Path(__file__).resolve().parent
REPO=HERE.parents[2]

def extract(text, anchor):
    start=text.index(anchor)
    # Preserve byte positions while ignoring braces in strings/comments.
    masked=re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda m:' '*len(m.group()),text,flags=re.S)
    opening=masked.index('{',start)
    depth=1; end=opening+1
    while depth:
        depth+=(masked[end]=='{')-(masked[end]=='}'); end+=1
    if text[end:end+1]==';': end+=1
    return text[start:end]+'\n'

def base_source(out):
    patches=REPO/'xnu/patches'
    patch=(patches/'0046-sweet-iosurface-iomfb.patch').read_text()
    marker='+++ b/iokit/Kernel/arm/SweetSurface.cpp\n'
    body=patch.split(marker,1)[1].split('\n--- ',1)[0]
    lines=body.splitlines(True)[1:]
    source=''.join(line[1:] for line in lines if line.startswith('+'))
    path=out/'iokit/Kernel/arm/SweetSurface.cpp'
    path.parent.mkdir(parents=True,exist_ok=True); path.write_text(source)
    subprocess.run(['git','apply',str(patches/'0068-sweet-surface-lookup-cached.patch')],cwd=out,check=True)
    source=path.read_text().replace('SWEET','DRE')
    path.write_text(source)
    return path

def selected(source):
    constants='\n'.join(re.findall(r'^#define DRE_SURFACE_.*$',source,re.M))
    classes=extract(source,'class SweetSurfaceRef :').replace('class SweetSurfaceRef :','struct SweetSurfaceRef :',1)
    classes+=extract(source,'class IOSurfaceRootUserClient :').replace('class IOSurfaceRootUserClient :','struct IOSurfaceRootUserClient :',1)
    methods=['static uint32_t *\nsweet_surface_seed(',
        'static bool\nsweet_surface_get(', 'static void\nsweet_surface_put(',
        'SweetSurface *\nsweet_surface_copy(', 'void\nSweetSurfaceRef::free(',
        'bool\nIOSurfaceRootUserClient::initWithTask(', 'void\nIOSurfaceRootUserClient::free(',
        'IOReturn\nIOSurfaceRootUserClient::clientClose(', 'SweetSurfaceRef *\nIOSurfaceRootUserClient::findRef(',
        'IOReturn\nIOSurfaceRootUserClient::addRef(', 'static void\nput32(', 'static void\nput64(',
        'static void\nsweet_surface_fill_result(', 'static IOReturn\nsweet_struct_out(',
        'IOReturn\nIOSurfaceRootUserClient::returnSurface(', 'IOReturn\nIOSurfaceRootUserClient::externalMethod(',
        'static IOReturn\nsweet_surface_seed_out(', 'static SweetSurface *\nsweet_surface_held(',
        'IOReturn\nIOSurfaceRootUserClient::trapLock(', 'IOReturn\nIOSurfaceRootUserClient::trapUnlock(',
        'IOReturn\nIOSurfaceRootUserClient::trapRelease(']
    fixed='sweet_surface_use_change(' in source
    extra=['IOReturn\nIOSurfaceRootUserClient::changeUseCount(',
        'IOReturn\nIOSurfaceRootUserClient::trapIncrementUse(',
        'IOReturn\nIOSurfaceRootUserClient::trapDecrementUse('] if fixed else ['IOReturn\nIOSurfaceRootUserClient::trapUseCount(']
    prefix=extract(source,'static IOReturn\nsweet_surface_use_change(') if fixed else ''
    body=prefix+''.join(extract(source,a) for a in methods+extra)
    # Trap table is not part of these tests. Traps are called through their real
    # methods, while the full kernel object is compiled separately.
    body+='\nIOReturn IOSurfaceRootUserClient::create(IOExternalMethodArguments *) { return kIOReturnUnsupported; }\n'
    body+='\nIOExternalTrap *IOSurfaceRootUserClient::getTargetAndTrapForIndex(IOService **, UInt32) { return nullptr; }\n'
    return '#define SURFACE_FIXED %d\n'%fixed+constants+'\n'+classes+'\n'+body

def run(cxx,out,source,baseline=False):
    include=out/'surface_selected.inc'; include.write_text(selected(source))
    binary=out/('surface-baseline' if baseline else 'surface-fixed')
    subprocess.run([cxx,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
        '-Wno-unused-parameter','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
        '-I',str(out),str(HERE/'test_iosurface_runtime.cpp'),'-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],text=True,capture_output=True)
    if baseline:
        if result.returncode==0 or 'result-local retain' not in result.stderr:
            raise RuntimeError('baseline failed for an unexpected reason: '+result.stdout+result.stderr)
        print('PASS baseline reproduces missing result-local retain',flush=True)
    else:
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        print(result.stdout.strip(),flush=True)

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',required=True,type=Path)
    ap.add_argument('--cxx',default='g++')
    args=ap.parse_args(); args.out=args.out.resolve(); args.out.mkdir(parents=True,exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    path=base_source(args.out)
    run(args.cxx,args.out,path.read_text(),baseline=True)
    subprocess.run(['git','apply',str(HERE/'0104-dre-iosurface-lifetime.patch')],cwd=args.out,check=True)
    run(args.cxx,args.out,path.read_text())
