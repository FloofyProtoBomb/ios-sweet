#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run production RPM/GLINK/MMIO tests; optionally cross-compile ARM64 objects.

No phone, transport, firmware, network or kernel installation is accessed.
Synthetic firmware is entirely inside the C fixture's private byte array.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SOURCES = ['rpm_smd.c', 'rpm_glink.c', 'rpm_mmio.c']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default='cc')
    p.add_argument('--zig', type=Path)
    p.add_argument('--sanitize', action='store_true', help='Linux GCC AddressSanitizer + UndefinedBehaviorSanitizer')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    if a.sanitize and (a.zig or not sys.platform.startswith('linux')):
        p.error('--sanitize requires Linux GCC without --zig')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out/'zig-global-cache'),
               ZIG_LOCAL_CACHE_DIR=str(out/'zig-local-cache'))
    report = {'schema': 1, 'production_host_tests_pass': False,
              'freestanding_arm64': {}, 'sanitizers': ['address', 'undefined'] if a.sanitize else [],
              'physical_rpm_ack_observed': False,
              'voltage_policy_qualified': False, 'gpu_power_ready': False,
              'source_sha256': {name: hashlib.sha256((HERE/name).read_bytes()).hexdigest()
                                for name in [*SOURCES, 'rpm_smd.h', 'rpm_mmio.h', 'test_rpm_smd.c']}}
    cc = [str(a.zig.resolve()), 'cc'] if a.zig else [a.cc]
    common = ['-std=c11', '-O1' if a.sanitize else '-O2', '-Wall', '-Wextra', '-Werror']
    if a.sanitize:
        common += ['-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    binary = out/('test-rpm-platform.exe' if sys.platform == 'win32' else 'test-rpm-platform')
    try:
        subprocess.run([*cc, *common, *(str(HERE/s) for s in SOURCES),
                        str(HERE/'test_rpm_smd.c'), '-o', str(binary)], check=True, env=env)
        result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20, check=True, env=env)
        (out/'host-tests.log').write_text(result.stdout+result.stderr)
        match = re.search(r'PASS (\d+) synthetic production RPM', result.stdout)
        if not match:
            raise ValueError('production fixture did not report its successful case count')
        report.update(production_host_tests_pass=True, synthetic_cases_passed=int(match[1]))
        if a.zig:
            for target, suffix in [('aarch64-linux-musl', 'o'), ('aarch64-macos-none', 'macho.o')]:
                for name in SOURCES:
                    subprocess.run([*cc, *common, '-target', target, '-ffreestanding', '-fno-builtin',
                        '-mgeneral-regs-only', '-mstrict-align', '-c', str(HERE/name), '-o',
                        str(out/(Path(name).stem+'.'+suffix))], check=True, env=env)
                report['freestanding_arm64'][target] = True
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report['failure'] = str(error)
        raise
    finally:
        (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
