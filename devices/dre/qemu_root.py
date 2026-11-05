#!/usr/bin/env python3
"""Boot the unchanged install kernel on a disposable semihosted HFS root.

This is explicit simulation, never native UFS/GPU/phone proof. The source root
and production build are read only. All virtual ADT/payload, root writes, logs
and framebuffer snapshots live in a new output directory. No RAM disk is used.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import queue
import re
import shlex
import struct
import subprocess
import sys
import threading
import time
import uuid
import zlib

from xnu import mkadt, mkpayload, SPLASH, BASE
from virt_payload import virtualize
from qemu_ui import symbol_values
from test_driver_prep import fixture
from install_readiness import digest, inspect_hfs, inspect_install_arguments, require

HERE = Path(__file__).resolve().parent
CHECK_CONTENT = b'DRE disposable QEMU HFSX root write/readback, not physical UFS.\n'
FIXTURE_TAGS = ('DRE_HFS_ROOT=PID1_RUNNING', 'DRE_HFS_RW=CREATE_WRITE_FSYNC_READBACK_PASS',
                'DRE_HFS_FLUSH=FULLFSYNC_LEGACY_STRUCTURED_PASS', 'DRE_HFS_ROOT=HEARTBEAT')


def allocated_ranges(stream, size):
    """Return known data extents, or None to safely fall back to dense reads."""
    if sys.platform == 'win32':
        import msvcrt
        from ctypes import wintypes
        control = ctypes.windll.kernel32.DeviceIoControl
        control.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                            ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        handle = msvcrt.get_osfhandle(stream.fileno())
        ranges, start = [], 0
        while start < size:
            query = (ctypes.c_longlong * 2)(start, size - start)
            values = (ctypes.c_longlong * 512)(); returned = wintypes.DWORD()
            success = control(handle, 0x940cf, ctypes.byref(query), ctypes.sizeof(query),
                              ctypes.byref(values), ctypes.sizeof(values), ctypes.byref(returned), None)
            more = not success and ctypes.windll.kernel32.GetLastError() == 234
            if not success and not more:
                return None
            require(returned.value % 16 == 0 and returned.value <= ctypes.sizeof(values),
                    'filesystem returned malformed allocated ranges')
            for index in range(0, returned.value // 8, 2):
                begin, length = values[index], values[index + 1]
                require(length > 0 and start <= begin < size and begin + length > begin,
                        'filesystem returned out-of-bounds allocated ranges')
                require(not ranges or begin >= ranges[-1][1], 'filesystem returned overlapping allocated ranges')
                ranges.append((begin, min(begin + length, size)))
            if not more:
                return ranges
            require(returned.value > 0 and ranges[-1][1] > start, 'allocated-range query made no progress')
            start = ranges[-1][1]
    elif hasattr(os, 'SEEK_DATA') and hasattr(os, 'SEEK_HOLE'):
        import errno
        ranges, start = [], 0
        while start < size:
            try:
                begin = os.lseek(stream.fileno(), start, os.SEEK_DATA)
                end = min(os.lseek(stream.fileno(), begin, os.SEEK_HOLE), size)
            except OSError as error:
                if error.errno == errno.ENXIO:
                    return ranges
                return None
            require(start <= begin < end <= size, 'filesystem returned malformed sparse extents')
            ranges.append((begin, end)); start = end
        return ranges
    return None


def sparse_copy(source, target):
    """Copy logical bytes while retaining zero holes, including on Windows."""
    before = source.stat()
    require(before.st_size > 0 and before.st_size % 4096 == 0, 'root must be nonempty and 4096-byte aligned')
    sha = hashlib.sha256()
    with source.open('rb') as original, target.open('xb') as copy:
        ranges = allocated_ranges(original, before.st_size)
        if ranges is None:
            ranges = [(0, before.st_size)]
        if sys.platform == 'win32':
            import msvcrt
            from ctypes import wintypes
            control = ctypes.windll.kernel32.DeviceIoControl
            control.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
            returned = wintypes.DWORD(); handle = msvcrt.get_osfhandle(copy.fileno())
            require(control(handle, 0x900c4, None, 0, None, 0, ctypes.byref(returned), None),
                    'scratch filesystem refused sparse-file flag')
            seek = ctypes.windll.kernel32.SetFilePointerEx
            seek.argtypes = [wintypes.HANDLE, ctypes.c_longlong, ctypes.c_void_p, wintypes.DWORD]
            end = ctypes.windll.kernel32.SetEndOfFile; end.argtypes = [wintypes.HANDLE]
            require(seek(handle, before.st_size, None, 0) and end(handle), 'sparse scratch sizing failed')
            copy.seek(0)
        else:
            copy.truncate(before.st_size)
        zeros = bytes(4 << 20)
        position = 0
        for begin, end in [*ranges, (before.st_size, before.st_size)]:
            gap = begin - position
            while gap:
                take = min(gap, len(zeros)); sha.update(zeros[:take]); gap -= take
            copy.seek(begin); original.seek(begin)
            remaining = end - begin
            while remaining:
                data = original.read(min(4 << 20, remaining))
                require(data, 'root was truncated during scratch copy')
                sha.update(data)
                if data.count(0) == len(data):
                    copy.seek(len(data), os.SEEK_CUR)
                else:
                    copy.write(data)
                remaining -= len(data)
            position = end
        copy.flush(); os.fsync(copy.fileno())
    after = source.stat()
    require((before.st_size, before.st_mtime_ns, before.st_ino) ==
            (after.st_size, after.st_mtime_ns, after.st_ino), 'source root changed during scratch copy')
    require(target.stat().st_size == before.st_size, 'scratch copy size changed')
    return sha.hexdigest()


def virtual_payload(build, kernel, out, latency_trace=False, ticket_timeout_us=None):
    info = json.loads((build / 'build-info.json').read_text(encoding='utf-8'))
    require(info.get('install_root') and info.get('ui_services') and info.get('native_ufs'),
            'requires a production --install-root UI/native-UFS build')
    require(digest(kernel) == info['kernel_sha256'], 'kernel differs from production build')
    require(digest(build / 'dre-xnu-boot.img') == info['image_sha256'], 'production boot image checksum mismatch')
    root, _ = mkadt.adt.parse((build / 'dre.adt').read_bytes())
    def put(node, key, value):
        require(any(k == key for k, f, v in node.props), 'missing ADT property: ' + key)
        node.props = [(k, f, value if k == key else v) for k, f, v in node.props]
    serial = b'QEMU-DRE-HFS-TEST\0'.ljust(64, b'\0')
    put(root, 'serial-number', serial)
    hardware = next(c for c in root.children if c.name == 'oneplus-hardware')
    put(hardware, 'serial-number', serial)
    hardware.props = [p for p in hardware.props if p[0] != 'imei']
    chosen = next(c for c in root.children if c.name == 'chosen')
    put(chosen, 'dre,host-uuid', uuid.uuid5(uuid.NAMESPACE_URL, 'urn:oneplus:dre:QEMU-DRE-HFS-TEST').bytes)
    virtualize(root)
    for index, cpu in enumerate(next(c for c in root.children if c.name == 'cpus').children):
        put(cpu, 'reg', mkadt.u32(index))
    dt = fixture(initrd=(0xa4500000, 0xa5c00000), board=True)
    dt_pa = 0xa0003ff8
    (out / 'map-fdt.bin').write_bytes(dt)
    first = struct.pack('<7Q', 0x0031534655455244, 0xa8040000, 16384, dt_pa,
                        len(dt), 0xa8000000, 0x1000000)
    put(next(c for c in root.children if c.name == 'dre-ufs'), 'dre,handoff',
        first + struct.pack('<Q', zlib.crc32(first)))
    adt = out / 'qemu-root.adt'; adt.write_bytes(mkadt.adt.serialize(root))
    command = (build / 'payload.bin').read_bytes()[0x48:0x48 + 608].split(b'\0', 1)[0].decode()
    inspect_install_arguments(command)
    tokens = shlex.split(command)
    require('rd=ufsroot0' in tokens and 'rootrw=1' in tokens and 'dre_ufs_rw=1' in tokens,
            'production payload is not the writable HFS root profile')
    require(not any(token.startswith(('dre_qemu_', 'dre_ufs_map_test=', 'dre_ufs_bsd_test=', 'dre_ufs_qemu_file='))
                    for token in tokens), 'production payload already contains virtual-test flags')
    command += ' dre_qemu_affinity=1 dre_ufs_map_test=1 dre_ufs_qemu_file=1'
    if latency_trace:
        # TCG host scheduling is not handset interrupt-latency evidence.
        # XNU arm_init.c's existing tunable retains trace checks, avoiding
        # its development-only 0xd0000-tick panic threshold in the VM.
        command += ' interrupt-masked-debug-mode=1'
    if ticket_timeout_us is not None:
        require(ticket_timeout_us == 1000000, 'only the bounded one-second virtual ticket timeout is supported')
        command += ' tlto_us=1000000'
    require(len(command.encode()) < 608, 'virtual root arguments exceed boot header')
    payload, _ = mkpayload.build(str(kernel), str(adt), command, None)
    (out / 'qemu-payload.bin').write_bytes(payload)
    (out / 'bootargs.txt').write_text(command + '\n', encoding='utf-8')
    return info, payload, dt_pa


def run(args):
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = args.root.resolve(); kernel = args.kernel.resolve(); build = args.build.resolve()
    geometry = inspect_hfs(source, source.stat().st_size)
    result = {'schema_version':1, 'environment':'QEMU semihosted disposable root', 'status':'INCOMPLETE',
              'phone_tested':False, 'native_ufs_proven':False, 'metal_proven':False,
              'host_hardware_durability_proven':False, 'kernel_bytes_modified':False,
              'source_root':str(source), 'source_root_geometry':geometry}
    process = None
    try:
        result['source_root_sha256'] = sparse_copy(source, out / 'dre-root.img')
        print('Copied root into isolated writable QEMU scratch file', flush=True)
        info, payload, dt_pa = virtual_payload(build, kernel, out, args.virtual_latency_trace,
                                              args.virtual_ticket_timeout_us)
        result['virtual_interrupt_hygiene_mode'] = 'trace' if args.virtual_latency_trace else 'production-default'
        result['virtual_ticket_timeout_us'] = args.virtual_ticket_timeout_us
        result['kernel_sha256'] = info['kernel_sha256']
        result['production_boot_sha256'] = info['image_sha256']
        spec = importlib.util.spec_from_file_location('qemu_common', HERE.parents[1] / 'tools/qemu-sweet.py')
        common = importlib.util.module_from_spec(spec); spec.loader.exec_module(common)
        common.FB_BASE = SPLASH
        base, ram, bootargs, entry = common.build_image(payload)
        (out / 'ram.bin').write_bytes(ram)
        subprocess.run([args.zig, 'cc', '-target', 'aarch64-linux-musl', '-static', '-nostdlib',
                        '-Wl,-e,_start', '-Wl,--build-id=none', f'-DBOOTARGS_PA={bootargs}',
                        f'-DENTRY_PA={entry}', str(HERE / 'qemu_start.S'), '-o', str(out / 'start.elf')], check=True)
        subprocess.run([args.zig, 'objcopy', '-O', 'binary', '-j', '.text', str(out / 'start.elf'),
                        str(out / 'start.bin')], check=True)
        symbols = symbol_values(kernel.read_bytes(), {'_iso_font', '_msgbufp', '_cpu_ttep', '_allproc',
                                '_native_pt_attr', '_kernel_pmap', '_proc_struct_size'})
        font = common.payload_data(payload, symbols['_iso_font'], 4096)
        ram_gib = max(4, 3 + (info.get('ram_mib', 512) + 1023) // 1024)
        command = [args.qemu, '-M', 'virt,gic-version=3,virtualization=on', '-cpu', 'cortex-a76',
                   '-smp', str(info['cpus']), '-m', f'{ram_gib}G', '-display', 'none', '-serial', 'none',
                   '-nodefaults', '-qmp', 'stdio', '-no-reboot',
                   '-semihosting-config', 'enable=on,target=native,userspace=off',
                   '-device', f'loader,file=ram.bin,addr={base:#x},force-raw=on',
                   '-device', f'loader,file=start.bin,addr={common.STUB_PA:#x},force-raw=on',
                   '-device', f'loader,addr={common.STUB_PA:#x},cpu-num=0',
                   '-device', f'loader,file=map-fdt.bin,addr={dt_pa:#x},force-raw=on']
        (out / 'qemu-command.json').write_text(json.dumps(command, indent=2) + '\n', encoding='utf-8')
        with (out / 'stderr.txt').open('w', encoding='utf-8') as stderr:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=stderr, text=True, cwd=out)
            replies = queue.Queue()
            def read_replies():
                for line in process.stdout:
                    replies.put(line)
                replies.put(None)
            threading.Thread(target=read_replies, daemon=True).start()
            def receive(timeout=30):
                try:
                    line = replies.get(timeout=timeout)
                except queue.Empty as error:
                    raise TimeoutError(f'QEMU did not answer QMP within {timeout} seconds') from error
                require(line is not None, 'QEMU closed QMP: ' + (out / 'stderr.txt').read_text(encoding='utf-8'))
                return json.loads(line)
            def qmp(operation, arguments=None, timeout=30):
                process.stdin.write(json.dumps({'execute':operation, 'arguments':arguments or {}}) + '\n')
                process.stdin.flush()
                while True:
                    reply = receive(timeout)
                    if 'error' in reply:
                        raise RuntimeError(str(reply['error']))
                    if 'return' in reply:
                        return reply['return']
            result['qemu'] = receive()
            qmp('qmp_capabilities')
            def virtual(address, size, name):
                reply = qmp('human-monitor-command', {'command-line':f'memsave {address:#x} {size} {name}'})
                require(not reply, 'QEMU virtual-memory snapshot failed: ' + str(reply))
                return (out / name).read_bytes()
            history, seen = [], set()
            def snapshot():
                registers = qmp('human-monitor-command', {'command-line':'info registers -a'})
                (out / 'registers.txt').write_text(registers, encoding='utf-8')
                qmp('pmemsave', {'val':SPLASH, 'size':1080 * 2400 * 4, 'filename':'fb.raw'})
                raw = (out / 'fb.raw').read_bytes()
                screen, _ = common.decode_screen(raw, font)
                (out / 'screen.txt').write_text(screen, encoding='utf-8')
                common.write_png(str(out / 'fb.png'), raw)
                log = ''
                try:
                    pointer, = struct.unpack('<Q', virtual(symbols['_msgbufp'], 8, 'msgptr.bin'))
                    if pointer:
                        _, size, head, _, buffer = struct.unpack('<4IQ', virtual(pointer, 24, 'msghdr.bin'))
                        require(0 < size <= 1048576 and head < size, 'msgbuf header is not initialized')
                        data = virtual(buffer, size, 'msgbuf.bin')
                        log = (data[head:] + data[:head]).decode(errors='replace')
                        (out / 'kernel-log.txt').write_text(log, encoding='utf-8')
                        for line in log.replace('\x00', '').splitlines():
                            if line and line not in seen:
                                history.append(line); seen.add(line)
                        (out / 'kernel-log-all.txt').write_text('\n'.join(history) + '\n', encoding='utf-8')
                except (ValueError, RuntimeError, struct.error) as error:
                    (out / 'msgbuf-snapshot-error.txt').write_text(str(error), encoding='utf-8')
                return screen, log
            tags = tuple(FIXTURE_TAGS if args.fixture else ()) + tuple(args.expect or ())
            deadline = time.monotonic() + args.seconds
            screen = log = ''
            while time.monotonic() < deadline:
                time.sleep(min(10, max(0, deadline - time.monotonic())))
                qmp('stop'); screen, log = snapshot()
                text = screen + '\n' + log
                if 'panic(cpu' in text or 'DRE_HFS_RW=FAIL_STAGE_' in text:
                    result['status'] = 'FAIL'; break
                if tags and all(tag in text for tag in tags):
                    result['status'] = 'PASS'; break
                qmp('cont')
            else:
                qmp('stop'); screen, log = snapshot()
                result['status'] = 'TIMEOUT' if tags else 'OBSERVED_WITHOUT_PASS_CRITERIA'
            result['expected_markers'] = list(tags)
            result['observed_markers'] = [tag for tag in tags if tag in screen + '\n' + log]
            result['framebuffer_png'] = str(out / 'fb.png')
            all_text = '\n'.join(history) + '\n' + screen
            result['simulated_file_provider_registered'] = 'DRE UFS: QEMU file provider registered' in all_text
            result['hfs_mount_records'] = re.findall(r'hfs: mounted [^\r\n]+', all_text)
            result['framebuffer_swap_records'] = re.findall(r'DRE FB: swap [^\r\n]+', all_text)
            result['launchd_exec_logged'] = bool(re.search(r'\bexec launchd\b', all_text))
            result['userspace_exec_records'] = sorted(set(re.findall(r'\bexec ([A-Za-z0-9_.-]+)', all_text)))
            result['panic_observed'] = 'panic(cpu' in all_text
            result['setup_functional_proven'] = False
            result['graphical_ui_requires_visual_inspection'] = True
            if args.fixture and result['status'] == 'PASS':
                require(result['simulated_file_provider_registered'], 'fixture did not use the file provider')
                require(f"DRE CPU: {info['cpus']}/{info['cpus']} CPUs initialized" in log,
                        'fixture did not initialize every requested virtual CPU')
                result['hfs_pid1_posix_write_readback'] = True
                result['hfs_fullfsync_and_both_flush_ioctls'] = True
            if args.capture_final_ram:
                # Guest CPUs remain stopped throughout the globals and RAM
                # reads. This is a private runtime snapshot, unlike ram.bin
                # which contains only the initial loader image.
                capture = {'status':'INCOMPLETE', 'cpus_paused':True, 'physical_base':BASE,
                           'bytes':info['ram_mib'] << 20, 'globals':{},
                           'missing_optional_globals':[]}
                result['final_memory_capture'] = capture
                for name in ('_cpu_ttep', '_allproc', '_native_pt_attr', '_kernel_pmap', '_proc_struct_size'):
                    if name in symbols:
                        filename = name.lstrip('_') + '.bin'
                        virtual(symbols[name], 8, filename)
                        capture['globals'][name] = {'address':symbols[name], 'bytes':8, 'file':filename}
                    else:
                        capture['missing_optional_globals'].append(name)
                require('_cpu_ttep' in capture['globals'] and '_allproc' in capture['globals'],
                        'runtime capture requires the page-table root and process-list symbols')
                print('Capturing paused upper RAM for offline process/stack analysis', flush=True)
                qmp('pmemsave', {'val':BASE, 'size':capture['bytes'], 'filename':'final-ram.bin'}, timeout=60)
                dump = out / 'final-ram.bin'
                require(dump.stat().st_size == capture['bytes'], 'truncated final RAM snapshot')
                capture['file'] = str(dump)
                capture['sha256'] = digest(dump)
                capture['status'] = 'COMPLETE'
            print(json.dumps({'status':result['status'], 'observed_markers':result['observed_markers']}, indent=2), flush=True)
    except Exception as error:
        result['status'] = 'FAIL'; result['error'] = str(error)
        raise
    finally:
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait()
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return 0 if result['status'] in ('PASS', 'OBSERVED_WITHOUT_PASS_CRITERIA') else 2


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('kernel', type=Path)
    parser.add_argument('root', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--zig', default='zig')
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    parser.add_argument('--seconds', type=int, default=300)
    parser.add_argument('--fixture', action='store_true', help='require the static HFS PID1 write/flush acceptance markers')
    parser.add_argument('--virtual-latency-trace', action='store_true',
                        help='QEMU payload only: trace slow interrupt handlers instead of the development panic')
    parser.add_argument('--virtual-ticket-timeout-us', type=int, choices=(1000000,),
                        help='QEMU payload only: retain a bounded one-second ticket-lock timeout')
    parser.add_argument('--capture-final-ram', action='store_true',
                        help='at final pause, privately capture upper RAM and translation/process globals for offline analysis')
    parser.add_argument('--expect', action='append', help='additional literal boot marker required for PASS')
    arguments = parser.parse_args()
    require(arguments.seconds > 0, 'timeout must be positive')
    for name in ('zig', 'qemu'):
        value = getattr(arguments, name)
        if Path(value).exists():
            setattr(arguments, name, str(Path(value).resolve()))
    sys.exit(run(arguments))
