#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and run actual portable GPU code on the host, without a device.

This is an Xcode/CI entry point for the existing production C fixtures. It
neither replaces their checks nor establishes native GPU or Metal support.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GPU = ROOT / 'devices/dre/gpu'
FIXTURES = {
    'queue': ['a619_backend.c', 'test_a619_backend.c'],
    'power': ['a619_holi_power.c', 'test_a619_holi_power.c'],
    'zap': ['a619_zap.c', 'test_a619_zap.c'],
    'pas13': ['a619_zap.c', 'a619_pas13.c', 'test_a619_pas13.c'],
    'smmu': ['a619_smmu.c', 'test_a619_smmu.c'],
    'cp': ['a619_backend.c', 'a619_cp.c', 'test_a619_cp.c'],
    'rbbm': ['a619_rbbm.c', 'test_a619_rbbm.c'],
    'vm': ['a619_smmu.c', 'a619_vm.c', 'test_a619_vm.c'],
    'factory': ['runtime/factory_probe.c', 'runtime/test_factory_probe.c'],
    'compiler-envelope': ['runtime/compiler_request.c', 'runtime/test_compiler_request.c'],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {'schema': 1, 'host_fixtures_pass': False, 'fixtures': {},
              'native_gpu_execution': False, 'metal_acceleration_pass': False}

    def run(command, log, timeout=120):
        path = out / log
        try:
            with path.open('wb') as stream:
                subprocess.run(command, cwd=ROOT, stdout=stream,
                               stderr=subprocess.STDOUT, timeout=timeout, check=True)
        except (OSError, subprocess.SubprocessError):
            if path.exists():
                print(path.read_text(encoding='utf-8', errors='replace')[-7000:],
                      file=sys.stderr, flush=True)
            raise

    try:
        run([args.cc, '--version'], 'compiler-version.log')
        for name, sources in FIXTURES.items():
            binary = out / ('test-' + name)
            command = [args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
            if name == 'factory':
                command.append('-DDRE_FACTORY_LIBRARY_ONLY')
            run([*command, *(str(GPU / source) for source in sources),
                 '-o', str(binary)], name + '-build.log')
            run([str(binary)], name + '-run.log')
            report['fixtures'][name] = {'pass': True, 'sources_sha256': {
                source: hashlib.sha256((GPU / source).read_bytes()).hexdigest()
                for source in sources}}
            print('PASS host production fixture: ' + name, flush=True)
        run([sys.executable, str(GPU / 'platform/test_platform.py'),
             '--cc', args.cc, '--out', str(out / 'platform')], 'platform.log')
        report['fixtures']['rpm-platform'] = json.loads(
            (out / 'platform/report.json').read_text(encoding='utf-8'))
        for name, directory, pattern in [
            ('compiler-guards', 'compiler', 'test_*.py'),
            ('runtime-abi', 'runtime', 'test_inspect_abi.py'),
        ]:
            run([sys.executable, '-m', 'unittest', 'discover', '-s', str(GPU / directory),
                 '-p', pattern, '-v'], name + '.log')
            report['fixtures'][name] = {'pass': True}
            print('PASS host Python checks: ' + name, flush=True)
        report['host_fixtures_pass'] = True
    except (OSError, subprocess.SubprocessError) as error:
        report['failure'] = str(error)
        raise
    finally:
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS host checks; native GPU execution and Metal remain unverified')


if __name__ == '__main__':
    main()
