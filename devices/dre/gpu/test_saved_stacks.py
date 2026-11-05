#!/usr/bin/env python3
"""Synthetic ARM page tables and saved-state fixtures for the read-only walker."""
import struct
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from inspect_saved_stacks import Inspector,Ram,Paging,ReadError,MASK64,capture_provenance
BASE=0x100000000
KVA=0xfffffe0000000000
UVA=0x160000000

def fixture():
    data=bytearray(2<<20)
    def word(pa,value):struct.pack_into('<Q',data,pa-BASE,value)
    def kw(off,value):word(BASE+0x10000+off,value)
    def kd(off,value):struct.pack_into('<I',data,0x10000+off,value)
    l={'magic':0x445245535441434b,'version':2,'with_pac':0,'table_address_mask':0x0000fffffffff000,
       'page_address_mask':0x0000fffffffff000,'proc_size':128,'proc_list':0,'proc_ro':8,'proc_pid':16,
       'proc_name':24,'proc_name_size':32,'proc_comm':56,'proc_comm_size':17,'proc_ro_task':0,
       'task_size':128,'task_map':0,'task_threads':16,'task_thread_count':32,'task_mach_header':40,
       'task_shared_region':48,'shared_region_slide':0,'thread_size':128,'thread_task_threads':64,
       'thread_upcb':16,'thread_id':24,'thread_state':32,'thread_wait_event':40,'thread_continuation':48,
       'map_pmap':0,'pmap_ttep':0,'pmap_attr':8,'attr_levels':0,'attr_root':8,'attr_max':16,
       'attr_page_shift':24,'level_size':56,'level_shift':16,'level_index_mask':24,'level_offmask':8,
       'level_valid_mask':32,'level_type_mask':40,'level_type_block':48,
       'saved_size':64,'saved_flavor':0,'saved_flavor64':21,'saved_pc':8,'saved_lr':16,'saved_sp':24,'saved_fp':32,
       'saved_cpsr':40,'saved_far':48,'saved_esr':56,'saved_exception':60}
    for i,shift in enumerate((36,25,14),1):
        l[f'kernel_l{i}_shift']=shift;l[f'kernel_l{i}_index_mask']=2047<<shift;l[f'kernel_l{i}_offmask']=(1<<shift)-1
    # Three-level16KiB kernel tree, mapping the object fixture into high KVA.
    word(BASE+((KVA>>36)&2047)*8,(BASE+0x4000)|3)
    word(BASE+0x4000+((KVA>>25)&2047)*8,(BASE+0x8000)|3)
    for n in range(32):word(BASE+0x8000+n*8,(BASE+0x10000+n*0x4000)|3)
    # Separate4KiB user tree with two deliberately noncontiguous frame pages.
    word(BASE+0xa0000+((UVA>>30)&511)*8,(BASE+0xa1000)|3)
    word(BASE+0xa1000+((UVA>>21)&511)*8,(BASE+0xa2000)|3)
    word(BASE+0xa2000,(BASE+0xb0000)|3);word(BASE+0xa2008,(BASE+0xc0000)|3)
    kw(0x1000,0);kw(0x1008,KVA+0x2000);kd(0x1010,26)
    data[0x10000+0x1018:0x10000+0x1018+12]=b'SpringBoard\0'
    kw(0x2000,KVA+0x3000)
    kw(0x3000,KVA+0x5000);kw(0x3010,KVA+0x4000);kd(0x3020,1)
    kw(0x3028,0x100000000);kw(0x3030,KVA+0xa000)
    kw(0x4040,KVA+0x3010);kw(0x4010,KVA+0x9000);kw(0x4018,101);kd(0x4020,1)
    kw(0x4028,0x1234);kw(0x4030,KVA+0xcccc)
    kw(0x5000,KVA+0x6000);kw(0x6000,BASE+0xa0000);kw(0x6008,KVA+0x7000)
    kw(0x7000,KVA+0x8000);kd(0x7008,1);kd(0x7010,3);kw(0x7018,12)
    for level,shift in enumerate((39,30,21,12)):
        off=0x8000+56*level
        for n,value in enumerate((1<<shift,(1<<shift)-1,shift,511<<shift,3 if level==3 else 1,2,2 if level==3 else 0)):kw(off+n*8,value)
    kd(0x9000,21);kw(0x9008,0x180012344);kw(0x9010,0x180011110)
    kw(0x9018,UVA+0xf00);kw(0x9020,UVA+0xff8);kw(0xa000,0x123000)
    kd(0x9028,0x60000000);kw(0x9030,0x16fed4000);kd(0x9038,0x56000080);kd(0x903c,0)
    # A frame crosses the4KiB boundary into a physically nonadjacent page.
    word(BASE+0xb0ff8,0);word(BASE+0xc0000,0x180123456)
    return data,l,word,kw,kd

class SavedStateTests(unittest.TestCase):
    def run_fixture(self,edit=None,budget=64<<20):
        data,l,w,kw,kd=fixture()
        if edit:edit(data,l,w,kw,kd)
        obj=Inspector(Ram(data,BASE,budget),l,BASE)
        return obj.inspect(KVA+0x1000,['SpringBoard'])
    def test_actual_process_and_cross_page_frame(self):
        r=self.run_fixture();self.assertFalse(r['errors']);self.assertFalse(r['missing_targets'])
        p=r['processes'][0];self.assertEqual(p['pid'],26);self.assertEqual(p['walked_threads'],1)
        self.assertEqual(p['shared_cache_slide'],'0x123000');self.assertEqual(p['user_page_shift'],12)
        t=p['threads'][0];self.assertEqual(t['id'],101);self.assertEqual(t['pc'],'0x180012344')
        self.assertEqual(t['frames'],['0x180123456']);self.assertEqual(t['state'],'0x1')
        self.assertEqual(t['esr'],'0x56000080');self.assertEqual(t['far'],'0x16fed4000')
        self.assertEqual(t['cpsr'],'0x60000000');self.assertEqual(t['exception'],'0x0')
    def test_fixed_native_paging_attribute(self):
        data,l,w,kw,kd=fixture();l['pmap_attr']=MASK64;kw(0x6008,0xdeadbeef)
        r=Inspector(Ram(data,BASE),l,BASE,KVA+0x7000).inspect(KVA+0x1000,['SpringBoard'])
        self.assertFalse(r['errors']);self.assertEqual(r['processes'][0]['threads'][0]['frames'],['0x180123456'])
    def test_u32_shared_cache_slide_at_unaligned_field(self):
        def edit(d,l,w,kw,kd):
            l['shared_region_slide']=4;kd(0xa004,0x345000);kd(0xa008,0xffffffff)
        r=self.run_fixture(edit);self.assertFalse(r['errors'])
        self.assertEqual(r['processes'][0]['shared_cache_slide'],'0x345000')
    def test_old_layout_remains_supported_without_exception_fields(self):
        def edit(d,l,*_):
            l['version']=1
            for key in ('cpsr','far','esr','exception'):del l['saved_'+key]
        r=self.run_fixture(edit);self.assertFalse(r['errors']);self.assertNotIn('esr',r['processes'][0]['threads'][0])
    def test_cyclic_process_list_is_bounded(self):
        r=self.run_fixture(lambda d,l,w,kw,kd:kw(0x1000,KVA+0x1000));self.assertIn('cyclic',r['errors'][0])
    def test_cyclic_thread_list_is_bounded(self):
        r=self.run_fixture(lambda d,l,w,kw,kd:kw(0x4040,KVA+0x4000));self.assertIn('cyclic',r['errors'][0])
    def test_missing_target(self):
        data,l,*_=fixture();r=Inspector(Ram(data,BASE),l,BASE).inspect(KVA+0x1000,['backboardd'])
        self.assertEqual(r['missing_targets'],['backboardd']);self.assertFalse(r['processes'])
    def test_read_budget(self):self.assertIn('budget',self.run_fixture(budget=8)['errors'][0])
    def test_unmapped_user_frame_is_reported(self):
        r=self.run_fixture(lambda d,l,w,kw,kd:w(BASE+0xa2000,0))
        self.assertIn('unmapped',r['processes'][0]['threads'][0]['saved_state_error'])
    def test_nonresident_saved_state_is_reported(self):
        r=self.run_fixture(lambda d,l,w,kw,kd:kw(0x4010,0))
        self.assertEqual(r['processes'][0]['threads'][0]['saved_state'],'none')
    def test_wrong_flavor(self):
        r=self.run_fixture(lambda d,l,w,kw,kd:kd(0x9000,1))
        self.assertEqual(r['processes'][0]['threads'][0]['saved_state'],'not_arm64')
    def test_invalid_fp_bounds(self):
        r=self.run_fixture(lambda d,l,w,kw,kd:kw(0x9020,UVA-8))
        self.assertEqual(r['processes'][0]['threads'][0]['frame_stop'],'invalid_frame_pointer')
    def test_geometry_refused(self):self.assertIn('geometry',self.run_fixture(lambda d,l,w,kw,kd:kw(0x7018,16))['errors'][0])
    def test_pac_refused(self):
        data,l,*_=fixture();l['with_pac']=1
        with self.assertRaisesRegex(ReadError,'PAC'):Inspector(Ram(data,BASE),l,BASE)
    def test_bad_magic_refused(self):
        data,l,*_=fixture();l['magic']=0
        with self.assertRaisesRegex(ReadError,'layout'):Inspector(Ram(data,BASE),l,BASE)
    def test_physical_bounds_and_alignment(self):
        for address,size in [(BASE-1,1),(BASE+(2<<20),1),(BASE,65537),(BASE,-1)]:
            with self.subTest(address=address,size=size):
                with self.assertRaises(ReadError):Ram(bytearray(2<<20),BASE).read(address,size)
    def test_kernel_table_alignment_refused(self):
        data,l,*_=fixture();i=Inspector(Ram(data,BASE),l,BASE+4096)
        with self.assertRaisesRegex(ReadError,'unaligned'):i.kr(KVA,8)
    def test_kernel_block_descriptor(self):
        data,l,w,kw,kd=fixture()
        # Independent 4KiB2MiB block translation, no third table needed.
        root=BASE+0x180000;second=BASE+0x181000
        w(root,second|3);w(second,BASE|1)
        p=Paging(Ram(data,BASE),l['table_address_mask'],l['page_address_mask'])
        levels=[(30,511<<30,(1<<30)-1),(21,511<<21,(1<<21)-1),(12,511<<12,4095)]
        self.assertEqual(p.translate(root,0x12345,levels,12),BASE+0x12345)
    def test_out_of_capture_translation_fails_closed(self):
        data,l,w,*_=fixture();w(BASE+((KVA>>36)&2047)*8,(BASE+0x400000)|3)
        with self.assertRaisesRegex(ReadError,'outside'):Inspector(Ram(data,BASE),l,BASE).kr(KVA,8)

class CaptureValidationTests(unittest.TestCase):
    def run_capture(self,change=None,layout_size=128):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);ram=p/'ram.bin';ram.write_bytes(bytes(4096))
            paths={name:p/(name+'.bin') for name in ('_cpu_ttep','_allproc','_proc_struct_size')}
            for name,file in paths.items():file.write_bytes(struct.pack('<Q',128 if name=='_proc_struct_size' else BASE))
            c={'status':'COMPLETE','cpus_paused':True,'physical_base':BASE,'bytes':4096,
               'file':str(ram),'sha256':hashlib.sha256(ram.read_bytes()).hexdigest(),
               'globals':{name:{'bytes':8,'file':file.name} for name,file in paths.items()}}
            if change:change(c,paths,ram)
            report=p/'result.json';report.write_text(json.dumps({'final_memory_capture':c,'kernel_sha256':'a'*64}))
            return capture_provenance(report,ram,BASE,paths,{'proc_size':layout_size})
    def test_complete_matching_capture(self):
        r=self.run_capture();self.assertTrue(r['all_cpus_paused']);self.assertEqual(r['proc_struct_size'],128)
    def test_unpaused_capture_rejected(self):
        with self.assertRaisesRegex(ReadError,'paused'):self.run_capture(lambda c,*_:c.update(cpus_paused=False))
    def test_incomplete_capture_rejected(self):
        with self.assertRaisesRegex(ReadError,'complete'):self.run_capture(lambda c,*_:c.update(status='PARTIAL'))
    def test_hash_mismatch_rejected(self):
        with self.assertRaisesRegex(ReadError,'SHA256'):self.run_capture(lambda c,*_:c.update(sha256='0'*64))
    def test_wrong_global_path_rejected(self):
        def change(c,*_):c['globals']['_allproc']['file']='different.bin'
        with self.assertRaisesRegex(ReadError,'global path'):self.run_capture(change)
    def test_wrong_proc_size_rejected(self):
        with self.assertRaisesRegex(ReadError,'proc size'):self.run_capture(layout_size=1920)
    def test_mismatched_ram_bounds_rejected(self):
        with self.assertRaisesRegex(ReadError,'bounds'):self.run_capture(lambda c,*_:c.update(bytes=8192))

if __name__=='__main__':unittest.main()
