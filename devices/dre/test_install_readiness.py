#!/usr/bin/env python3
"""Malformed artifact and false-readiness regressions; no phone or Apple files."""
import argparse
import copy
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import zlib

spec = importlib.util.spec_from_file_location('readiness', Path(__file__).with_name('install_readiness.py'))
r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    checks = 0
    def reject(fn, expected):
        nonlocal checks
        try:
            fn()
        except ValueError as error:
            assert expected in str(error), (str(error), expected)
        else:
            raise AssertionError('invalid artifact was accepted: ' + expected)
        checks += 1
    command = ' '.join(f'{key}={value}' for key,value in r.REQUIRED_ARGS.items())
    assert r.inspect_install_arguments(command)['rd'] == 'ufsroot0'; checks += 1
    for option in ('dre_qemu_affinity=1', 'dre_ufs_map_test=1', 'dre_ufs_bsd_test=1',
                   'dre_ufs_qemu_file=1', 'interrupt-masked-debug-mode=1'):
        reject(lambda:r.inspect_install_arguments(command + ' ' + option), 'QEMU-only')
    reject(lambda:r.inspect_install_arguments(command + ' rootrw=0'), 'duplicate')
    reject(lambda:r.inspect_install_arguments(command.replace('rootrw=1', 'rootrw=0')), 'writable-iOS-root')
    reject(lambda:r.inspect_install_arguments(command.replace('launchd_unsecure_cache=1', 'launchd_unsecure_cache=0')),
           'writable-iOS-root')
    with tempfile.TemporaryDirectory(dir=args.out) as tmp:
        tmp = Path(tmp)
        image = tmp/'hfs.img'; size = 8 << 20
        raw = bytearray(size)
        h = bytearray(512)
        struct.pack_into('>HHI', h, 0, 0x4858, 5, (1 << 8) | (1 << 13))
        struct.pack_into('>I', h, 12, 2)
        struct.pack_into('>III', h, 40, 4096, size//4096, size//4096-8)
        struct.pack_into('>II', h, 288, 3, 1)
        raw[1024:1536] = h; raw[-1024:-512] = h
        raw[3*4096+51] = 0xbc
        image.write_bytes(raw)
        assert r.inspect_hfs(image, size)['free_bytes'] == size-8*4096; checks += 1
        for field, value, reason in ((0,b'H+','case-sensitive'),(4,struct.pack('>I',1<<13),'unmounted'),
                                     (4,struct.pack('>I',(1<<8)|(1<<11)|(1<<13)),'inconsistent'),
                                     (4,struct.pack('>I',1<<8),'journaled'),
                                     (12,bytes(4),'journal info'),(40,struct.pack('>I',512),'block size'),
                                     (44,struct.pack('>I',1),'geometry'),(288,struct.pack('>I',0xffffffff),'catalog extent')):
            bad = bytearray(raw); bad[1024+field:1024+field+len(value)] = value
            image.write_bytes(bad); reject(lambda:r.inspect_hfs(image,size),reason)
        image.write_bytes(raw)
        reject(lambda:r.inspect_hfs(image,size+4096),'exact userdata')
        bad=bytearray(raw); bad[-1024:-1022]=b'H+'; image.write_bytes(bad)
        reject(lambda:r.inspect_hfs(image,size),'alternate')
        bad=bytearray(raw); struct.pack_into('>I', bad, size-1024+48, size//4096-12)
        image.write_bytes(bad)
        assert r.inspect_hfs(image,size)['free_bytes'] == size-8*4096; checks += 1
        bad=bytearray(raw); struct.pack_into('>I', bad, size-1024+44, size//4096-1)
        image.write_bytes(bad); reject(lambda:r.inspect_hfs(image,size),'alternate')
        bad=bytearray(raw); bad[3*4096+51]=0xcf; image.write_bytes(bad)
        reject(lambda:r.inspect_hfs(image,size),'binary key')

        # A first-install sparse image must deterministically encode all blocks.
        data=b'ABCD'*2048 + bytes(8192)
        header=struct.pack('<I4H4I',0xed26ff3a,1,0,28,12,4096,4,3,zlib.crc32(data))
        chunks=(struct.pack('<HHII',0xcac1,0,2,12+8192)+data[:8192]+
                struct.pack('<HHII',0xcac2,0,2,16)+bytes(4)+
                struct.pack('<HHII',0xcac4,0,0,16)+struct.pack('<I',zlib.crc32(data)))
        sparse=tmp/'root.simg'; sparse.write_bytes(header+chunks)
        sha=hashlib.sha256(data).hexdigest()
        assert r.inspect_sparse(sparse,len(data),sha)['all_blocks_encoded']; checks += 1
        reject(lambda:r.inspect_sparse(sparse,len(data), '0'*64),'SHA-256')
        reject(lambda:r.inspect_sparse(sparse,len(data)+4096,sha),'logical size')
        sparse.write_bytes(header+chunks[:-1]); reject(lambda:r.inspect_sparse(sparse,len(data),sha),'CRC')
        sparse.write_bytes(header+chunks+b'x'); reject(lambda:r.inspect_sparse(sparse,len(data),sha),'trailing')
        bad=bytearray(header+chunks); struct.pack_into('<I',bad,24,1); sparse.write_bytes(bad)
        reject(lambda:r.inspect_sparse(sparse,len(data),sha),'header checksum')
        sparse.write_bytes(struct.pack('<I4H4I',0xed26ff3a,1,0,28,12,4096,4,1,0)+
                           struct.pack('<HHII',0xcac3,0,4,12))
        reject(lambda:r.inspect_sparse(sparse,len(data),sha),'DONT_CARE')
        sparse.write_bytes(struct.pack('<I4H4I',0xed26ff3a,1,0,28,12,4096,4,1,0)+
                           struct.pack('<HHII',0xcac2,0,5,16)+bytes(4))
        reject(lambda:r.inspect_sparse(sparse,len(data),sha),'bounds')

        evidence={'device':'DE2118','environment':'physical','kernel_sha256':'a'*64,
                  'root_sha256':'b'*64,'setup_completed':True,'local_activation_observed':True,
                  'touch_input':True,'framebuffer_present':True,
                  'native_ufs':{key:True for key in ('raw_read','block_read','userdata_write_readback','flush',
                      'out_of_bounds_rejected','hfs_rw_mount')},
                  'metal':{'gpu_chip_id':0x06010900,'software_renderer':False,
                      **{key:True for key in ('device_created','shader_compiled','gpu_command_completed','output_matches_reference')}}}
        evidence['native_ufs'].update(userdata_first_block=r.USERDATA_FIRST_BLOCK,userdata_blocks=r.USERDATA_BLOCKS)
        assert r.physical_proof(evidence,'a'*64,'b'*64)['native_storage_rw']; checks += 1
        assert r.requested_scope_proof(evidence,'b'*64)['metal_workload']; checks += 1
        bad=copy.deepcopy(evidence); bad['environment']='QEMU'
        reject(lambda:r.physical_proof(bad,'a'*64,'b'*64),'physical DE2118')
        reject(lambda:r.requested_scope_proof(bad,'b'*64),'physical DE2118')
        reject(lambda:r.physical_proof(evidence,'c'*64,'b'*64),'another kernel')
        for key in ('raw_read','block_read','userdata_write_readback','flush','out_of_bounds_rejected'):
            bad=copy.deepcopy(evidence); bad['native_ufs'][key]=False
            reject(lambda:r.physical_proof(bad,'a'*64,'b'*64),key)
        bad=copy.deepcopy(evidence); bad['native_ufs']['userdata_blocks']+=1
        reject(lambda:r.physical_proof(bad,'a'*64,'b'*64),'geometry')
        bad=copy.deepcopy(evidence); bad['metal']['software_renderer']=True
        reject(lambda:r.requested_scope_proof(bad,'b'*64),'hardware renderer')
        bad=copy.deepcopy(evidence); bad['metal']['output_matches_reference']=False
        reject(lambda:r.requested_scope_proof(bad,'b'*64),'output_matches_reference')
        bad=copy.deepcopy(evidence); bad['setup_completed']=False
        reject(lambda:r.requested_scope_proof(bad,'b'*64),'completion')
    print(f'PASS {checks} artifact bounds/geometry/initial sparse and exact-hardware readiness checks')


if __name__=='__main__':
    main()
