#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Translate a real iOS AIR/metallib pipeline; never claim GPU execution.

Requires Linux process resource limits, a hash-pinned metal2vulkan binary and
SPIRV-Tools. The device profile is supplied explicitly; it is not a measured
DE2118 capability claim. Output directories contain private shader artifacts.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import time

from metal_input import bounded_file, inspect_file, require

UPSTREAM = 'https://github.com/steelbrain/metal2vulkan'
UPSTREAM_COMMIT = '219f01365dc4c8eb346816c405b5b17403793764'
WALL_SECONDS = 20
MEMORY_BYTES = 500 * 1024 * 1024
MAX_ARTIFACT_BYTES = 16 * 1024 * 1024


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run_bounded(command, log, deadline, cwd, environment=None, *, file_limit=MAX_ARTIFACT_BYTES):
    require(sys.platform.startswith('linux'), 'Linux resource limits are required for this offline worker')
    import resource
    remaining = deadline - time.monotonic()
    require(remaining > 0, 'pipeline exceeded its wall-clock budget')
    def limits():
        resource.setrlimit(resource.RLIMIT_AS, (MEMORY_BYTES, MEMORY_BYTES))
        resource.setrlimit(resource.RLIMIT_FSIZE, (file_limit, file_limit))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_CPU, (WALL_SECONDS, WALL_SECONDS))
    with Path(log).open('xb') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
            cwd=cwd, env=environment, start_new_session=True, preexec_fn=limits)
        try:
            code = process.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise ValueError('pipeline worker exceeded its wall-clock budget')
        finally:
            # A tool may leave a subprocess after its own failure. Own only
            # this worker's process group; never terminate another build.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    require(code == 0, f'external compiler/validator exited {code}; inspect the private worker log')


def spirv_header(path):
    value = bounded_file(path, MAX_ARTIFACT_BYTES)
    require(len(value) >= 20 and len(value) % 4 == 0, 'truncated or unaligned SPIR-V output')
    magic, version, generator, bound, reserved = struct.unpack_from('<5I', value)
    require(magic == 0x07230203 and 0x10000 <= version <= 0x10600 and reserved == 0,
            'invalid SPIR-V output header')
    require(0 < bound <= 0x1000000, 'SPIR-V ID bound exceeds the fixed worker budget')
    return {'bytes':len(value),'sha256':hashlib.sha256(value).hexdigest(),
            'version_word':version,'generator':generator,'id_bound':bound}


def translate(args):
    require(re.fullmatch('[0-9a-fA-F]{64}',args.translator_sha256) is not None,
            'explicit translator SHA256 pin is required')
    translator = args.translator.resolve(strict=True)
    validator = args.spirv_val.resolve(strict=True)
    require(translator.is_file() and os.access(translator,os.X_OK), 'translator must be executable')
    require(digest(translator) == args.translator_sha256.lower(), 'translator SHA256 pin differs')
    require(validator.is_file() and os.access(validator,os.X_OK), 'spirv-val must be executable')
    data, envelope = inspect_file(args.input)
    profile = bounded_file(args.profile, 4 * 1024 * 1024)
    require(isinstance(json.loads(profile),dict), 'device profile must be a JSON object')
    require(args.stage in ('compute','render') and args.function, 'explicit pipeline stage/function required')
    require(bool(args.fragment) == (args.stage == 'render'), 'render requires a fragment; compute does not')
    require(bool(args.color_format) == (args.stage == 'render'), 'render requires an explicit color format')
    out = args.out.resolve()
    out.mkdir(parents=True,exist_ok=False)
    report = {'schema':1,'operation':'offline-metal-bytecode-translation','completed':False,
        'input_envelope':envelope,'translator_sha256':digest(translator),
        'translator_reference':{'repository':UPSTREAM,'commit':UPSTREAM_COMMIT,
            'license':'GPL-2.0-or-later OR LGPL-3.0-or-later',
            'binary_source_correspondence':'caller-supplied binary hash; build provenance must be retained'},
        'validator_sha256':digest(validator),'profile_sha256':hashlib.sha256(profile).hexdigest(),
        'profile_is_measured_de2118':False,'stage':args.stage,
        'limits':{'wall_seconds':WALL_SECONDS,'address_space_bytes':MEMORY_BYTES},
        'gpu_execution_observed':False,'metal_runtime_implemented':False,
        'a619_machinecode_generated':False,'phone_operations_performed':False,'outputs':[]}
    started = time.monotonic()
    try:
        # Freeze both inputs; a later source-file update cannot change this job.
        source = out / ('input.metallib' if envelope['kind']=='ios-metallib' else 'input.air')
        source.write_bytes(data)
        selected_profile = out / 'device-profile.json'
        selected_profile.write_bytes(profile)
        output = out / 'pipeline.spv'
        command = [str(translator),str(source),str(output),'--profile',str(selected_profile),
            '--function',args.function,'--emit-meta',str(out/'pipeline.json'),'--timeout-ms','20000']
        if args.fragment:
            command += ['--fragment',args.fragment,'--color-format',args.color_format]
        environment = dict(os.environ,METAL2VULKAN_SPIRV_VAL=str(validator),
                           METAL2VULKAN_REPRO_DIR=str(out/'private-repros'))
        deadline = started + WALL_SECONDS
        run_bounded(command,out/'translate.log',deadline,out,environment)
        names = ['pipeline.spv'] if args.stage=='compute' else ['pipeline.vert.spv','pipeline.frag.spv']
        for name in names:
            path = out/name
            facts = spirv_header(path)
            run_bounded([str(validator),'--target-env','vulkan1.2',str(path)],
                        out/(name+'.validation.log'),deadline,out,environment)
            report['outputs'].append(dict(facts,file=name,spirv_val_vulkan12_pass=True))
        require((out/'pipeline.json').is_file(), 'translator did not emit binding/pipeline metadata')
        report['pipeline_metadata_sha256'] = digest(out/'pipeline.json')
        report['completed'] = True
        report['selected_pipeline_translated_and_spirv_validated'] = True
    except (ValueError,OSError) as error:
        report['failure'] = str(error)
        raise
    finally:
        report['elapsed_seconds'] = time.monotonic() - started
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('input','profile','translator','spirv-val','out'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--translator-sha256',required=True)
    p.add_argument('--stage',choices=('compute','render'),required=True)
    p.add_argument('--function',required=True)
    p.add_argument('--fragment')
    p.add_argument('--color-format')
    args=p.parse_args()
    try:
        print(json.dumps(translate(args),indent=2))
    except (ValueError,OSError) as error:
        p.exit(2,'Offline translation refused/incomplete: '+str(error)+'\n')


if __name__ == '__main__':main()
