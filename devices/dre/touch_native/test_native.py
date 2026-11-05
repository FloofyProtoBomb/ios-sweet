#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host failure/lifetime tests of production native-touch C components.

The peripheral/IRQ/ownership boundary is synthetic. No phone, private firmware,
boot image, kernel recipe or hardware provider is accessed. Only explicit
--private-fdt permits read-only inspection of an existing local capture.
"""
import argparse
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
DRE = HERE.parent
PRODUCTION = [*(HERE / (n + '.c') for n in
                ('profile', 'gpi_spi', 'nt36672c', 'input', 'smmu', 'qup', 'tlmm')),
              *(DRE / 'touch' / (n + '.c') for n in ('firmware', 'report', 'spi_plan'))]


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def crypto_argument(value):
    if value:
        path = Path(value)
        if not path.is_file():
            raise ValueError('explicit host crypto library is missing')
        return str(path.resolve())
    for path in (Path('/opt/homebrew/opt/openssl@3/lib/libcrypto.dylib'),
                 Path('/usr/local/opt/openssl@3/lib/libcrypto.dylib')):
        if path.is_file():
            return str(path)
    found = ctypes.util.find_library('crypto')
    if not found:
        raise ValueError('real host OpenSSL libcrypto required; install it or pass --crypto-library')
    return '-l:' + found if sys.platform.startswith('linux') else '-lcrypto'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--crypto-library')
    parser.add_argument('--zig', type=Path, help='optional freestanding ARM64 production object checks')
    parser.add_argument('--private-fdt', type=Path, help='optional read-only local captured FDT; never copied or printed')
    args = parser.parse_args()
    if os.name == 'nt':
        parser.error('host fixture driver requires Linux/macOS C compiler; use WSL on Windows')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    flags = ['-D_DEFAULT_SOURCE', '-std=c11', '-O1' if args.sanitize else '-O2',
             '-Wall', '-Wextra', '-Werror', '-I', str(DRE / 'libfdt')]
    if args.sanitize:
        flags += ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    # The selected kernel recipe includes only this read-only portion too.
    # Do not fake the unrelated fdt_appendprop writer or link an unresolved so.
    address_source = (DRE / 'libfdt/fdt_addresses.c').read_text()
    marker = '/* This function assumes'
    if address_source.count(marker) != 1:
        raise ValueError('pinned read-only FDT source boundary changed')
    address_readonly = out / 'fdt_addresses_readonly.c'
    address_readonly.write_text(address_source.split(marker)[0].rstrip() + '\n')
    fdt = [DRE / 'libfdt' / name for name in ('fdt.c', 'fdt_ro.c', 'fdt_check.c')]
    fixtures = {
        'input': ([HERE / 'input.c', DRE / 'touch/report.c'], []),
        'gpi_spi': ([HERE / 'gpi_spi.c', DRE / 'touch/spi_plan.c'], []),
        'domains': ([HERE / n for n in ('smmu.c', 'tlmm.c', 'qup.c')] + fdt + [address_readonly], []),
        'controller': ([HERE / 'nt36672c.c', DRE / 'touch/firmware.c', DRE / 'touch/report.c'],
                       [crypto_argument(args.crypto_library)]),
    }
    if args.private_fdt:
        fixtures['profile'] = ([HERE / n for n in ('profile.c', 'qup.c', 'smmu.c', 'tlmm.c')] +
                               fdt + [address_readonly], [])
    report = {'schema': 1, 'host_tests_pass': False, 'synthetic_boundaries': True,
              'sanitizers': ['address', 'undefined'] if args.sanitize else [],
              'native_touch_executed': False, 'physical_dma_proven': False,
              'physical_irq_proven': False, 'ios_input_consumption_proven': False,
              'compiler': args.cc, 'fixtures': {}, 'freestanding_arm64': {},
              'source_sha256': {str(p.relative_to(DRE)): digest(p) for p in
                                [*PRODUCTION, *HERE.glob('*.h'), *HERE.glob('test_*.c')]}}
    failure = None
    try:
        for name, (sources, link) in fixtures.items():
            executable = out / ('test-' + name)
            command = [args.cc, *flags, *(str(p) for p in sources),
                       str(HERE / ('test_' + name + '.c')), *link, '-o', str(executable)]
            build = subprocess.run(command, text=True, capture_output=True, timeout=120)
            (out / (name + '-build.log')).write_text(build.stdout + build.stderr)
            report['fixtures'][name] = {'build_exit_code': build.returncode}
            if build.returncode:
                raise RuntimeError(name + ' fixture compilation failed: ' + build.stderr[-4000:])
            command = [str(executable)]
            if name == 'profile':
                command += [str(args.private_fdt.resolve())]
            run = subprocess.run(command, text=True, capture_output=True, timeout=30)
            (out / (name + '-run.log')).write_text(run.stdout + run.stderr)
            report['fixtures'][name].update(run_exit_code=run.returncode,
                                            binary_sha256=digest(executable))
            match = re.search(r'^PASS (\d+) production ', run.stdout, re.M)
            if run.returncode or not match:
                raise RuntimeError(name + ' fixture failed: ' + run.stdout + run.stderr)
            report['fixtures'][name]['checks'] = int(match[1])
            print(run.stdout.strip(), flush=True)
        report['host_tests_pass'] = True
        if args.zig:
            env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-global'),
                       ZIG_LOCAL_CACHE_DIR=str(out / 'zig-local'))
            for p in PRODUCTION:
                obj = out / (p.parent.name + '-' + p.stem + '-arm64.o')
                result = subprocess.run([str(args.zig.resolve()), 'cc', '-target', 'aarch64-linux-musl',
                    '-std=c11', '-O2', '-ffreestanding', '-fno-builtin', '-mstrict-align',
                    '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-I', str(DRE / 'libfdt'),
                    '-c', str(p), '-o', str(obj)], text=True, capture_output=True, timeout=120, env=env)
                (out / (obj.name + '.log')).write_text(result.stdout + result.stderr)
                report['freestanding_arm64'][str(p.relative_to(DRE))] = result.returncode
                if result.returncode:
                    raise RuntimeError('freestanding ARM64 failed: ' + str(p) + '\n' + result.stderr)
    except (OSError, subprocess.SubprocessError, RuntimeError, ValueError) as error:
        failure = str(error)
        report['failure'] = failure
    finally:
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if failure:
        print(failure, file=sys.stderr)
        return 1
    print('PASS native-touch host models; no physical DMA/IRQ or iOS acceptance claimed', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
