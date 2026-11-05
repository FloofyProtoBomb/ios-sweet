#!/usr/bin/env python3
"""Install the explicit Metal acceptance probe into an offline DE2118 HFS image.

This host-only operation does not contact a phone or start the probe. Passing
artifact/filesystem checks does not establish GPU or Metal support. Run as root
on Linux with libguestfs and hfsprogs; all guest paths and commands are fixed.
"""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

from install_readiness import USERDATA_BYTES, digest, inspect_hfs, require

HERE = Path(__file__).resolve().parent
FILES = ('dre-metal-probe', 'dre-metal-probe.metallib', 'dre-metal-probe.metal')
TARGETS = ('/usr/local/bin/dre-metal-probe',
           '/usr/local/share/dre-metal-probe/dre-metal-probe.metallib',
           '/usr/local/share/dre-metal-probe/dre-metal-probe.metal')


def artifact_regular(path, name):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode), 'probe artifact is not a regular file: ' + name)
    limit = (4096 if name == 'SHA256SUMS' else 64 << 20 if name == FILES[0]
             else 16 << 20 if name == FILES[1] else 1 << 20)
    require(0 < info.st_size <= limit, 'unexpected artifact size: ' + name)


def validate_executable(path):
    """Require a thin plain-arm64 iOS executable, not an arm64 macOS binary."""
    data = path.read_bytes()
    require(32 <= len(data) <= 64 << 20, 'unexpected executable size')
    magic, cpu, subtype, kind, count, length, flags, reserved = struct.unpack_from('<8I', data)
    require((magic, cpu, subtype & 0xffffff, kind) == (0xfeedfacf, 0x100000c, 0, 2),
            'probe must be a thin plain-arm64 Mach-O executable')
    require(0 < count <= 4096 and count * 8 <= length <= min(len(data) - 32, 1 << 20),
            'invalid Mach-O load-command bounds')
    offset, ios = 32, False
    for _ in range(count):
        require(offset + 8 <= 32 + length, 'truncated Mach-O command')
        command, size = struct.unpack_from('<II', data, offset)
        require(size >= 8 and size % 8 == 0 and offset + size <= 32 + length,
                'invalid Mach-O command length')
        if command == 0x32:  # LC_BUILD_VERSION: platform iOS, not simulator/macOS.
            require(size >= 24, 'truncated LC_BUILD_VERSION')
            platform, minimum, sdk, tools = struct.unpack_from('<4I', data, offset + 8)
            require(platform == 2 and minimum <= (16 << 16 | 7 << 8 | 16),
                    'probe is not compatible with the target iOS 16.7.16')
            require(24 + tools * 8 <= size, 'invalid LC_BUILD_VERSION tool table')
            ios = True
        offset += size
    require(offset == 32 + length and ios, 'missing iOS LC_BUILD_VERSION')


def validate_artifacts(directory):
    sums = directory / 'SHA256SUMS'
    artifact_regular(sums, 'SHA256SUMS')
    expected = {}
    for line in sums.read_text(encoding='ascii').splitlines():
        match = re.fullmatch(r'([0-9a-fA-F]{64}) [ *]([^/\\\s]+)', line)
        require(match is not None, 'invalid SHA256SUMS line')
        value, name = match.groups()
        require(name in FILES and name not in expected, 'unexpected or duplicate artifact name')
        expected[name] = value.lower()
    require(set(expected) == set(FILES), 'SHA256SUMS must cover exactly the three probe artifacts')
    for name in FILES:
        path = directory / name
        artifact_regular(path, name)
        require(digest(path) == expected[name], 'artifact checksum mismatch: ' + name)
    validate_executable(directory / FILES[0])
    library = directory / FILES[1]
    with library.open('rb') as stream:
        header = stream.read(32)
    require(len(header) == 32 and header[:4] == b'MTLB', 'probe library has no Metal library header')
    source = (directory / FILES[2]).read_bytes()
    require(source == (HERE / 'gpu' / 'metal_probe.metal').read_bytes(),
            'probe shader differs from the project-owned acceptance shader')
    return expected


def active_image_owners(image, proc=Path('/proc'), own_pid=None):
    """Find open file descriptors by device/inode, including deleted path names."""
    target = image.stat()
    own_pid = os.getpid() if own_pid is None else own_pid
    owners = []
    for process in proc.iterdir():
        if not process.name.isdigit() or int(process.name) == own_pid:
            continue
        try:
            fds = list((process / 'fd').iterdir())
        except FileNotFoundError:
            continue
        except PermissionError as error:
            raise ValueError('cannot inspect another process; image exclusivity is unknown') from error
        for fd in fds:
            try:
                info = fd.stat()
            except FileNotFoundError:
                continue
            except PermissionError as error:
                raise ValueError('cannot inspect an open descriptor; exclusivity is unknown') from error
            if (info.st_dev, info.st_ino) == (target.st_dev, target.st_ino):
                owners.append({'pid':int(process.name), 'fd':fd.name})
    return owners


def require_offline(image):
    require(not active_image_owners(image), 'image is in use by another process; unmount/close it first')


@contextmanager
def locked_image(image):
    import fcntl
    descriptor = os.open(image, os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        opened, named = os.fstat(descriptor), image.stat()
        require(stat.S_ISREG(opened.st_mode), 'image must be a regular file')
        require((opened.st_dev, opened.st_ino) == (named.st_dev, named.st_ino),
                'image path changed while opening it')
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError('another cooperating process has locked the image') from error
        require_offline(image)
        yield
    finally:
        os.close(descriptor)


def checked_run(argv, log, allowed=(0,), env=None):
    with log.open('wb') as stream:
        result = subprocess.run([str(item) for item in argv], stdin=subprocess.DEVNULL,
                                stdout=stream, stderr=subprocess.STDOUT, env=env, check=False)
    require(result.returncode in allowed,
            f'{Path(argv[0]).name} exited {result.returncode}; see {log}')
    return result.returncode


def inspect_guest_parents(image, reports, guestfish):
    parents = ('/usr', '/usr/local', '/usr/local/bin', '/usr/local/share',
               '/usr/local/share/dre-metal-probe')
    commands = [guestfish, '--ro', '--format=raw', '-a', image, 'run', ':',
                'mount-options', 'ro', '/dev/sda', '/']
    for path in parents:
        commands += [':', 'is-symlink', path]
    commands += [':', 'umount-all']
    log = reports / 'guestfish-parent-inspection.log'
    checked_run(commands, log)
    require(log.read_text(encoding='utf-8').splitlines() == ['false'] * len(parents),
            'probe destination has a symlink parent or unexpected inspection output')


def verify_guest_files(image, expected, reports, guestfish):
    commands = [guestfish, '--ro', '--format=raw', '-a', image, 'run', ':',
                'mount-options', 'ro', '/dev/sda', '/']
    for target in TARGETS:
        commands += [':', 'checksum', 'sha256', target]
    for target in TARGETS:
        commands += [':', 'statns', target]
    commands += [':', 'umount-all']
    log = reports / 'guestfish-installed-verification.log'
    checked_run(commands, log)
    lines = log.read_text(encoding='utf-8').splitlines()
    require(lines[:3] == [expected[name] for name in FILES], 'uploaded probe file checksum mismatch')
    remaining = '\n'.join(lines[3:])
    for key, desired in (('st_uid', [0] * 3), ('st_gid', [0] * 3),
                         ('st_mode', [stat.S_IFREG | 0o755, stat.S_IFREG | 0o644, stat.S_IFREG | 0o644])):
        values = [int(value) for value in re.findall(r'^' + key + r': (\d+)$', remaining, re.M)]
        require(values == desired, 'uploaded probe ownership/type/mode mismatch: ' + key)


def repair_and_verify(image, reports, fsck):
    module_path = HERE.parents[1] / 'tools' / 'normalize-hfs-image-size.py'
    spec = importlib.util.spec_from_file_location('dre_normalize_hfs_image', module_path)
    normalizer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(normalizer)
    for attempt in range(1, 4):
        require_offline(image)
        # hfsprogs uses exit 1 for corrected errors. Other results are failures.
        checked_run([fsck, '-fy', image], reports / f'fsck-repair-{attempt}.log', (0, 1))
        try:
            removed = normalizer.normalize(image, USERDATA_BYTES)
        except ValueError as error:
            (reports / f'normalize-{attempt}.txt').write_text(str(error) + '\n', encoding='utf-8')
            continue
        log = reports / f'fsck-readonly-{attempt}.log'
        result = checked_run([fsck, '-fn', image], log, (0, 1, 2, 3, 4, 8))
        if result == 0:
            try:
                geometry = inspect_hfs(image)
            except ValueError as error:
                (reports / f'header-verification-{attempt}.txt').write_text(str(error) + '\n', encoding='utf-8')
                continue
            return {'log':str(log), 'log_sha256':digest(log), 'exit_code':0,
                    'clean':True, 'padding_removed_last_pass':removed, 'geometry':geometry}
    raise ValueError('HFS failed clean readonly verification after three repair passes')


def install(image, artifacts, reports, guestfish='guestfish', fsck='fsck.hfsplus'):
    require(sys.platform.startswith('linux') and hasattr(os, 'geteuid') and os.geteuid() == 0,
            'offline installation requires root Linux to inspect all image owners')
    require(image.is_file() and not image.is_symlink(), 'image must be a regular nonsymlink file')
    image = image.resolve()
    reports = reports.resolve()
    reports.mkdir(parents=True, exist_ok=False)
    manifest = {'schema_version':1, 'operation':'install-explicit-metal-acceptance-probe',
                'image':str(image), 'auto_start':False, 'metal_support_observed':False,
                'phone_accessed':False, 'completed':False}
    try:
        # Copy then validate: a CI download changing later cannot change uploads.
        with tempfile.TemporaryDirectory(prefix='dre-metal-probe-') as temporary:
            staged = Path(temporary)
            for name in (*FILES, 'SHA256SUMS'):
                source = artifacts / name
                artifact_regular(source, name)
                shutil.copyfile(source, staged / name)
            manifest['artifacts_sha256'] = validate_artifacts(staged)
            with locked_image(image):
                manifest['before_geometry'] = inspect_hfs(image)
                checked_run([fsck, '-fn', image], reports / 'fsck-before-readonly.log')
                require_offline(image)
                inspect_guest_parents(image, reports, guestfish)
                require_offline(image)
                commands = [guestfish, '--rw', '--format=raw', '-a', image, 'run', ':',
                            'mount-options', 'rw,force', '/dev/sda', '/', ':',
                            'mkdir-p', '/usr/local/bin', ':', 'mkdir-p', '/usr/local/share/dre-metal-probe']
                for index, (name, target) in enumerate(zip(FILES, TARGETS)):
                    commands += [':', 'rm-f', target, ':', 'upload', staged / name, target,
                                 ':', 'chown', '0', '0', target, ':', 'chmod', '0755' if index == 0 else '0644', target]
                commands += [':', 'umount-all', ':', 'sync']
                manifest['image_mutation_started'] = True
                checked_run(commands, reports / 'guestfish-install.log')
                require_offline(image)
                manifest['fsck_readonly'] = repair_and_verify(image, reports, fsck)
                verify_guest_files(image, manifest['artifacts_sha256'], reports, guestfish)
                require_offline(image)
                manifest['image_sha256'] = digest(image)
                manifest['fsck_readonly']['image_sha256'] = manifest['image_sha256']
                manifest['image_bytes'] = image.stat().st_size
                manifest['completed'] = True
    finally:
        (reports / 'result.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--reports', type=Path)
    parser.add_argument('--validate-artifacts-only', action='store_true')
    args = parser.parse_args()
    try:
        if args.validate_artifacts_only:
            print(json.dumps({'artifacts_sha256':validate_artifacts(args.artifacts),
                              'metal_support_observed':False}, indent=2))
        else:
            require(args.image is not None and args.reports is not None, '--image and --reports are required')
            result = install(args.image, args.artifacts, args.reports)
            print(json.dumps(result, indent=2))
    except (OSError, ValueError, struct.error, UnicodeError) as error:
        parser.exit(2, f'Refused/incomplete Metal probe installation: {error}\n')


if __name__ == '__main__':
    main()
