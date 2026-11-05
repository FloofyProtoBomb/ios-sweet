#!/usr/bin/env python3
"""Qualify project-generated SPIR-V for Vulkan1.1+KHR_spirv_1_4.

Uses Khronos disassembler/assembler/validator, never a binary-header patch.
Instruction words and canonical disassembly must remain exactly unchanged.
Requires and verifies the private metallib→SPIR-V→offline-A619 report chain.
Outputs are private build artifacts, not native XNU/Metal execution evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

HERE=Path(__file__).resolve().parent
def sha(data):return hashlib.sha256(data).hexdigest()
def run(command):return subprocess.run(command,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True).stdout
def generated_header(report,code):
    header=['/* Generated from qualified project shaders; keep outside git. */','#include <stdint.h>']
    for stage,data in code.items():
        words=struct.unpack('<'+'I'*(len(data)//4),data)
        header.append('static const uint32_t dre_'+stage+'_spv[]={')
        for i in range(0,len(words),8):header.append('    '+','.join(f'0x{x:08x}' for x in words[i:i+8])+',')
        header.append('};')
    header.append('static const char dre_shader_qualification[]='+json.dumps(json.dumps(report,separators=(',',':')))+';')
    return '\n'.join(header)+'\n'
def qualify(source,out,stage,assembler,validator,disassembler):
    original=source.read_bytes()
    if len(original)<20 or len(original)>65536 or len(original)%4:raise ValueError('invalid input size')
    words=struct.unpack('<5I',original[:20])
    if words[0]!=0x07230203 or words[1]!=0x10500 or words[4]:raise ValueError('unexpected source SPIR-V header')
    assembly=run([disassembler,'--no-header','--raw-id',str(source)])
    capabilities=re.findall(r'^\s*OpCapability (\w+)\s*$',assembly,re.M)
    if capabilities!=['Shader'] or re.search(r'^\s*OpExtension ',assembly,re.M):raise ValueError('unqualified capabilities/extensions')
    imports=re.findall(r'OpExtInstImport "([^"]+)"',assembly)
    if any(x!='GLSL.std.450' for x in imports):raise ValueError('unqualified extended instruction set')
    if 'OpMemoryModel Logical GLSL450' not in assembly:raise ValueError('unexpected memory model')
    expected={'compute':'GLCompute','vertex':'Vertex','fragment':'Fragment'}[stage]
    if len(re.findall(r'OpEntryPoint ',assembly))!=1 or not re.search(r'OpEntryPoint '+expected+r' %\d+ "main"',assembly):
        raise ValueError('unexpected shader entry point')
    text=out/(stage+'.spvasm');text.write_text(assembly)
    target=out/(stage+'.spv')
    run([assembler,'--target-env','vulkan1.1spv1.4','--preserve-numeric-ids',str(text),'-o',str(target)])
    run([validator,'--target-env','vulkan1.1spv1.4',str(target)])
    result=target.read_bytes()
    if struct.unpack_from('<I',result,4)[0]!=0x10400:raise ValueError('assembler did not target SPIR-V1.4')
    if original[20:]!=result[20:] or run([disassembler,'--no-header','--raw-id',str(target)])!=assembly:
        raise ValueError('reassembly changed shader instructions/semantics')
    header=struct.unpack('<5I',result[:20])
    if header[0]!=words[0] or header[3:]!=words[3:]:raise ValueError('unexpected bounds/schema change')
    return {'stage':stage,'original_sha256':sha(original),'qualified_sha256':sha(result),
        'bytes':len(result),'source_version':words[1],'target_version':header[1],
        'generator_before':words[2],'generator_after':header[2],
        'capabilities':capabilities,'extended_instruction_sets':imports,
        'instruction_words_identical':True,'canonical_disassembly_identical':True,
        'spirv_val_target':'vulkan1.1spv1.4','validator_pass':True},result

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--results',required=True,type=Path)
    ap.add_argument('--out',required=True,type=Path)
    ap.add_argument('--spirv-as',default='spirv-as')
    ap.add_argument('--spirv-val',default='spirv-val')
    ap.add_argument('--spirv-dis',default='spirv-dis')
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    records=[];code={};metallib_hash=None
    for stage,group,name in [('compute','compute','pipeline.spv'),('vertex','render','pipeline.vert.spv'),('fragment','render','pipeline.frag.spv')]:
        directory=args.results/('final-a619-'+group);managed=args.results/('managed-'+group)
        offline=json.loads((directory/'report.json').read_text());translated=json.loads((managed/'report.json').read_text())
        if not offline.get('completed') or not offline.get('a619_pipeline_compiled_offline') or offline.get('gpu_execution_observed'):
            raise ValueError('missing/inconsistent offline compiler provenance')
        if not translated.get('completed') or not translated.get('selected_pipeline_translated_and_spirv_validated'):
            raise ValueError('missing actual metallib translation provenance')
        mh=sha((managed/'input.metallib').read_bytes())
        if mh!=translated['input_envelope']['sha256'] or (metallib_hash and mh!=metallib_hash):raise ValueError('metallib provenance mismatch')
        metallib_hash=mh;source=directory/name;source_hash=sha(source.read_bytes())
        if not any(x['file']==name and x['sha256']==source_hash for x in offline['input_spirv']) or not any(
                x['file']==name and x['sha256']==source_hash for x in translated['outputs']):raise ValueError('SPIR-V provenance hash mismatch')
        record,data=qualify(source,args.out,stage,args.spirv_as,args.spirv_val,args.spirv_dis)
        record['translation_report_sha256']=sha((managed/'report.json').read_bytes())
        record['offline_a619_report_sha256']=sha((directory/'report.json').read_bytes())
        record['translator_sha256']=translated['translator_sha256']
        record['translator_source_commit']=translated['translator_reference']['commit']
        records.append(record);code[stage]=data
    report={'schema':1,'operation':'qualify-project-metal-shaders-for-android-vulkan',
        'target_environment':'Vulkan1.1 + VK_KHR_spirv_1_4 + VK_KHR_shader_float_controls',
        'metallib_sha256':metallib_hash,'project_msl_sha256':sha((HERE.parent/'metal_probe.metal').read_bytes()),
        'shaders':records,'native_xnu_execution_proven':False,'metal_runtime_proven':False,
        'tools':{name:run([tool,'--version']).splitlines()[0] for name,tool in [
            ('assembler',args.spirv_as),('validator',args.spirv_val),('disassembler',args.spirv_dis)]}}
    (args.out/'qualification.json').write_text(json.dumps(report,indent=2)+'\n')
    (args.out/'qualified_shaders.h').write_text(generated_header(report,code))
    print(json.dumps({'qualified':True,'target':'vulkan1.1spv1.4','instruction_semantics_unchanged':True,
        'shaders':[{k:r[k] for k in ['stage','original_sha256','qualified_sha256']} for r in records]}))

if __name__=='__main__':main()
