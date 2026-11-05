#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual offline Turnip A619 pipeline compile; no shader execution or MTLDevice.

Supports the bounded compute/render contracts documented by offline_ir3.c.
Consumes the translator's real reflection instead of guessing descriptors.
"""
import argparse
import json
import os
from pathlib import Path
import time

from metal_input import bounded_file, require
from translate_offline import digest, run_bounded, spirv_header, WALL_SECONDS


RENDER_GRAPHICS_CONTRACT = {
    'topology':3,'color_attachments':[{'format':44,'write_mask':15,'blend':None}],
    'depth_format':None,'stencil_format':None,'rasterization_samples':1,
    'alpha_to_coverage':False,'alpha_to_one':False,'rasterizer_discard':False,
    'dynamic_states':[0,1,3,4,6,7,8],'input_attachments':[],
    'rasterization_order_attachment_access':False,'max_vertex_amplification_count':1,
    'declares_layer':False,'tessellation':None,'fragment_imageblock':None,
    'sample_shading':False,'vertex_writes_memory':False,'writes_viewport_index':False,
    'storage_attachments':[],'shadow_stencil':None,'early_fragment_tests':False,
    'mesh_records':None,'emulated_tessellation':None,'writes_point_size':False,
    'flat_fragment_inputs':False,'clip_distances':0,'writes_depth':False,
    'reads_barycentric_coord':False,'reads_primitive_id':False,'reads_position_depth':False,
    'fragment_writes_memory':False,'color_outputs':1,'sample_mask':None,'shader_tests':None,
    'snapshot_reads':False,'color_group':None,'logic_op':None}


def render_contract(metadata):
    require(isinstance(metadata,dict) and metadata.get('kind')=='render',
            'render metadata must be an object with render kind')
    stages=metadata.get('stages')
    require(isinstance(stages,list) and len(stages)==2,'render fixture requires vertex + fragment')
    for stage,name in zip(stages,('vert','frag')):
        require(isinstance(stage,dict) and stage.get('stage')==name and
                stage.get('entry_point')=='main' and stage.get('required_subgroup_size') is None and
                stage.get('stage_flags')==0 and stage.get('specialization')=={'constants':[]},
                'render fixture cannot apply specialization, subgroup or stage flags')
    require(metadata.get('layout')=={'sets':[{'set':0,'bindings':[],'flags':0}],
            'push_constants':[],'push_descriptor_eligible':False},
            'render fixture supports no resource bindings or push constants')
    require(metadata.get('vertex_input')=={'bindings':[],'attributes':[],'pulled':[],'variant_pulled':[]},
            'render fixture supports procedural vertices only')
    require(metadata.get('graphics')==RENDER_GRAPHICS_CONTRACT,
            'render state differs from the bounded triangle-list/BGRA8 contract')


def layout_contract(metadata):
    require(isinstance(metadata,dict),'pipeline metadata must be an object')
    require(metadata.get('kind')=='compute', 'only compute pipelines are supported by this offline fixture')
    stages=metadata.get('stages',[])
    require(isinstance(stages,list) and len(stages)==1 and isinstance(stages[0],dict) and
            stages[0].get('stage')=='comp' and stages[0].get('entry_point')=='main' and
            stages[0].get('required_subgroup_size') is None and stages[0].get('stage_flags',0)==0,
            'unsupported compute stage, entry point or subgroup/flags')
    layout=metadata.get('layout',{})
    require(isinstance(layout,dict),'pipeline layout must be an object')
    sets=layout.get('sets',[])
    require(isinstance(sets,list) and len(sets)==1 and isinstance(sets[0],dict) and
            sets[0].get('set')==0 and sets[0].get('flags')==0,
            'offline fixture requires one unflagged descriptor set0')
    bindings=sets[0].get('bindings',[])
    require(isinstance(bindings,list) and 0<len(bindings)<=32, 'offline descriptor count exceeds fixed budget')
    for index,binding in enumerate(bindings):
        require(isinstance(binding,dict) and binding.get('binding')==index and binding.get('ty')==7 and
                binding.get('count')==1 and binding.get('stages')==32 and
                binding.get('flags')==0 and binding.get('immutable_sampler') is None,
                'offline fixture supports sequential set0 storage buffers only')
    pushes=layout.get('push_constants',[])
    require(isinstance(pushes,list) and len(pushes)<=1, 'offline fixture supports at most one push range')
    size=0
    if pushes:
        push=pushes[0];require(isinstance(push,dict),'push range must be an object');size=push.get('size',-1)
        require(push.get('stages')==32 and push.get('offset')==0 and
                isinstance(size,int) and 0<size<=128 and size%4==0, 'unsupported push range')
    return len(bindings),size


def compile_pipeline(args):
    source=args.translated.resolve(strict=True)
    translation=json.loads(bounded_file(source/'report.json',4*1024*1024))
    require(translation.get('completed') is True and translation.get('stage') in ('compute','render') and
            translation.get('selected_pipeline_translated_and_spirv_validated') is True,
            'a complete validated compute/render translation is required')
    require(digest(source/'pipeline.json')==translation['pipeline_metadata_sha256'], 'pipeline metadata changed')
    metadata=json.loads(bounded_file(source/'pipeline.json',4*1024*1024))
    stage=translation['stage'];bindings=push=0
    if stage=='compute':bindings,push=layout_contract(metadata)
    else:render_contract(metadata)
    names=['pipeline.spv'] if stage=='compute' else ['pipeline.vert.spv','pipeline.frag.spv']
    require(len(translation['outputs'])==len(names),'SPIR-V stage count differs')
    codes=[]
    for name,output in zip(names,translation['outputs']):
        code=source/name;facts=spirv_header(code)
        require(output['file']==name and facts['sha256']==output['sha256'] and
                output.get('spirv_val_vulkan12_pass') is True,
                'SPIR-V changed after its mandatory validation')
        codes.append((code,facts))
    build=args.mesa_build.resolve(strict=True)
    driver=build/'src/freedreno/vulkan/libvulkan_freedreno.so'
    shim=build/'src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so'
    fixture=args.fixture.resolve(strict=True)
    for path in (driver,shim,fixture):require(path.is_file(),'missing offline compiler artifact')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    report={'schema':1,'operation':'offline-a619-pipeline-compilation','completed':False,
        'target_chip_id':0x06010900,'frontend':'Mesa no-op DRM; not hardware',
        'mesa_driver_sha256':digest(driver),'mesa_noop_shim_sha256':digest(shim),
        'fixture_sha256':digest(fixture),'stage':stage,
        'input_spirv':[dict(file=code.name,sha256=facts['sha256']) for code,facts in codes],
        'bindings':bindings,'push_bytes':push,'gpu_execution_observed':False,
        'metal_runtime_implemented':False,'queue_submission_performed':False,
        'raw_gpu_binary_exported':False,'phone_operations_performed':False,'representations':[]}
    # Mesa src/drm-shim/device.c creates a sparse 4-GiB memfd aperture with
    # ftruncate. It is not 4 GiB resident memory. Retain the same 500-MiB
    # address-space ceiling and C's 16-MiB per-representation bound.
    report['limits']={'wall_seconds':WALL_SECONDS,'address_space_bytes':500*1024*1024,
                      'sparse_memfd_logical_bytes':4*1024*1024*1024}
    started=time.monotonic()
    try:
        frozen=[]
        for code,facts in codes:
            copy=out/code.name;copy.write_bytes(bounded_file(code,16*1024*1024))
            require(digest(copy)==facts['sha256'],'SPIR-V changed while freezing input')
            frozen.append(str(copy))
        icd=out/'offline-icd.json'
        icd.write_text(json.dumps({'file_format_version':'1.0.0',
            'ICD':{'library_path':str(driver),'api_version':'1.3.0'}})+'\n')
        env=dict(os.environ,LD_PRELOAD=str(shim),FD_GPU_ID='619',VK_ICD_FILENAMES=str(icd),
                 VK_DRIVER_FILES=str(icd),MESA_SHADER_CACHE_DISABLE='true')
        command=[str(fixture),*frozen,str(bindings),str(push),str(out/'pipeline')] if stage=='compute' else \
                [str(fixture),'--render',*frozen,str(out/'pipeline')]
        run_bounded(command,
                    out/'compile.log',started+WALL_SECONDS,out,env,file_limit=4*1024*1024*1024)
        log=(out/'compile.log').read_text()
        require('OFFLINE COMPILE ONLY; no GPU execution:' in log and 'representation=IR3 Assembly' in log,
                'offline compiler did not report an A619 IR3 representation')
        for path in sorted(out.glob('pipeline.*.txt')):
            data=bounded_file(path,16*1024*1024)
            report['representations'].append({'file':path.name,'bytes':len(data),'sha256':digest(path)})
        require(report['representations'],'no offline representations saved')
        report['completed']=True
        report['a619_pipeline_compiled_offline']=True
        report['ir3_assembly_saved']=True
    except (ValueError,OSError) as error:
        report['failure']=str(error)
        raise
    finally:
        report['elapsed_seconds']=time.monotonic()-started
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('translated','mesa-build','fixture','out'):p.add_argument('--'+name,type=Path,required=True)
    args=p.parse_args()
    try:print(json.dumps(compile_pipeline(args),indent=2))
    except (ValueError,OSError,KeyError) as error:p.exit(2,'Offline A619 compile refused/incomplete: '+str(error)+'\n')


if __name__=='__main__':main()
