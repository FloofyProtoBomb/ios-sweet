#!/usr/bin/env python3
"""Exercise the production AArch64 helper against only synthetic host files."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
MAGIC=0x44524553454d4931


def sparse_file(path,size):
    with path.open('wb') as stream:
        if sys.platform == 'win32':
            import msvcrt
            from ctypes import wintypes
            call=ctypes.windll.kernel32.DeviceIoControl
            call.argtypes=[wintypes.HANDLE,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,
                           ctypes.c_void_p,wintypes.DWORD,ctypes.POINTER(wintypes.DWORD),ctypes.c_void_p]
            returned=wintypes.DWORD()
            handle=msvcrt.get_osfhandle(stream.fileno())
            if not call(handle,0x900c4,None,0,None,0,ctypes.byref(returned),None):
                raise OSError('FSCTL_SET_SPARSE failed on scratch file')
            # CPython's _chsize_s path physically zero-fills extensions, even
            # for a sparse file. Native SetEndOfFile preserves sparse holes.
            seek=ctypes.windll.kernel32.SetFilePointerEx
            seek.argtypes=[wintypes.HANDLE,ctypes.c_longlong,ctypes.c_void_p,wintypes.DWORD]
            end=ctypes.windll.kernel32.SetEndOfFile
            end.argtypes=[wintypes.HANDLE]
            if not seek(handle,size,None,0) or not end(handle):
                raise OSError('native sparse-file sizing failed')
        else:
            stream.truncate(size)


def run_case(args,case,name):
    out=args.out/name; out.mkdir(parents=True,exist_ok=True)
    disk=out/'dre-root.img'
    if case==4: sparse_file(disk,0)
    elif case==5: sparse_file(disk,4097)
    elif case==9: sparse_file(disk,0x100000000 * 4096)
    elif case==6:
        sparse_file(disk,0x100002000)
        with disk.open('r+b') as stream:
            stream.write(b'\x11'*4096)
            stream.seek(0x100000000); stream.write(b'\x33'*4096)
    else: disk.write_bytes(b'\x11'*4096+b'\x22'*4096)
    link=out/'test.ld'
    link.write_text('ENTRY(_start)\nSECTIONS { . = 0x40400000; .text : { *(.text.entry) *(.text*) } '
                    '.rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n')
    elf=out/'test.elf'
    subprocess.run([args.zig,'cc','-target','aarch64-linux-musl','-O2','-ffreestanding','-fno-builtin',
        '-fno-stack-protector','-mgeneral-regs-only','-mstrict-align','-static','-nostdlib','-Wl,--build-id=none',
        '-Wl,-T,'+str(link),'-DTEST_CASE='+str(case),str(HERE/'test_qemu_disk_start.S'),
        str(HERE/'test_qemu_disk_baremetal.c'),str(HERE/'qemu_disk.c'),'-o',str(elf)],check=True)
    cmd=[args.qemu,'-M','virt,gic-version=3,virtualization=on','-cpu','cortex-a76','-m','128M',
         '-display','none','-serial','none','-monitor','none','-nodefaults','-no-reboot','-qmp','stdio',
         '-device','loader,file=test.elf,cpu-num=0',
         '-semihosting-config',f'enable={"off" if case in (2,3) else "on"},target=native,userspace=off']
    with (out/'stderr.txt').open('w') as err:
        process=subprocess.Popen(cmd,cwd=out,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True)
        def qmp(command,arguments=None):
            process.stdin.write(json.dumps({'execute':command,'arguments':arguments or {}})+'\n'); process.stdin.flush()
            while True:
                line=process.stdout.readline()
                if not line: raise RuntimeError((out/'stderr.txt').read_text())
                value=json.loads(line)
                if 'error' in value: raise RuntimeError(value['error'])
                if 'return' in value: return value['return']
        try:
            version=json.loads(process.stdout.readline())
            qmp('qmp_capabilities')
            deadline=time.monotonic()+10
            done=False
            while time.monotonic()<deadline:
                time.sleep(0.2)
                qmp('stop')
                qmp('pmemsave',{'val':0x401f0000,'size':48,'filename':'result.bin'})
                values=struct.unpack('<6Q',(out/'result.bin').read_bytes())
                magic,code=values[:2]
                if magic==MAGIC:
                    assert code==0,(name,code,[hex(v) for v in values[2:]])
                    done=True; break
                qmp('cont')
            assert done,'test did not finish (unexpected trap or ABI fault): '+name
            qmp('quit'); process.wait(timeout=10)
        finally:
            if process.poll() is None: process.kill()
            process.wait()
    if case==0:
        assert disk.read_bytes()==b'\x11'*4096+bytes((i^0x5a)&255 for i in range(4096))
    if case==1: assert disk.read_bytes()==b'\x11'*4096+b'\x22'*4096
    if case==6:
        with disk.open('rb') as stream:
            assert stream.read(4096)==b'\x11'*4096
            stream.seek(0x100000000); assert stream.read(4096)==b'\x33'*4096
            assert stream.read(4096)==bytes((i^0x5a)&255 for i in range(4096))
    return {'name':name,'status':'PASS','real_aarch64_semihosting':case not in (2,3),
            'semihosting_disabled_guard':case in (2,3),'phone_tested':False,'qemu':version}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zig',required=True); ap.add_argument('--qemu',required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--case',type=int,action='append')
    args=ap.parse_args();args.zig=str(Path(args.zig).resolve());args.qemu=str(Path(args.qemu).resolve())
    args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=True)
    names=('rw_bounds_flush','read_only','disabled_no_trap','phone_frequency_no_trap','empty_rejected',
           'unaligned_length_rejected','seek_above_4g','short_read_poison','flush_length_change_poison','oversize_rejected')
    reports=[]
    # Case9 needs a filesystem allowing a sparse file larger than16TiB;
    # common NTFS/ext4 configurations reject creation before QEMU can test it.
    for i in (args.case if args.case is not None else range(9)):
        reports.append(run_case(args,i,names[i]));print('PASS',names[i],flush=True)
    (args.out/'result.json').write_text(json.dumps({'status':'PASS','cases':reports,
        'host_hardware_durability_proven':False,'native_ufs_proven':False,
        'oversize_real_file_case_selected':bool(args.case and 9 in args.case)},indent=2)+'\n')


if __name__=='__main__':main()
