# Writable HFS root regression

These tools exercise the production install kernel and HFS implementation on
a disposable semihosted disk. They never use a RAM disk, flash a phone, modify
the source image or establish physical UFS, GPU, Metal or Setup support.

Build the static, project-owned PID 1 without SDK libraries or Apple files:

```sh
zig cc -target aarch64-ios -g0 -c devices/dre/hfs-root-init.S -o /private/path/init.o
python3 tools/mkinit.py /private/path/init.o /private/path/launchd
```

On Linux with hfsprogs/libguestfs, create a new 64 MiB journaled HFSX fixture:

```sh
env LIBGUESTFS_BACKEND=direct LIBGUESTFS_BACKEND_SETTINGS=force_kvm \
  LIBGUESTFS_CACHEDIR=/private/path/guestfs-cache \
  python3 devices/dre/make_hfs_root.py --init /private/path/launchd \
  --out /private/path/new-fixture-directory
```

Run the unchanged kernel matching a production `build.py --install-root`
build. The runner works on Windows or Linux with Zig and QEMU AArch64:

```sh
python3 devices/dre/qemu_root.py /private/path/install-build \
  /private/path/kernel.development.dre /private/path/new-fixture-directory/hfs-fixture.img \
  --out /private/path/new-qemu-directory --zig /private/path/zig \
  --qemu /private/path/qemu-system-aarch64 --fixture --seconds 300
```

The runner clones logical bytes into `OUT/dre-root.img`, preserving zero holes.
NTFS allocated-range queries or Linux `SEEK_HOLE` avoid reading known holes;
their logical zero bytes still participate in the full SHA-256. Unsupported
extent queries fall back to dense reads. The sparse-copy regression checks
exact bytes and hashes, source preservation and an offset above 4 GiB.
All virtual ADT/payload changes stay in `OUT`: synthetic serial/UUID, virtual
GIC/timer/CPU affinity, synthetic ownership FDT and the three explicit flags
`dre_qemu_affinity=1 dre_ufs_map_test=1 dre_ufs_qemu_file=1`. The physical build
must contain none of these flags. Semihosting uses
`enable=on,target=native,userspace=off`; run only a trusted kernel in an
isolated output directory.

`--fixture` requires actual HFS PID 1, create/write/fsync/F_FULLFSYNC,
close/reopen/readback, both legacy and 24-byte structured disk flush ioctls,
and a later heartbeat. It also requires the simulated file provider and every
requested virtual CPU in the kernel log. `result.json`, `kernel-log.txt`,
`screen.txt`, `fb.png`, all-CPU register snapshots, raw snapshots and the exact
QEMU command remain in OUT. `kernel-log-all.txt` accumulates observed lines
across ring-buffer snapshots. QMP replies have a bounded timeout. Result fields
separate observed mount/exec records from unproven Setup functionality.
A panic, fixture failure or timeout fails the assertion.

After QEMU has stopped, verify the file independently with a separate
read-only Linux appliance:

```sh
env LIBGUESTFS_BACKEND=direct LIBGUESTFS_BACKEND_SETTINGS=force_kvm \
  LIBGUESTFS_CACHEDIR=/private/path/guestfs-cache \
  python3 devices/dre/check_hfs_root.py --image /private/path/new-qemu-directory/dre-root.img \
  --out /private/path/new-qemu-directory/host-readback-proof.json
```

This checks exact file contents and proves the verifier did not change the
scratch image. The file provider's close/reopen flush is not host fsync or
physical controller durability evidence.

For the real prepared iOS image, omit `--fixture`. It receives another isolated
scratch copy. Capture a bounded observation or provide specific repeatable
`--expect 'literal marker'` criteria. With no criteria the result is explicitly
`OBSERVED_WITHOUT_PASS_CRITERIA`; a mounted HFS volume or attempted launchd
load is never automatically called functional iOS or working Setup.app.

Observed regression (kernel `fce3ae657868be30c164dc6d2eb9001f3c215a53597813ea8998ba2172754a4d`):
eight virtual CPUs initialized, `hfs: mounted DRE-QEMU-HFS on device b(1, 0)`,
all PID 1 write/flush/readback markers and heartbeat passed. The separate
read-only appliance recovered the exact 64-byte file, SHA-256
`c1c85079bb57e0a6143446350824ff1726e6dbbae06de6fce30b6f194ab5776f`.

## First complete iOS root observation

The same kernel mounted the prepared iOS 16.7.16/20H392 HFS image and executed
Apple's launchd, Darwin Ignition and boot tasks through `launchd_cache_loader`.
The first run then panicked because that critical process could not find the
`AppleMobileFileIntegrity` IOKit service. This is distinct from the existing
built-in AMFI kernel interface and MAC policy, which were already registered.

Inspection of the actual 20H392 executable identifies an IOKit lookup for
`AppleMobileFileIntegrity`, user-client type 0 and selector 6 with one scalar
input equal to 1, a 20-byte code-directory hash and no outputs. Success means
the hash is trusted; `0xe00002f0` means not found. The port has no Apple trust
cache, so an unconditional success response would misrepresent its policy.

The same executable explicitly supports `launchd_unsecure_cache=1` before
that signature/trust path. The prepared root deliberately changes
`/System/Library/xpc/launchd.plist`, and the existing
`tools/patch-launchd-plist.py` already documents that development option.
Use the explicit option for that local configuration rather than claiming
Apple trust for the modified cache. With the option, the next observation
passed early boot, started LaunchDaemons and logged `exec SpringBoard`.
It then hit a development interrupt-hygiene panic: 14.75 ms exceeded the
13.63 ms threshold under TCG. This establishes process startup, not a working
SpringBoard display, Setup.app or physical interrupt performance.

`--virtual-latency-trace` adds XNU's existing
`interrupt-masked-debug-mode=1` only to the virtual payload. This retains
latency tracing instead of the development panic, because host scheduling
delays under TCG are not handset timing evidence. It does not change the
production build; the handset readiness validator forbids that argument.
