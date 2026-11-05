#!/usr/bin/env python3
"""Read saved user states from a paused QEMU RAM dump; never attaches or writes.

Requires configuration-matched constants from build_saved_stack_layout.py and
8-byte QMP snapshots of cpu_ttep/allproc (values, not symbol addresses), and the
capture report proving paused CPUs and the RAM hash. Raw RAM and reports are
private research artifacts and must never enter the repository. Saved user PCs
may precede capture; they are not live kernel stacks or evidence of deadlock.
"""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import struct

MASK64=(1<<64)-1
KMIN=0xffff800000000000
class ReadError(ValueError): pass

def u32(data,off=0): return struct.unpack_from('<I',data,off)[0]
def u64(data,off=0): return struct.unpack_from('<Q',data,off)[0]
def kernel_pointer(value): return KMIN<=value<=MASK64 and value%8==0

class Ram:
    def __init__(self,data,base,budget=64<<20):
        self.data=data; self.base=base; self.size=len(data);self.budget=budget;self.read_bytes=0
        if not 0<base<1<<48 or base%4096 or not self.size or base+self.size>1<<48:
            raise ReadError('invalid captured RAM bounds')
    def read(self,pa,size):
        if size<0 or size>65536 or pa<self.base or pa-self.base>self.size-size:
            raise ReadError(f'physical read outside captured RAM: {pa:#x}+{size}')
        if self.read_bytes+size>self.budget: raise ReadError('RAM read budget exceeded')
        self.read_bytes+=size
        return self.data[pa-self.base:pa-self.base+size]

class Paging:
    def __init__(self,ram,table_mask,page_mask):
        self.ram=ram;self.table_mask=table_mask;self.page_mask=page_mask;self.entries={}
        if table_mask!=0x0000fffffffff000 or page_mask!=0x0000fffffffff000:
            raise ReadError('unsupported physical address encoding')
    def word(self,address):
        if address not in self.entries:self.entries[address]=u64(self.ram.read(address,8))
        return self.entries[address]
    def translate(self,root,va,levels,page_shift):
        if root&((1<<page_shift)-1):raise ReadError('unaligned translation root')
        table=root
        for n,level in enumerate(levels):
            shift,index_mask,offmask=level
            entry=self.word(table+((va&index_mask)>>shift)*8)
            if not entry&1: raise ReadError(f'unmapped VA {va:#x}, level{n}')
            last=n==len(levels)-1
            if last:
                if entry&3!=3:raise ReadError('invalid leaf descriptor')
                return (entry&self.page_mask&~offmask)|(va&offmask)
            if not entry&2:
                if shift>=48:raise ReadError('unsupported root block')
                return (entry&self.page_mask&~offmask)|(va&offmask)
            table=entry&self.table_mask
            if table&((1<<page_shift)-1):raise ReadError('unaligned next-level table')
        raise ReadError('empty page geometry')
    def read(self,root,va,size,levels,page_shift):
        if not 0<size<=65536 or va<0 or va+size>1<<64:raise ReadError('invalid virtual read bounds')
        result=bytearray();page_size=1<<page_shift
        while size:
            part=min(size,page_size-(va&(page_size-1)))
            result.extend(self.ram.read(self.translate(root,va,levels,page_shift),part))
            va+=part;size-=part
        return bytes(result)

class Inspector:
    def __init__(self,ram,layout,cpu_ttep,native_attr=0):
        self.ram=ram;self.l=layout;self.cpu_ttep=cpu_ttep;self.native_attr=native_attr
        if layout.get('magic')!=0x445245535441434b or layout.get('version') not in (1,2):
            raise ReadError('unsupported layout record')
        if layout.get('with_pac'):raise ReadError('PAC saved states need a separate authenticated-state decoder')
        self.paging=Paging(ram,layout['table_address_mask'],layout['page_address_mask'])
        self.kernel_levels=[tuple(layout[f'kernel_l{i}_{key}'] for key in ('shift','index_mask','offmask')) for i in (1,2,3)]
        self.kernel_shift=layout['kernel_l3_shift']
        if self.kernel_shift not in (12,14):raise ReadError('unsupported kernel granule')
    def kr(self,address,size):
        if not KMIN<=address<=MASK64:raise ReadError(f'noncanonical kernel address {address:#x}')
        return self.paging.read(self.cpu_ttep,address,size,self.kernel_levels,self.kernel_shift)
    def k64(self,address):return u64(self.kr(address,8))
    def field(self,data,key,bits=64):
        off=self.l[key]
        if off>len(data)-bits//8:raise ReadError('layout field outside object: '+key)
        return u32(data,off) if bits==32 else u64(data,off)
    def geometry(self,pmap):
        l=self.l
        attr=self.native_attr if l['pmap_attr']==MASK64 else self.k64(pmap+l['pmap_attr'])
        levels_address=self.k64(attr+l['attr_levels'])
        root=u32(self.kr(attr+l['attr_root'],8))
        maximum=u32(self.kr(attr+l['attr_max'],8))
        shift=self.k64(attr+l['attr_page_shift'])
        if shift not in (12,14) or not 0<=root<=maximum<=3 or maximum!=3:
            raise ReadError('unsupported task page geometry')
        levels=[]
        for index in range(root,maximum+1):
            data=self.kr(levels_address+index*l['level_size'],l['level_size'])
            value=lambda key:u64(data,l[key])
            s=value('level_shift');mask=value('level_index_mask');offmask=value('level_offmask')
            if not shift<=s<48 or offmask!=(1<<s)-1 or not mask or mask&offmask:
                raise ReadError('malformed task paging level')
            expected_valid,expected_block=(3,2) if index==maximum else (1,0)
            if value('level_valid_mask')!=expected_valid or value('level_type_mask')!=2 or value('level_type_block')!=expected_block:
                raise ReadError('unsupported task descriptor encoding')
            levels.append((s,mask,offmask))
        if levels[-1][0]!=shift:raise ReadError('leaf granule mismatch')
        return self.k64(pmap+l['pmap_ttep']),levels,shift
    def process(self,address,data):
        l=self.l;task=self.k64(self.field(data,'proc_ro')+l['proc_ro_task'])
        if not 0<l['task_size']<=65536 or not 0<l['thread_size']<=65536:raise ReadError('implausible object sizes')
        td=self.kr(task,l['task_size']);count=self.field(td,'task_thread_count',32)
        if count>256:raise ReadError('task thread count exceeds bound')
        info={'pid':self.field(data,'proc_pid',32),'proc':hex(address),'task':hex(task),
            'mach_header':hex(self.field(td,'task_mach_header')),'declared_threads':count,'threads':[]}
        region=self.field(td,'task_shared_region')
        info['shared_cache_slide']=hex(u32(self.kr(region+l['shared_region_slide'],4))) if region else '0x0'
        vm_map=self.field(td,'task_map');pmap=self.k64(vm_map+l['map_pmap'])
        user_root,levels,page_shift=self.geometry(pmap)
        info['user_page_shift']=page_shift;info['user_table_root']=hex(user_root)
        head=task+l['task_threads'];link=self.k64(head);visited=set()
        while link!=head:
            if not kernel_pointer(link) or link in visited or len(visited)>=256:
                raise ReadError('invalid/cyclic task thread list')
            visited.add(link);thread=link;state=self.kr(thread,l['thread_size'])
            link=u64(state,l['thread_task_threads'])
            item={'id':self.field(state,'thread_id'),'thread':hex(thread),
                'state':hex(self.field(state,'thread_state',32)),
                'wait_event':hex(self.field(state,'thread_wait_event')),
                'continuation':hex(self.field(state,'thread_continuation'))}
            info['threads'].append(item)
            saved=self.field(state,'thread_upcb')
            if not saved:item['saved_state']='none';continue
            try:
                ss=self.kr(saved,l['saved_size'])
                if self.field(ss,'saved_flavor',32)!=l['saved_flavor64']:
                    item['saved_state']='not_arm64';continue
                for key in ('pc','lr','sp','fp'):item[key]=hex(self.field(ss,'saved_'+key))
                if l['version']>=2:
                    for key,bits in (('cpsr',32),('far',64),('esr',32),('exception',32)):
                        item[key]=hex(self.field(ss,'saved_'+key,bits))
                fp=self.field(ss,'saved_fp');sp=self.field(ss,'saved_sp');frames=[];item['frames']=frames
                for _ in range(16):
                    if not fp:break
                    if fp&7 or not 0x1000<=fp<1<<47 or fp<sp or fp-sp>32<<20:
                        item['frame_stop']='invalid_frame_pointer';break
                    frame=self.paging.read(user_root,fp,16,levels,page_shift)
                    parent,lr=struct.unpack('<QQ',frame);frames.append(hex(lr))
                    if parent<=fp:break
                    fp=parent
            except ReadError as error:item['saved_state_error']=str(error)
        info['walked_threads']=len(visited)
        if len(visited)!=count:info['warning']='thread-count mismatch; paused capture/layout may be inconsistent'
        return info
    def inspect(self,first,targets):
        result={'schema':1,'read_only_saved_user_states':True,'gpu_or_metal_proof':False,
            'saved_states_are_not_live_kernel_stacks':True,
            'processes':[],'visited_processes':0,'errors':[]}
        visited=set();address=first
        while address:
            if address in visited or len(visited)>=1024:
                result['errors'].append('cyclic/oversized process list');break
            visited.add(address)
            try:
                size=self.l['proc_size']
                if not 0<size<=65536:raise ReadError('implausible proc size')
                data=self.kr(address,size)
                off=self.l['proc_name'];n=self.l['proc_name_size']
                name=data[off:off+n].split(b'\0',1)[0].decode('ascii',errors='replace')
                if not name:
                    off=self.l['proc_comm'];n=self.l['proc_comm_size'];name=data[off:off+n].split(b'\0',1)[0].decode('ascii',errors='replace')
                if name in targets:
                    try:record=self.process(address,data);record['name']=name;result['processes'].append(record)
                    except ReadError as error:result['errors'].append(f'{name}: {error}')
                address=u64(data,self.l['proc_list'])
            except ReadError as error:result['errors'].append(str(error));break
        result['visited_processes']=len(visited);result['physical_bytes_read']=self.ram.read_bytes
        result['missing_targets']=sorted(set(targets)-{p['name'] for p in result['processes']})
        return result

def read_pointer(path):
    data=path.read_bytes()
    if len(data)!=8:raise ReadError('expected an eight-byte captured kernel global: '+str(path))
    return u64(data)

def capture_provenance(report,ram_path,ram_base,globals_paths,layout):
    """Validate the paused capture and matching size before dereferencing RAM.

    proc size is a useful configuration guard, not proof every layout field
    matches. The caller must still compile against the captured kernel's exact
    configured source/generated headers, as recorded in layout provenance.
    """
    raw=json.loads(report.read_text())
    capture=raw.get('final_memory_capture',{})
    if capture.get('status')!='COMPLETE' or capture.get('cpus_paused') is not True:
        raise ReadError('capture must be complete with all CPUs paused')
    if capture.get('physical_base')!=ram_base or capture.get('bytes')!=ram_path.stat().st_size:
        raise ReadError('capture RAM bounds do not match input')
    if Path(capture.get('file','')).resolve()!=ram_path.resolve():
        raise ReadError('RAM path does not match capture report')
    for name,path in globals_paths.items():
        record=capture.get('globals',{}).get(name,{})
        if record.get('bytes')!=8 or (report.parent/record.get('file','')).resolve()!=path.resolve():
            raise ReadError('global path does not match capture report: '+name)
        read_pointer(path)
    if read_pointer(globals_paths['_proc_struct_size'])!=layout['proc_size']:
        raise ReadError('proc size differs from captured kernel: rebuild layout constants')
    digest=capture.get('sha256','')
    if not isinstance(digest,str) or len(digest)!=64 or any(c not in '0123456789abcdef' for c in digest):
        raise ReadError('capture lacks a valid RAM SHA256')
    actual=hashlib.sha256()
    with ram_path.open('rb') as file:
        for chunk in iter(lambda:file.read(8<<20),b''):actual.update(chunk)
    if actual.hexdigest()!=digest:raise ReadError('RAM SHA256 does not match frozen capture')
    return {'all_cpus_paused':True,'physical_base':ram_base,'ram_bytes':capture['bytes'],
        'ram_sha256':digest,'kernel_sha256':raw.get('kernel_sha256'),
        'proc_struct_size':layout['proc_size']}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--layout',required=True,type=Path)
    ap.add_argument('--ram',required=True,type=Path)
    ap.add_argument('--ram-base',required=True,type=lambda s:int(s,0))
    ap.add_argument('--cpu-ttep',required=True,type=Path)
    ap.add_argument('--allproc',required=True,type=Path)
    ap.add_argument('--native-pt-attr',type=Path)
    ap.add_argument('--proc-struct-size',required=True,type=Path)
    ap.add_argument('--capture-report',required=True,type=Path)
    ap.add_argument('--target',action='append',default=[])
    ap.add_argument('--out',required=True,type=Path)
    args=ap.parse_args()
    layout=json.loads(args.layout.read_text())
    paths={'_cpu_ttep':args.cpu_ttep,'_allproc':args.allproc,'_proc_struct_size':args.proc_struct_size}
    if args.native_pt_attr:paths['_native_pt_attr']=args.native_pt_attr
    elif layout['layout']['pmap_attr']==MASK64:raise ReadError('native-pt-attr capture is required for fixed native paging')
    if args.out.exists():raise ReadError('report output must be a new path')
    provenance=capture_provenance(args.capture_report,args.ram,args.ram_base,paths,layout['layout'])
    with args.ram.open('rb') as file,mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
        inspector=Inspector(Ram(data,args.ram_base),layout['layout'],read_pointer(args.cpu_ttep),
            read_pointer(args.native_pt_attr) if args.native_pt_attr else 0)
        result=inspector.inspect(read_pointer(args.allproc),args.target or ['SpringBoard','backboardd','com.apple.datamigrator'])
    result['layout_provenance']=layout.get('provenance',{})
    result['capture_provenance']=provenance
    with args.out.open('x',encoding='utf-8') as file:json.dump(result,file,indent=2);file.write('\n')
    print(json.dumps({k:result[k] for k in ('visited_processes','physical_bytes_read','missing_targets','errors')}))
    for proc in result['processes']:print(proc['name'],'pid',proc['pid'],'saved threads',proc['walked_threads'])

if __name__=='__main__':main()
