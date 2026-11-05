#!/usr/bin/env python3
"""Emit exact XNU layout constants from an existing matching CI build tree.

Read-only inputs: kernel build tree and recorded clang command log. Outputs go
only under a new --out directory; the compile-only C source is not a driver.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess

HERE=Path(__file__).resolve().parent
LABELS=('magic version with_pac proc_size proc_list proc_ro proc_pid proc_comm proc_comm_size '
    'proc_name proc_name_size proc_ro_task task_size task_map task_threads task_thread_count '
    'task_mach_header task_shared_region shared_region_slide thread_size thread_task_threads '
    'thread_upcb thread_id thread_state thread_wait_event thread_continuation map_pmap pmap_ttep '
    'pmap_attr attr_levels attr_root attr_max attr_page_shift level_size level_shift level_index_mask '
    'level_offmask level_valid_mask level_type_mask level_type_block saved_size saved_flavor '
    'saved_flavor64 saved_pc saved_lr saved_sp saved_fp saved_cpsr saved_far saved_esr saved_exception '
    'kernel_l1_shift kernel_l1_index_mask '
    'kernel_l1_offmask kernel_l2_shift kernel_l2_index_mask kernel_l2_offmask kernel_l3_shift '
    'kernel_l3_index_mask kernel_l3_offmask table_address_mask page_address_mask').split()

def array_from_macho(path,labels):
    data=path.read_bytes()
    if struct.unpack_from('<I',data)[0]!=0xfeedfacf: raise ValueError('not a little-endian64 Mach-O object')
    count=struct.unpack_from('<I',data,16)[0]; cursor=32; sections=[]; symtab=None
    for _ in range(count):
        cmd,size=struct.unpack_from('<II',data,cursor)
        if size<8 or cursor+size>len(data): raise ValueError('invalid object load command')
        if cmd==0x19:
            number=struct.unpack_from('<I',data,cursor+64)[0]
            for n in range(number):
                o=cursor+72+n*80
                addr,length,offset=struct.unpack_from('<QQI',data,o+32)
                sections.append((addr,length,offset))
        elif cmd==2: symtab=struct.unpack_from('<4I',data,cursor+8)
        cursor+=size
    if not symtab: raise ValueError('object lacks symbols')
    offset,count,strings,string_size=symtab
    for n in range(count):
        name,kind,section,desc,value=struct.unpack_from('<IBBHQ',data,offset+n*16)
        end=data.index(0,strings+name,strings+string_size)
        if data[strings+name:end]!=b'_dre_saved_stack_layout': continue
        address,length,fileoff=sections[section-1]
        wanted=len(labels)*8
        if value<address or value-address+wanted>length: raise ValueError('layout record outside section')
        return struct.unpack_from('<'+'Q'*len(labels),data,fileoff+value-address)
    raise ValueError('layout record symbol absent')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--buildtree',required=True,type=Path)
    ap.add_argument('--build-log',required=True,type=Path)
    ap.add_argument('--zig',type=Path)
    ap.add_argument('--out',required=True,type=Path)
    args=ap.parse_args(); args.out=args.out.resolve()
    if args.buildtree.resolve()==args.out or args.buildtree.resolve() in args.out.parents:
        raise ValueError('output must be outside the read-only build tree')
    args.out.mkdir(parents=True,exist_ok=False)
    bt=args.buildtree.resolve().as_posix()
    repo=HERE.parents[2]
    spec=importlib.util.spec_from_file_location('xcc',repo/'tools/xnu-cc.py')
    xcc=importlib.util.module_from_spec(spec);spec.loader.exec_module(xcc)
    pattern=xcc.CI_SDK_PLATFORM
    class LiteralPattern:
        def sub(self,replacement,value): return pattern.sub(lambda _:replacement,value)
    xcc.CI_SDK_PLATFORM=LiteralPattern()
    if args.zig: xcc.CLANG=args.zig.resolve().as_posix()
    original=subprocess.run
    commands=xcc.commands(args.build_log)
    dirs={}
    for file in args.buildtree.resolve().joinpath('build/xnu.obj').rglob('meta_features.h'):
        for comp in xcc.COMPONENTS:
            if '/'+comp+'/' in file.as_posix(): dirs[comp]=file.parent.as_posix()
    os.environ['XNU_CC_OBJDIR']=str(args.out)
    values={}; object_hashes={}
    for comp,relative,labels in [('bsd','bsd/dev/dre/saved_stack_layout.c',LABELS[3:12]),
                                ('osfmk','osfmk/kern/dre_saved_stack_layout.c',LABELS[:3]+LABELS[12:])]:
        source=args.out/(comp+'-saved_stack_layout.c')
        source.write_bytes((HERE/'saved_stack_layout.c').read_bytes())
        fake=xcc.CI_DXB+'/xnu/'+relative
        def run(command,**kwargs):
            command=[str(source) if arg==bt+'/xnu/'+relative else arg for arg in command]
            if args.zig and command[0].startswith(xcc.CLANG):
                command=[xcc.CLANG,'c++' if command[0].endswith('++') else 'cc',*command[1:]]
                command=['aarch64-macos-none' if a=='arm64-apple-macos14.0' else a for a in command]
                command=['-mcpu=generic' if a.startswith('-march=') else a for a in command]
            command += ['-I'+bt+'/xnu/osfmk','-I'+bt+'/xnu/osfmk/libsa',
                        '-DDRE_STACK_LAYOUT_BSD='+('1' if comp=='bsd' else '0')]
            return original(command,**kwargs)
        xcc.subprocess.run=run
        command=xcc.template_for(fake,commands)
        if command is None: raise ValueError('no matching '+comp+' C command in build log')
        _,rc,log=xcc.run((fake,command),bt,dirs)
        (args.out/(comp+'-compile.log')).write_text(log)
        if rc: raise RuntimeError('layout compile failed:\n'+log[-5000:])
        obj=args.out/(relative.replace('/','__')+'.o')
        values.update(zip(labels,array_from_macho(obj,labels)))
        object_hashes[comp]=hashlib.sha256(obj.read_bytes()).hexdigest()
    if values['magic']!=0x445245535441434b or values['version']!=2: raise ValueError('bad layout record')
    result={'schema':1,'layout':values,'provenance':{
        'build_log_sha256':hashlib.sha256(args.build_log.read_bytes()).hexdigest(),
        'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
        'object_sha256':object_hashes,
        'kernel_layout_is_configuration_specific':True}}
    (args.out/'layout.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(values,indent=2))

if __name__=='__main__': main()
