#!/usr/bin/env python3
"""Read-only DE2118 install audit. Never connects to a phone or flashes it.

Host preparation, exact-kernel hardware proof and the user's requested Metal
scope are separate results. A prepared image is not evidence of a working port.
Use --inspect-root-only on Linux to inspect a read-only mounted image. The
normal audit consumes that JSON, bound to the exact raw-image SHA-256, plus
the final read-only filesystem-check log. It never substitutes QEMU evidence
for native storage or Metal measurements on the physical phone.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shlex
import stat
import struct
import sys
import zlib

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'tools'))
import adt

USERDATA_BYTES = 54431395840
USERDATA_FIRST_BLOCK = 0x1b1228
USERDATA_BLOCKS = 0xcac5d3
RESTORE_SHA256 = 'd12c5d921522abd545c39884f00af6265ff32f749afbdb1917aa3c741601c44c'
RW_MARKER = b'DRE UFS: userdata read-write enabled'
REQUIRED_ARGS = {'rd':'ufsroot0', 'dre_ufs':'1', 'dre_ufs_rw':'1', 'rootrw':'1',
                 'unentitled_ios_sim_launch':'1', 'ignition_level':'1', 'cpus':'8',
                 'launchd_unsecure_cache':'1',
                 'sweetfb_noconsole':'1', 'sweetfb_hz':'30'}
QEMU_ARGS = {'dre_qemu_affinity', 'dre_ufs_map_test', 'dre_ufs_bsd_test', 'dre_ufs_qemu_file',
             'interrupt-masked-debug-mode', 'tlto_us'}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def regular(path):
    require(path is not None and path.is_file(), 'missing regular artifact')
    require(stat.S_ISREG(path.stat().st_mode), 'artifact must be a regular file')


def digest(path):
    regular(path)
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inspect_hfs(path, expected_bytes=USERDATA_BYTES):
    regular(path)
    size = path.stat().st_size
    require(size == expected_bytes and size % 4096 == 0, 'raw image must fill the exact userdata partition')
    with path.open('rb') as stream:
        stream.seek(1024); header = stream.read(512)
        stream.seek(size - 1024); alternate = stream.read(512)
        require(len(header) == len(alternate) == 512, 'truncated HFS volume header')
        sig, version, attributes = struct.unpack_from('>HHI', header)
        block_size, total, free = struct.unpack_from('>III', header, 40)
        require((sig, version) == (0x4858, 5), 'expected case-sensitive HFSX version 5')
        require(block_size >= 4096 and block_size & (block_size - 1) == 0, 'invalid HFS allocation block size')
        require(total * block_size == size and 0 <= free < total, 'HFS geometry does not fill the image')
        require(attributes & (1 << 8), 'HFS volume is not cleanly unmounted')
        require(not attributes & (1 << 11), 'HFS volume is marked inconsistent')
        require(attributes & (1 << 13), 'installation root must be journaled')
        jib, = struct.unpack_from('>I', header, 12)
        require(0 < jib < total, 'missing or out-of-range journal info block')
        # The alternate header may retain stale free/file/write counters after
        # a clean Linux HFS+ unmount; only block size/count define geometry.
        require(alternate[:4] == header[:4] and alternate[40:48] == header[40:48], 'alternate HFS header geometry differs')
        catalog_start, catalog_count = struct.unpack_from('>II', header, 288)
        require(catalog_start > 0 and catalog_count > 0 and catalog_start + catalog_count <= total,
                'invalid initial catalog extent')
        # BTNodeDescriptor is 14 bytes; BTHeaderRec.keyCompareType is byte 37.
        stream.seek(catalog_start * block_size + 51)
        require(stream.read(1) == b'\xbc', 'catalog does not use case-sensitive binary key comparison')
    return {'bytes':size, 'allocation_block_size':block_size, 'allocation_blocks':total,
            'free_bytes':free * block_size, 'case_sensitive':True, 'journaled':True,
            'cleanly_unmounted':True}


def inspect_sparse(path, expected_bytes, raw_sha256):
    """Strict initial-install transport: no differential DONT_CARE chunks."""
    regular(path)
    sha = hashlib.sha256()
    counts = {'raw':0, 'fill':0, 'crc':0}
    produced, crc = 0, 0
    with path.open('rb') as stream:
        head = stream.read(28)
        require(len(head) == 28, 'truncated sparse header')
        magic, major, minor, fh, ch, block, blocks, chunks, checksum = struct.unpack('<I4H4I', head)
        require(magic == 0xed26ff3a and major == 1 and minor == 0, 'unsupported Android sparse format')
        require(28 <= fh <= 4096 and 12 <= ch <= 4096, 'invalid sparse header lengths')
        require(block == 4096 and blocks * block == expected_bytes, 'sparse logical size differs from userdata')
        require(0 < chunks <= blocks * 2 + 1, 'invalid sparse chunk count')
        stream.seek(fh)
        for _ in range(chunks):
            header = stream.read(ch)
            require(len(header) == ch, 'truncated sparse chunk header')
            kind, reserved, nblocks, total = struct.unpack_from('<HHII', header)
            require(reserved == 0 and total >= ch, 'invalid sparse chunk header')
            length = nblocks * block
            require(length <= expected_bytes - produced, 'sparse chunk exceeds userdata bounds')
            if kind == 0xcac1:
                require(nblocks > 0 and total - ch == length, 'invalid RAW sparse chunk')
                counts['raw'] += 1
                remaining = length
                while remaining:
                    data = stream.read(min(4 << 20, remaining))
                    require(data, 'truncated RAW sparse payload')
                    sha.update(data); crc = zlib.crc32(data, crc); remaining -= len(data)
            elif kind == 0xcac2:
                require(nblocks > 0 and total - ch == 4, 'invalid FILL sparse chunk')
                pattern = stream.read(4)
                require(len(pattern) == 4, 'truncated FILL pattern')
                counts['fill'] += 1
                data = pattern * ((4 << 20) // 4)
                remaining = length
                while remaining:
                    take = min(len(data), remaining)
                    sha.update(data[:take]); crc = zlib.crc32(data[:take], crc); remaining -= take
            elif kind == 0xcac3:
                raise ValueError('DONT_CARE/differential sparse chunks are forbidden for the initial install')
            elif kind == 0xcac4:
                require(nblocks == 0 and total - ch == 4, 'invalid CRC sparse chunk')
                value = stream.read(4)
                require(len(value) == 4 and struct.unpack('<I', value)[0] == crc,
                        'sparse CRC chunk does not match expanded data')
                counts['crc'] += 1
            else:
                raise ValueError('unknown sparse chunk type')
            produced += length
        require(produced == expected_bytes, 'sparse image does not cover every userdata block')
        require(not stream.read(1), 'trailing bytes after sparse chunks')
        require(checksum == 0 or checksum == crc, 'sparse header checksum mismatch')
    expanded = sha.hexdigest()
    require(expanded == raw_sha256, 'expanded sparse SHA-256 differs from raw image')
    return {'transport_bytes':path.stat().st_size, 'logical_bytes':produced,
            'expanded_sha256':expanded, 'chunks':counts, 'all_blocks_encoded':True}


def macho_arm64(path):
    regular(path)
    with path.open('rb') as stream:
        head = stream.read(32)
        require(len(head) == 32, 'truncated executable')
        magic, = struct.unpack_from('<I', head)
        if magic == 0xfeedfacf:
            cpu, sub = struct.unpack_from('<II', head, 4)
            require(cpu == 0x100000c and sub & 0xffffff == 0, 'executable is not plain arm64')
        else:
            require(head[:4] == b'\xca\xfe\xba\xbe', 'executable is not Mach-O')
            count, = struct.unpack_from('>I', head, 4)
            require(0 < count <= 32, 'invalid fat executable architecture count')
            stream.seek(8); found = None
            for _ in range(count):
                entry = stream.read(20)
                require(len(entry) == 20, 'truncated fat architecture table')
                cpu, sub, offset, size, align = struct.unpack('>5I', entry)
                if cpu == 0x100000c and sub & 0xffffff == 0:
                    require(offset + size <= path.stat().st_size and size >= 32, 'fat slice out of bounds')
                    found = offset
            require(found is not None, 'fat executable has no plain arm64 slice')
            stream.seek(found)
            thin = stream.read(12)
            require(len(thin) == 12 and struct.unpack('<III', thin) == (0xfeedfacf, 0x100000c, 0),
                    'fat arm64 slice header is invalid')


def inspect_root_tree(root):
    require(root is not None and root.is_dir(), 'missing read-only mounted iOS root tree')
    root = root.resolve()
    def inside(name):
        path = root / name
        require(path.resolve().is_relative_to(root), 'critical root path escapes the image')
        return path
    def plist(name):
        with inside(name).open('rb') as stream:
            value = plistlib.load(stream)
        require(isinstance(value, dict), 'expected a dictionary plist at ' + name)
        return value
    system = plist('System/Library/CoreServices/SystemVersion.plist')
    require(system.get('ProductVersion') == '16.7.16' and system.get('ProductBuildVersion') == '20H392',
            'iOS root version/build is not the target 16.7.16/20H392')
    for name in ('sbin/launchd', 'usr/lib/dyld'):
        path = inside(name); macho_arm64(path)
        info = path.stat()
        require(info.st_uid == info.st_gid == 0 and info.st_mode & 0o111 != 0,
                'launchd/dyld ownership or executable permission is wrong')
    setup = plist('Applications/Setup.app/Info.plist')
    require(setup.get('CFBundleIdentifier') == 'com.apple.purplebuddy', 'Setup.app is missing or replaced')
    executable = setup.get('CFBundleExecutable')
    require(isinstance(executable, str) and Path(executable).name == executable, 'invalid Setup executable name')
    macho_arm64(inside('Applications/Setup.app/' + executable))
    prefs = inside('private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist')
    if prefs.exists():
        with prefs.open('rb') as stream:
            values = plistlib.load(stream)
        require(not any(values.get(key) for key in ('SetupDone','SetupFinishedAllSteps','SetupVersion','daemonizeNeedsToRun')),
                'Setup.app has completion/skip flags set')
    for name in ('Bundle', 'Data/System', 'Shared/SystemGroup', 'Temp'):
        require(inside('private/var/containers/' + name).is_dir(), 'missing iOS data-container directory')
    jobs = plist('System/Library/xpc/launchd.plist').get('LaunchDaemons', {})
    require(isinstance(jobs, dict), 'LaunchDaemons is not a dictionary')
    labels = {v.get('Label') for v in jobs.values() if isinstance(v, dict)}
    require('com.apple.mobileactivationd' in labels, 'mobileactivationd must be retained for local setup')
    require('com.apple.CommCenter' not in labels, 'unsupported CommCenter job can block first setup')
    caches = inside('System/Library/Caches/com.apple.dyld')
    main = caches / 'dyld_shared_cache_arm64'
    regular(main)
    with main.open('rb') as stream:
        magic = stream.read(16)
    require(magic.startswith(b'dyld_v1') and b'arm64' in magic and b'arm64e' not in magic,
            'plain-arm64 dyld shared cache is missing or wrong architecture')
    files = [p for p in caches.iterdir() if p.is_file() and p.name.startswith('dyld_shared_cache_arm64')]
    require(len(files) == 44 and all(p.stat().st_size > 0 for p in files), 'target shared cache needs all 44 files')
    migration = plist('private/var/mobile/Library/Preferences/com.apple.migration.plist')
    require(migration.get('LastSystemVersion') == '20H392', 'migration target build is not configured')
    return {'product_version':'16.7.16', 'build':'20H392', 'plain_arm64_executables':True,
            'setup_retained_first_run':True, 'mobileactivationd_retained':True,
            'activation_observed':False, 'dyld_cache_files':len(files),
            'dyld_cache_bytes':sum(p.stat().st_size for p in files), 'container_directories':True}


def inspect_install_arguments(command):
    args = {}
    for word in shlex.split(command):
        key, sep, value = word.partition('=')
        require(key not in args, 'duplicate boot argument')
        args[key] = value if sep else True
    require(not QEMU_ARGS.intersection(args), 'QEMU-only arguments in handset image')
    require(all(args.get(k) == v for k,v in REQUIRED_ARGS.items()),
            'boot payload is diagnostic or lacks required writable-iOS-root arguments')
    if 'dre_quiet_boot' in args:
        require(args['dre_quiet_boot'] == '1' and '-enable_kprintf_spam' not in args,
                'invalid quiet boot profile')
    if 'dre_boot_progress' in args:
        require(args['dre_boot_progress'] == '1' and args.get('dre_quiet_boot') == '1',
                'boot checkpoints require the quiet boot profile')
    if 'dre_panel_hz' in args:
        require(args['dre_panel_hz'] in ('45', '60', '90'), 'unsupported software vsync mode')
    if 'dre_ufs_read_batch' in args:
        require(args['dre_ufs_read_batch'] == '1', 'invalid native UFS read batching flag')
    return args


def inspect_boot(build, kernel):
    require(build is not None and build.is_dir(), 'missing boot build directory')
    macho_arm64(kernel)
    info = json.loads((build / 'build-info.json').read_text(encoding='utf-8'))
    require(isinstance(info, dict), 'invalid boot metadata')
    require(info.get('device') == 'DE2118' and info.get('native_ufs') and info.get('ui_services'),
            'boot build lacks the DRE native-storage/UI profile')
    require(info.get('kernel_sha256') == digest(kernel), 'kernel hash differs from build metadata')
    boot_sha = digest(build / 'dre-xnu-boot.img')
    require(info.get('image_sha256') == boot_sha, 'boot image hash differs from build metadata')
    payload = (build / 'payload.bin').read_bytes()
    require(payload[:8] == b'SWEETXNU' and len(payload) >= 0x2a8, 'invalid shim payload')
    version, segments = struct.unpack_from('<II', payload, 8)
    require(version == 1 and 0 < segments <= 256 and 0x2a8 + segments * 32 <= len(payload),
            'invalid shim payload version or segment table')
    boot = (build / 'dre-xnu-boot.img').read_bytes()
    require(boot[:8] == b'ANDROID!' and len(boot) <= 100663296, 'invalid or oversized Android boot image')
    kernel_size, = struct.unpack_from('<I', boot, 8)
    header_version, = struct.unpack_from('<I', boot, 40)
    require(header_version == 3 and 0 < kernel_size <= len(boot) - 4096,
            'unexpected Android boot header or kernel length')
    image = boot[4096:4096 + kernel_size]
    if info.get('direct_xnu') is True:
        from direct_image import flatten
        flat, _ = flatten(payload)
        require(image.endswith(flat) and image.count(flat) == 1 and payload not in image,
                'direct boot image does not contain this exact flat XNU image')
    else:
        require(image.count(payload) == 1, 'boot image does not contain this exact payload')
    cmd = payload[0x48:0x48 + 608]
    require(b'\0' in cmd, 'boot command line is not terminated')
    args = inspect_install_arguments(cmd.split(b'\0')[0].decode('ascii'))
    quiet_boot = 'dre_quiet_boot' in args
    boot_progress = 'dre_boot_progress' in args
    read_batch = 'dre_ufs_read_batch' in args
    panel_hz = int(args['dre_panel_hz']) if 'dre_panel_hz' in args else None
    require(info.get('quiet_boot', False) == quiet_boot and info.get('software_vsync_hz') == panel_hz,
            'boot console or software vsync differs from build metadata')
    require(info.get('boot_progress', False) == boot_progress,
            'boot checkpoints differ from build metadata')
    require(info.get('ufs_read_batch', False) == read_batch,
            'native UFS read batching differs from build metadata')
    off, size = struct.unpack_from('<QQ', payload, 0x20)
    require(0 < size <= len(payload) and off <= len(payload) - size, 'payload ADT range is invalid')
    root, consumed = adt.parse(payload[off:off + size])
    require(consumed == size, 'payload ADT has trailing data')
    chosen = next(c for c in root.children if c.name == 'chosen')
    mmap = next(c for c in chosen.children if c.name == 'memory-map')
    require(mmap.prop('RAMDisk') is None, 'install boot still embeds diagnostic RAM root')
    require(root.prop('model') == b'iPhone10,3\0', 'incorrect iOS model identity')
    product = next((c for c in root.children if c.name == 'product'), None)
    require(product is not None and len(product.props) > 1, 'D22AP product metadata is absent')
    cpus = next(c for c in root.children if c.name == 'cpus')
    require(len(cpus.children) == 8, 'install ADT must select eight CPU nodes')
    dram_size = chosen.prop('dram-size')
    require(isinstance(dram_size, bytes) and len(dram_size) == 8, 'invalid ADT DRAM size')
    require(struct.unpack('<Q', dram_size)[0] >= 2047 * 1024**2,
            'install image must retain the validated 2 GiB upper RAM window')
    for key, expected in (('certificate-production-status',0), ('effective-production-status-ap',0),
                          ('certificate-security-mode',1), ('effective-security-mode-sep',1)):
        require(chosen.prop(key) == struct.pack('<I', expected), 'local activation boot properties differ')
    raw = kernel.read_bytes()
    for marker in (b'IOCoreSurfaceRoot', b'AppleCLCD', b'_sweet_hfs_register', RW_MARKER, b'rootrw\0'):
        require(marker in raw, 'kernel lacks required built-in UI/HFS/userdata-RW capability')
    require(not quiet_boot or b'dre_quiet_boot\0' in raw, 'kernel lacks quiet-console support')
    require(not boot_progress or b'dre_boot_progress\0' in raw, 'kernel lacks boot checkpoint support')
    require(not read_batch or b'dre_ufs_read_batch\0' in raw, 'kernel lacks native UFS read batching')
    require(panel_hz is None or b'dre_panel_hz\0' in raw, 'kernel lacks software vsync support')
    return {'boot_sha256':boot_sha, 'kernel_sha256':info['kernel_sha256'],
            'root_device':'ufsroot0', 'userdata_rw_opt_in':True, 'diagnostic_ram_root':False,
            'product_metadata':True, 'cpus':8, 'local_activation_properties':True}


def physical_proof(evidence, kernel_sha, root_sha):
    require(isinstance(kernel_sha, str) and len(kernel_sha) == 64, 'missing validated kernel digest')
    require(evidence.get('device') == 'DE2118' and evidence.get('environment') == 'physical',
            'hardware proof must come from the physical DE2118')
    require(evidence.get('kernel_sha256') == kernel_sha, 'hardware proof is for another kernel')
    storage = evidence.get('native_ufs', {})
    for key in ('raw_read','block_read','userdata_write_readback','flush','out_of_bounds_rejected'):
        require(storage.get(key) is True, 'native storage hardware proof missing: ' + key)
    require(storage.get('userdata_first_block') == USERDATA_FIRST_BLOCK and
            storage.get('userdata_blocks') == USERDATA_BLOCKS, 'hardware userdata geometry differs')
    return {'exact_kernel':True, 'native_storage_rw':True}


def requested_scope_proof(evidence, root_sha):
    require(evidence.get('device') == 'DE2118' and evidence.get('environment') == 'physical',
            'UI/Metal proof must come from the physical DE2118')
    require(evidence.get('root_sha256') == root_sha and root_sha is not None,
            'userspace proof is for another iOS root')
    require(evidence.get('native_ufs', {}).get('hfs_rw_mount') is True,
            'writable HFS root mount has not been observed')
    require(evidence.get('setup_completed') is True and evidence.get('local_activation_observed') is True,
            'Setup/local activation completion has not been observed')
    require(evidence.get('touch_input') is True and evidence.get('framebuffer_present') is True,
            'interactive iOS display/touch has not been observed')
    metal = evidence.get('metal', {})
    require(metal.get('gpu_chip_id') == 0x06010900 and metal.get('software_renderer') is False,
            'Metal proof must identify the A619 hardware renderer')
    for key in ('device_created','shader_compiled','gpu_command_completed','output_matches_reference'):
        require(metal.get(key) is True, 'Metal hardware workload proof missing: ' + key)
    return {'setup_completed':True, 'local_activation_observed':True, 'metal_workload':True,
            'touch_input':True, 'framebuffer_present':True}


def inspection_proof(path, raw_sha, fsck_log):
    regular(path)
    value = json.loads(path.read_text(encoding='utf-8'))
    require(isinstance(value, dict) and value.get('schema') == 1 and
            value.get('image_sha256') == raw_sha and raw_sha is not None,
            'root inspection does not name this exact image')
    contents = value.get('root_contents', {})
    require(isinstance(contents, dict), 'invalid root inspection contents')
    require(contents.get('product_version') == '16.7.16' and contents.get('build') == '20H392' and
            contents.get('dyld_cache_files') == 44, 'root inspection target/cache differs')
    for key in ('plain_arm64_executables','setup_retained_first_run','mobileactivationd_retained','container_directories'):
        require(contents.get(key) is True, 'root inspection check missing: ' + key)
    fsck = value.get('fsck_readonly', {})
    require(isinstance(fsck, dict) and fsck.get('image_sha256') == raw_sha and
            fsck.get('exit_code') == 0 and fsck.get('clean') is True and
            isinstance(fsck.get('log_sha256'), str) and len(fsck['log_sha256']) == 64,
            'final read-only fsck proof is missing or for another image')
    require(digest(fsck_log) == fsck['log_sha256'], 'filesystem-check log hash differs from image inspection')
    return contents


def audit(args):
    checks, facts = {}, {}
    def check(name, function):
        try:
            facts[name] = function()
            checks[name] = {'pass':True}
        except (ValueError, OSError, KeyError, StopIteration, struct.error, AssertionError, plistlib.InvalidFileException,
                UnicodeError, json.JSONDecodeError) as error:
            checks[name] = {'pass':False, 'reason':str(error) or 'missing expected data'}
    check('raw_hfs', lambda: inspect_hfs(args.root_image))
    check('raw_image_digest', lambda: digest(args.root_image))
    check('sparse_transport', lambda: inspect_sparse(args.sparse_image, USERDATA_BYTES, facts.get('raw_image_digest')))
    check('ios_root_contents', lambda: inspection_proof(args.root_inspection, facts.get('raw_image_digest'), args.fsck_log))
    check('install_boot', lambda: inspect_boot(args.boot_build, args.kernel))
    check('restore_backup', lambda: require(digest(args.restore_image) == RESTORE_SHA256, 'original boot_b restore hash mismatch'))
    evidence = {}
    if args.hardware_report:
        try:
            evidence = json.loads(args.hardware_report.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            pass
    if not isinstance(evidence, dict):
        evidence = {}
    check('physical_storage', lambda: physical_proof(evidence, facts.get('install_boot',{}).get('kernel_sha256'),
                                                    facts.get('raw_image_digest')))
    check('requested_ui_metal_scope', lambda: requested_scope_proof(evidence, facts.get('raw_image_digest')))
    host_names = ('raw_hfs','raw_image_digest','sparse_transport','ios_root_contents','install_boot','restore_backup')
    prepared = all(checks[name]['pass'] for name in host_names)
    install_ready = prepared and checks['physical_storage']['pass']
    requested_ready = install_ready and checks['requested_ui_metal_scope']['pass']
    return {'schema':1, 'device':'DE2118', 'target':{'ios':'16.7.16','build':'20H392','model':'iPhone10,3'},
            'partition':{'name':'userdata','bytes':USERDATA_BYTES,'first_block_4096':USERDATA_FIRST_BLOCK,
                         'blocks_4096':USERDATA_BLOCKS,'gpt_changes':False},
            'host_prepared':prepared, 'installation_hardware_ready':install_ready,
            'requested_scope_ready':requested_ready, 'checks':checks, 'facts':facts,
            'writes_partitions':False, 'specific_partition_write_approval_required':True,
            'limitations':['Offline checks cannot prove Qualcomm DMA operation, Setup completion or Metal acceleration',
                           'An initial userdata install overwrites the existing Android data partition',
                           'No iTunes restore/USB gadget compatibility is implied']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root-image','sparse-image','root-tree','root-inspection','fsck-log','boot-build','kernel','restore-image','hardware-report','out'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--inspect-root-only', action='store_true',
                        help='inspect a mounted root and bind its report to the raw image hash; add final fsck proof afterward')
    parser.add_argument('--stage', choices=('prepare','install','requested-scope'), default='requested-scope',
                        help='select the exit-status gate; every gate is always reported')
    args = parser.parse_args()
    if args.inspect_root_only:
        try:
            result = {'schema':1, 'image_sha256':digest(args.root_image),
                      'hfs_header':inspect_hfs(args.root_image), 'root_contents':inspect_root_tree(args.root_tree)}
        except (ValueError, OSError, KeyError, struct.error, plistlib.InvalidFileException) as error:
            parser.exit(2, 'root inspection failed: ' + str(error) + '\n')
    else:
        result = audit(args)
    encoded = json.dumps(result, indent=2) + '\n'
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding='utf-8')
    print(encoded, end='')
    if args.inspect_root_only:
        return 0
    gate = {'prepare':'host_prepared','install':'installation_hardware_ready',
            'requested-scope':'requested_scope_ready'}[args.stage]
    return 0 if result[gate] else 2


if __name__ == '__main__':
    sys.exit(main())
