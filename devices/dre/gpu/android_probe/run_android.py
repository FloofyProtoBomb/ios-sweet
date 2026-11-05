#!/usr/bin/env python3
"""Check identity/UID; optionally run the temporary Android Vulkan fixture.

Default is read-only device metadata plus a local execution plan. --run pushes
only the verified helper under a fresh /data/local/tmp directory. No root,
reboot, fastboot, package installation or partition operation is implemented.
Private serial selectors and raw ADB errors are never printed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def public_summary(result):
    return {k:result.get(k) for k in ['status','identity_matched','uid','binary_sha256','android_gpu_execution_pass',
        'compute_values_checked','pixels_checked','gpu_timestamp_activity','native_xnu_metal_proven']}
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--adb',required=True,type=Path)
    ap.add_argument('--identity',required=True,type=Path)
    ap.add_argument('--build',required=True,type=Path)
    ap.add_argument('--out',required=True,type=Path)
    ap.add_argument('--run',action='store_true')
    args=ap.parse_args()
    identity=json.loads(args.identity.read_text());serial=identity.get('serial')
    if not isinstance(serial,str) or not serial or any(c.isspace() for c in serial):raise ValueError('invalid private identity input')
    args.out.mkdir(parents=True,exist_ok=False)
    def adb(parts,timeout=15):
        try:return subprocess.run([str(args.adb),'-s',serial,*parts],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout)
        except (subprocess.TimeoutExpired,OSError):return None
    def shell(parts,timeout=15):return adb(['shell',*parts],timeout)
    def read(parts):
        r=shell(parts)
        if r is None or r.returncode:raise RuntimeError('read-only device query failed; no privilege change attempted')
        return r.stdout.decode('utf-8',errors='replace').strip()
    build=json.loads((args.build/'build.json').read_text());binary=args.build/'dre-android-vulkan-probe'
    if digest(binary)!=build['binary_sha256']:raise ValueError('built helper hash differs from manifest')
    uid=read(['id','-u'])
    if uid!='2000':raise RuntimeError('device ADB UID is not 2000; no privilege change attempted')
    actual=read(['getprop','ro.serialno'])
    if actual!=serial:raise RuntimeError('connected phone identity does not match the private DE2118 record')
    result={'schema':1,'status':'READY_NOT_EXECUTED','identity_matched':True,'uid':2000,
        'binary_sha256':build['binary_sha256'],'bytes':build['bytes'],'android_gpu_execution_pass':False,
        'root_actions_performed':False,'reboot_performed':False,'partition_writes_performed':False,
        'ios_boot_performed':False,'native_xnu_metal_proven':False}
    if not args.run:
        (args.out/'plan.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(public_summary(result)));return
    remote='/data/local/tmp/dre-shader-'+build['binary_sha256'][:12]+'-'+secrets.token_hex(4)
    if not re.fullmatch(r'/data/local/tmp/dre-shader-[0-9a-f]{12}-[0-9a-f]{8}',remote):raise ValueError('invalid generated temporary directory')
    result['temporary_directory']=remote
    (args.out/'run.json').write_text(json.dumps(result,indent=2)+'\n')
    def require(r,message):
        if r is None or r.returncode:raise RuntimeError(message)
        return r
    require(shell(['mkdir',remote]),'could not create temporary test directory')
    require(adb(['push',str(binary),remote+'/probe']),'temporary helper transfer failed')
    require(shell(['chmod','0700',remote+'/probe']),'temporary helper permission setup failed')
    got=read(['sha256sum',remote+'/probe']).split()[0]
    if got!=build['binary_sha256']:raise RuntimeError('on-device helper SHA256 mismatch; execution refused')
    if read(['id','-u'])!='2000':raise RuntimeError('ADB UID changed; execution refused without privilege changes')
    result['status']='RUNNING';(args.out/'run.json').write_text(json.dumps(result,indent=2)+'\n')
    run=shell([remote+'/probe',remote+'/result.json'],timeout=45)
    if run is not None:
        (args.out/'stdout.txt').write_bytes(run.stdout);(args.out/'stderr.private.txt').write_bytes(run.stderr)
        result['process_exit_status']=run.returncode
    result['status']='EXECUTED_PENDING_REPORT';(args.out/'run.json').write_text(json.dumps(result,indent=2)+'\n')
    post={'uid':int(read(['id','-u'])),'boot_completed':read(['getprop','sys.boot_completed'])=='1',
        'lineage_running':bool(read(['getprop','ro.lineage.version'])),'identity_matched':read(['getprop','ro.serialno'])==serial}
    (args.out/'postcheck.json').write_text(json.dumps(post,indent=2)+'\n')
    result['postcheck']=post
    if post['uid']!=2000 or not post['identity_matched']:
        result['status']='FAILED_POSTCHECK';(args.out/'run.json').write_text(json.dumps(result,indent=2)+'\n')
        raise RuntimeError('post-run identity/UID changed; no further device writes attempted')
    raw=shell(['cat',remote+'/result.json'])
    if raw is None or raw.returncode:
        result['status']='FAILED_NO_REPORT';(args.out/'run.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(public_summary(result)));raise SystemExit(1)
    device=json.loads(raw.stdout)
    # The fixture never emits a serial or UUID. Do not relax this if adding
    # vendor properties later; raw identifier queries are out of scope.
    if serial in raw.stdout.decode('utf-8',errors='replace') or any('uuid' in k.lower() or 'serial' in k.lower() for k in device):
        raise RuntimeError('unexpected private identifier in diagnostic report')
    (args.out/'device-report.json').write_text(json.dumps(device,indent=2)+'\n')
    passed=post['boot_completed'] and post['lineage_running'] and run is not None and run.returncode==0 and device.get('status')=='PASS' and device.get('uid')==2000 and (
        device.get('vendor_id')==0x5143 and device.get('device_id')==0x06010900 and device.get('device_type')==1 and
        device.get('compute_exact_pass') is True and device.get('compute_values_checked')==257 and
        device.get('input_guard_words_checked')==255 and device.get('output_guard_words_checked')==255 and
        device.get('offscreen_bgra_pass') is True and device.get('pixels_checked')==4096 and
        device.get('gpu_timestamp_activity') is True and all(x>0 for x in device.get('gpu_elapsed_ticks',[0,0])) and
        device.get('native_xnu_metal_proven') is False and device.get('mesa_ir3_execution_proven') is False)
    result.update({'status':'PASS' if passed else 'FAIL','android_gpu_execution_pass':passed,
        **{k:device.get(k) for k in ['device_name','compute_values_checked','pixels_checked','gpu_timestamp_activity','gpu_elapsed_ticks']}})
    result['evidence_sha256']={name:digest(args.out/name) for name in ['device-report.json','stdout.txt','stderr.private.txt','postcheck.json'] if (args.out/name).exists()}
    result['evidence_sha256']['build.json']=digest(args.build/'build.json')
    # Clean up only the exact generated files, after pulling the report. No
    # recursive operation or user-supplied device path is accepted.
    removed=shell(['rm',remote+'/probe',remote+'/result.json'])
    result['temporary_files_removed']=removed is not None and removed.returncode==0
    if result['temporary_files_removed']:
        directory=shell(['rmdir',remote]);result['temporary_directory_removed']=directory is not None and directory.returncode==0
    (args.out/'run.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(public_summary(result)))
    if not passed:raise SystemExit(1)

if __name__=='__main__':
    try:main()
    except (ValueError,RuntimeError) as error:raise SystemExit(str(error))
