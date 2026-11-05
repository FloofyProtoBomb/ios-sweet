#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Link actual C storage/touch implementations from a C++ consumer.

Reproduces the native XNU C/C++ boundary that object-only compilation missed.
This host regression neither links XNU nor qualifies hardware operation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DRE = ROOT / 'devices/dre'
SOURCES = ('storage_abi.c', 'dma_memory.c', 'touch_native/profile.c',
           'libfdt/fdt.c', 'libfdt/fdt_ro.c', 'libfdt/fdt_check.c',
           'libfdt/fdt_addresses.c')
CONSUMER = r'''
#include "storage_abi.h"
#include "dma_memory.h"
#include "touch_native/native.h"
int main() {
    const uint64_t invalid[8] = {};
    dre_tn_profile profile = {};
    if (dre_storage_crc("123456789", 9) != UINT32_C(0xcbf43926)) return 1;
    if (dre_storage_abi_valid(invalid) != 0) return 2;
    if (dre_dma_ram_allowed(nullptr, 0, 0) != 0) return 3;
    if (dre_dma_boot_owned_ram_allowed(nullptr, 0, 0) != 0) return 4;
    if (dre_tn_profile_from_fdt(nullptr, 0, &profile) != DRE_TN_ARGUMENT) return 5;
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    flags = ['-O0', '-Wall', '-Wextra', '-Werror', '-ffunction-sections',
             '-fdata-sections', '-D_POSIX_C_SOURCE=200809L',
             '-I', str(DRE), '-I', str(DRE / 'libfdt')]
    report = {'status': 'FAIL', 'host_c_cpp_link_only': True,
              'native_kernel_linked': False, 'hardware_executed': False,
              'sources': {}}

    def run(command, name):
        with (out / (name + '.log')).open('wb') as stream:
            subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                           timeout=60, check=True)

    try:
        objects = []
        for source in SOURCES:
            path = DRE / source
            obj = out / (source.replace('/', '_') + '.o')
            run([args.cc, '-std=c11', *flags, '-c', str(path), '-o', str(obj)],
                obj.name)
            objects.append(obj)
            report['sources'][source] = hashlib.sha256(path.read_bytes()).hexdigest()
        consumer = out / 'consumer.cpp'
        consumer.write_text(CONSUMER, encoding='utf-8')
        binary = out / 'c-cpp-linkage'
        # The bundled libfdt is read-only; discard the unrelated append helper
        # in fdt_addresses.c rather than inventing its absent write API.
        strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        run([args.cxx, '-std=c++11', *flags, str(consumer),
             *(str(p) for p in objects), strip, '-o', str(binary)], 'link')
        run([str(binary)], 'run')
        report['status'] = 'PASS_HOST_LINK_AND_CALLS'
    finally:
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS actual C definitions linked and called through C++ headers')


if __name__ == '__main__':
    main()
