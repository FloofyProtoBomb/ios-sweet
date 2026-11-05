#!/usr/bin/env python3
"""Boot a generic DRE UI/SMP diagnostic on QEMU; no phone or iOS root required.

Only virtual CPU affinities, GIC and timer metadata are substituted. The kernel
bytes are unchanged. Hardware report fields are explicitly simulated. This
verifies CPU startup, PID 1, identity and driver-start records, not an iOS GUI.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import time
import zlib

from xnu import mkadt, mkpayload, SPLASH

HERE = Path(__file__).resolve().parent


def symbol_values(kernel, wanted):
    result = {}
    off = 32
    for _ in range(struct.unpack_from('<I', kernel, 16)[0]):
        command, size = struct.unpack_from('<II', kernel, off)
        if command == 2:
            symbols, count, strings, _ = struct.unpack_from('<4I', kernel, off + 8)
            for i in range(count):
                index, _, _, _, value = struct.unpack_from('<IBBHQ', kernel, symbols + i * 16)
                start = strings + index
                name = kernel[start:kernel.index(0, start)].decode(errors='replace')
                if name in wanted: result[name] = value
        off += size
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('build', type=Path)
    ap.add_argument('kernel', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--zig', default='zig')
    ap.add_argument('--qemu', default='qemu-system-aarch64')
    ap.add_argument('--seconds', type=int, default=180)
    ap.add_argument('--native-map-test', action='store_true', help='exercise positive FDT map/copy/unmap ownership path; no Qualcomm MMIO')
    ap.add_argument('--native-bsd-test', action='store_true', help='exercise real BSD disk callbacks with explicitly simulated read-only data; no Qualcomm MMIO')
    args = ap.parse_args()
    if args.native_bsd_test:
        args.native_map_test = True
    if args.seconds <= 0:
        ap.error('--seconds must be positive')
    build, kernel_path, out = args.build.resolve(), args.kernel.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    info = json.loads((build / 'build-info.json').read_text())
    if not info['ui_services'] or info['private_identity_provisioned']:
        ap.error('use a generic --ui-services build without private identity input')
    kernel = kernel_path.read_bytes()
    assert hashlib.sha256(kernel).hexdigest() == info['kernel_sha256']
    assert hashlib.sha256((build / 'dre-xnu-boot.img').read_bytes()).hexdigest() == info['image_sha256']
    subprocess.run([sys.executable, str(HERE / 'virt_payload.py'), str(build), str(kernel_path),
                    '--test-serial', 'QEMU-DRE-TEST', '--qemu-smp'], check=True)
    root, _ = mkadt.adt.parse((build / 'dre-virt.adt').read_bytes())
    cpus = next(c for c in root.children if c.name == 'cpus')
    assert len(cpus.children) == info['cpus']
    extra_devices = []
    if args.native_map_test:
        if not info.get('native_ufs'):
            ap.error('--native-map-test requires a native storage image')
        from test_driver_prep import fixture
        dt = fixture(initrd=(0xa4500000, 0xa5c00000), board=True)
        # Deliberately cross a 16K page edge to test unmap rounding too.
        dt_pa = 0xa0003ff8
        (out / 'map-fdt.bin').write_bytes(dt)
        words = (0x0031534655455244, 0xa8040000, 16384, dt_pa, len(dt), 0xa8000000, 0x1000000)
        first = struct.pack('<7Q', *words)
        handoff = first + struct.pack('<Q', zlib.crc32(first))
        node = next(c for c in root.children if c.name == 'dre-ufs')
        node.props = [(k, f, handoff if k == 'dre,handoff' else v) for k, f, v in node.props]
        (build / 'dre-virt.adt').write_bytes(mkadt.adt.serialize(root))
        extra_devices = ['-device', f'loader,file=map-fdt.bin,addr={dt_pa:#x},force-raw=on']
    cmd = (build / 'virt-payload.bin').read_bytes()[0x48:0x48 + 608].split(b'\0')[0].decode()
    cmd += (' dre_report=UFS=EMU_NO_HARDWARE|USB=EMU_NO_HARDWARE|TOUCH=EMU_NO_HARDWARE'
            '|GPU_HW=EMU_NO_HARDWARE|SERIAL=EMU_TEST')
    if args.native_map_test:
        cmd += ' dre_ufs_map_test=1'
    if args.native_bsd_test:
        cmd += ' dre_ufs_bsd_test=1'
    assert len(cmd.encode()) < 608
    payload, _ = mkpayload.build(str(kernel_path), str(build / 'dre-virt.adt'), cmd, str(build / 'dre-init.macho'))
    spec = importlib.util.spec_from_file_location('qemu_common', str(HERE.parents[1] / 'tools/qemu-sweet.py'))
    common = importlib.util.module_from_spec(spec); spec.loader.exec_module(common)
    common.FB_BASE = SPLASH
    base, ram, bootargs, entry = common.build_image(payload)
    (out / 'ram.bin').write_bytes(ram)
    subprocess.run([args.zig, 'cc', '-target', 'aarch64-linux-musl', '-static', '-nostdlib',
                    '-Wl,-e,_start', '-Wl,--build-id=none', f'-DBOOTARGS_PA={bootargs}', f'-DENTRY_PA={entry}',
                    str(HERE / 'qemu_start.S'), '-o', str(out / 'start.elf')], check=True)
    subprocess.run([args.zig, 'objcopy', '-O', 'binary', '-j', '.text', str(out / 'start.elf'), str(out / 'start.bin')], check=True)
    symbols = symbol_values(kernel, {'_iso_font', '_msgbufp'})
    virtual_ram_gib = max(4, 3 + (info.get('ram_mib', 512) + 1023) // 1024)
    font = common.payload_data(payload, symbols['_iso_font'], 4096)
    command = [str(Path(args.qemu).resolve()) if Path(args.qemu).exists() else args.qemu,
        '-M', 'virt,gic-version=3,virtualization=on', '-cpu', 'cortex-a76',
        '-smp', str(info['cpus']), '-m', f'{virtual_ram_gib}G', '-display', 'none', '-serial', 'none',
        '-nodefaults', '-qmp', 'stdio', '-no-reboot',
        '-device', f'loader,file=ram.bin,addr={base:#x},force-raw=on',
        '-device', f'loader,file=start.bin,addr={common.STUB_PA:#x},force-raw=on',
        '-device', f'loader,addr={common.STUB_PA:#x},cpu-num=0', *extra_devices]
    with (out / 'stderr.txt').open('w') as stderr:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=stderr, text=True, cwd=out)
        def qmp(operation, arguments=None):
            process.stdin.write(json.dumps({'execute': operation, 'arguments': arguments or {}}) + '\n')
            process.stdin.flush()
            while True:
                line = process.stdout.readline()
                if not line: raise RuntimeError((out / 'stderr.txt').read_text())
                response = json.loads(line)
                if 'error' in response: raise RuntimeError(response['error'])
                if 'return' in response: return response['return']
        try:
            version = json.loads(process.stdout.readline())
            qmp('qmp_capabilities')
            deadline = time.monotonic() + args.seconds
            screen = ''
            tags = ('DRE identity checks PASS', f"CPUS={info['cpus']}/{info['cpus']} PASS",
                    'UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER', 'DRE init: still alive')
            if info.get('ram_mib', 512) != 512:
                tags += (f"RAM_MIB={info['ram_mib'] - 1} PASS",)
            while time.monotonic() < deadline:
                time.sleep(min(15, max(0, deadline - time.monotonic())))
                qmp('stop')
                qmp('pmemsave', {'val': SPLASH, 'size': 1080 * 2400 * 4, 'filename': 'fb.raw'})
                screen, _ = common.decode_screen((out / 'fb.raw').read_bytes(), font)
                (out / 'screen.txt').write_text(screen)
                print([s for s in screen.splitlines() if s.startswith(('CPUS=', 'UI_DRIVER_LOGS='))][-2:], flush=True)
                if 'panic(cpu' in screen or all(tag in screen for tag in tags): break
                qmp('cont')
            assert all(tag in screen for tag in tags), screen
            def save_virtual(address, size, filename):
                reply = qmp('human-monitor-command', {'command-line': f'memsave {address:#x} {size} {filename}'})
                if reply: raise RuntimeError(reply)
                return (out / filename).read_bytes()
            pointer, = struct.unpack('<Q', save_virtual(symbols['_msgbufp'], 8, 'msgptr.bin'))
            _, size, head, _, buffer = struct.unpack('<4IQ', save_virtual(pointer, 24, 'msghdr.bin'))
            assert 0 < size <= 1048576 and head < size
            raw = save_virtual(buffer, size, 'msgbuf.bin')
            log = (raw[head:] + raw[:head]).decode(errors='replace')
            (out / 'kernel-log.txt').write_text(log)
            assert 'DRE SURF: IOCoreSurfaceRoot registered' in log
            assert 'DRE FB: AppleCLCD 1080x2400 stride 4320' in log
            assert f"DRE CPU: {info['cpus']}/{info['cpus']} CPUs initialized, SGI1 verified" in log
            if info.get('native_ufs'):
                # The virtual ADT has an empty ownership slot: the driver
                # must refuse it before accessing any Qualcomm hardware.
                if args.native_bsd_test:
                    assert 'NATIVE_UFS=BSD_RAW+BLOCK+GPT_PASS + WRITE_OPEN_REJECTED' in screen
                    assert 'DRE UFS: simulated BSD test device ready, no Qualcomm MMIO' in log
                    assert 'NATIVE_STAGE=' not in screen
                    state = 'VIRTUAL_TEST:READ_PASS'
                else:
                    assert 'NATIVE_UFS=UNAVAILABLE_OR_FAILED' in screen
                    assert 'DRE UFS: native disk unavailable (6)' in log
                    assert 'NATIVE_STAGE=001 ERRNO=002' in screen
                    state = 'MAP_TEST:PASS' if args.native_map_test else 'HANDOFF:INVALID'
                assert 'NATIVE_DRIVER=' + state in screen
                if args.native_map_test:
                    assert 'DRE UFS: transient FDT mapping lifetime PASS' in log
            result = {'status': 'PASS', 'kernel_sha256': info['kernel_sha256'],
                      'physical_image_sha256': info['image_sha256'], 'cpus_active': info['cpus'],
                      'pid1_identity_checks': 'PASS', 'surface_and_framebuffer_start': 'PASS',
                      'kernel_bytes_modified': False, 'hardware_report': 'simulated',
                      'qemu': version, 'ios_user_interface_rendered': False, 'phone_tested': False}
            if info.get('native_ufs'):
                result['native_ufs_missing_ownership_refused'] = not args.native_map_test
                result['native_fdt_mapping_lifetime_test'] = args.native_map_test
                result['native_stage_errno_and_driver_status'] = True
                result['native_bsd_interface_simulated'] = args.native_bsd_test
            result['ram_window_mib'] = info.get('ram_mib', 512)
            result['virtual_machine_ram_gib'] = virtual_ram_gib
            (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            print(f"PASS QEMU {info['cpus']} active CPUs, SGIs, PID 1, identity and display services", flush=True)
        finally:
            if process.poll() is None: process.kill()
            process.wait()
