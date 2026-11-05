#!/usr/bin/env python3
"""Synthetic ADB side-effect/privacy guards; never contacts a phone."""
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import run_android

class Guards(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.base=Path(self.temp.name)
        self.serial='PRIVATE-SYNTHETIC-IDENTITY';self.calls=[];self.uid='2000';self.identity_ok=True;self.post_root=False;self.id_calls=0
        self.device={'status':'PASS','uid':2000,'vendor_id':0x5143,'device_id':0x06010900,'device_type':1,'device_name':'Adreno (TM) 619',
            'compute_exact_pass':True,'compute_values_checked':257,'input_guard_words_checked':255,'output_guard_words_checked':255,
            'offscreen_bgra_pass':True,'pixels_checked':4096,'gpu_timestamp_activity':True,'gpu_elapsed_ticks':[20,30],
            'native_xnu_metal_proven':False,'mesa_ir3_execution_proven':False}
        self.build=self.base/'build';self.build.mkdir();binary=self.build/'dre-android-vulkan-probe';binary.write_bytes(b'synthetic-test-only')
        self.hash=hashlib.sha256(binary.read_bytes()).hexdigest()
        (self.build/'build.json').write_text(json.dumps({'binary_sha256':self.hash,'bytes':binary.stat().st_size}))
        self.identity=self.base/'identity.private.json';self.identity.write_text(json.dumps({'serial':self.serial}))
    def adb(self,command,**kwargs):
        del kwargs
        self.calls.append(command);parts=command[3:];text='';rc=0
        if parts[:1]==['push']:pass
        elif parts[:1]==['shell']:
            p=parts[1:]
            if p==['id','-u']:
                self.id_calls+=1;text='0' if self.post_root and self.id_calls>=3 else self.uid
            elif p==['getprop','ro.serialno']:text=self.serial if self.identity_ok else 'DIFFERENT-PRIVATE'
            elif p==['getprop','sys.boot_completed']:text='1'
            elif p==['getprop','ro.lineage.version']:text='synthetic-Lineage'
            elif p[:1]==['sha256sum']:text=self.hash+'  probe'
            elif p[:1]==['cat']:text=json.dumps(self.device)
            elif p[:1] in [['mkdir'],['chmod'],['rm'],['rmdir']]:pass
            elif p and p[0].endswith('/probe'):text=json.dumps(self.device)
            else:self.fail('unexpected synthetic command')
        else:self.fail('unexpected synthetic ADB operation')
        return subprocess.CompletedProcess(command,rc,(text+'\n').encode(),b'')
    def invoke(self,execute=False):
        argv=['run_android.py','--adb','synthetic-adb','--identity',str(self.identity),'--build',str(self.build),'--out',str(self.base/'out')]
        if execute:argv.append('--run')
        self.output=io.StringIO()
        with patch.object(sys,'argv',argv),patch.object(subprocess,'run',side_effect=self.adb),redirect_stdout(self.output):run_android.main()
    def no_writes(self):
        self.assertTrue(all(c[3:] in [['shell','id','-u'],['shell','getprop','ro.serialno']] for c in self.calls))
    def test_default_only_read_metadata_and_local_plan(self):
        self.invoke();self.no_writes();self.assertNotIn(self.serial,self.output.getvalue())
        self.assertEqual(json.loads(self.output.getvalue())['status'],'READY_NOT_EXECUTED')
    def test_uid0_refused_without_privilege_or_device_writes(self):
        self.uid='0'
        with self.assertRaisesRegex(RuntimeError,'UID'):self.invoke(True)
        self.no_writes();self.assertNotIn(self.serial,self.output.getvalue())
    def test_identity_mismatch_refused_before_transfer(self):
        self.identity_ok=False
        with self.assertRaisesRegex(RuntimeError,'identity'):self.invoke(True)
        self.no_writes()
    def test_modified_binary_refused_before_adb(self):
        (self.build/'dre-android-vulkan-probe').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError,'hash'):self.invoke(True)
        self.assertFalse(self.calls)
    def test_synthetic_pass_has_postcheck_and_exact_nonrecursive_cleanup(self):
        self.invoke(True);self.assertNotIn(self.serial,self.output.getvalue())
        report=json.loads((self.base/'out/run.json').read_text());self.assertTrue(report['android_gpu_execution_pass'])
        self.assertEqual(report['postcheck']['uid'],2000);self.assertTrue(report['temporary_directory_removed'])
        cleanup=[c[4:] for c in self.calls if c[3:5]==['shell','rm']]
        self.assertEqual(len(cleanup),1);self.assertEqual(len(cleanup[0]),3)
        self.assertNotIn('-r',cleanup[0]);self.assertTrue(cleanup[0][1].endswith('/probe'));self.assertTrue(cleanup[0][2].endswith('/result.json'))
    def test_postrun_uid_change_stops_before_more_device_writes(self):
        self.post_root=True
        with self.assertRaisesRegex(RuntimeError,'post-run'):self.invoke(True)
        self.assertFalse(any(c[3:5] in [['shell','rm'],['shell','rmdir']] for c in self.calls))
        self.assertEqual(json.loads((self.base/'out/run.json').read_text())['status'],'FAILED_POSTCHECK')
    def test_unexpected_identifier_not_printed_or_published(self):
        self.device['device_name']=self.serial
        with self.assertRaisesRegex(RuntimeError,'identifier'):self.invoke(True)
        self.assertNotIn(self.serial,self.output.getvalue());self.assertFalse((self.base/'out/device-report.json').exists())
    def test_failed_workload_remains_failed_with_no_privilege_escalation(self):
        self.device['compute_exact_pass']=False
        with self.assertRaises(SystemExit) as error:self.invoke(True)
        self.assertEqual(error.exception.code,1)
        self.assertFalse(json.loads((self.base/'out/run.json').read_text())['android_gpu_execution_pass'])
        self.assertFalse(any('root' in c[3:] or 'reboot' in c[3:] for c in self.calls))

if __name__=='__main__':unittest.main()
