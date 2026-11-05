# DE2118 install readiness

`install_readiness.py` is a read-only host audit. It does not connect to ADB,
run fastboot, mount a disk or write a partition. It reports three independent
gates:

| Gate | Required evidence |
| --- | --- |
| `host_prepared` | Exact-size clean journaled HFSX image, complete deterministic sparse transport, mounted-root inspection and final read-only fsck log, correct install boot payload and kernel, verified original boot backup |
| `installation_hardware_ready` | Host preparation plus native raw/block reads, bounded userdata write/readback, flush and rejected out-of-bounds accesses on the physical DE2118 with this exact kernel |
| `requested_scope_ready` | Previous gates plus this root image booting read-write, observed Setup/local activation, interactive display/touch and an A619-backed Metal workload with reference output |

The default exit-status gate is the full requested scope. `--stage prepare`
selects the offline preparation gate only; every gate remains visible in the
JSON. Exit status 2 means the selected gate is incomplete. Missing inputs,
failed measurements and evidence from a different kernel are rejected.
QEMU can establish software regressions and userspace progress, but is never
accepted as physical UFS, touch or Metal proof.

## Image inspection

The installation uses the existing `userdata` partition directly, with no
GPT changes. Its size is **54,431,395,840 bytes**, containing 13,288,915 native
4,096-byte blocks starting at LUN-0 block 1,774,120. The HFSX image must use
that entire logical capacity; filesystem free blocks provide the remaining
space. An initial sparse transport must encode every block as RAW or FILL,
including zero-filled free space. Differential/DONT_CARE images are rejected
for an initial install. The transport's expanded SHA-256 must match the raw
image, not merely its length or first filesystem header.

After constructing the image, inspect a read-only mount of that same image:

```sh
python3 devices/dre/install_readiness.py --inspect-root-only \
  --root-image /private/path/ios-dre-20H392-full.img \
  --root-tree /private/path/read-only-root \
  --out /private/path/root-inspection.json
```

The inspector checks iOS 16.7.16/20H392, plain arm64 launchd/dyld/Setup,
critical root ownership and executable modes, all 44 shared-cache files,
data-container directories, migration configuration, Setup completion flags
cleared and mobileactivationd retained. Its output is configuration evidence,
not proof that Setup or activation has run. A new image may have no genuine
mobileactivationd MCM container yet. Do not invent container metadata or mark
activation observed merely because local-development boot properties exist.

Add the result of the actual final **read-only** filesystem check to that
inspection JSON. `fsck_readonly` contains `image_sha256`, `exit_code: 0`,
`clean: true` and `log_sha256`. The audit rehashes the raw image and that log;
the image must not change after its final check. A repair run followed by no
clean read-only check is insufficient. Never fill these fields with expected
results before executing the check.

## Install boot profile

The boot payload must omit the diagnostic RAM disk and select:

```text
rd=ufsroot0 dre_ufs=1 dre_ufs_rw=1 rootrw=1 cpus=8
unentitled_ios_sim_launch=1 ignition_level=1
sweetfb_noconsole=1 sweetfb_hz=30
```

The UI profile already stages built-in HFS and the writable/unsealed root
patches. Native storage additionally needs its explicit userdata-only
read/write capability; root mounting flags do not create a write driver.
The kernel must contain `DRE UFS: userdata read-write enabled`, and the
payload must opt into it. The ADT needs D22AP `/product` metadata, iPhone10,3
identity, eight CPU nodes, the validated upper-memory window and the local
development properties used for first setup. QEMU-only flags are forbidden
in a handset payload. Console suppression starts after the first GUI swap;
panic drawing remains enabled.

Example final audit (paths are local, unpublished Apple artifacts):

```sh
python3 devices/dre/install_readiness.py \
  --root-image /private/path/ios-dre-20H392-full.img \
  --sparse-image /private/path/ios-dre-20H392-full.simg \
  --root-inspection /private/path/root-inspection.json \
  --fsck-log /private/path/final-readonly-fsck.log \
  --boot-build /private/path/install-boot-build \
  --kernel /private/path/kernel.development.dre \
  --restore-image /private/path/lineage-boot_b.img \
  --hardware-report /private/path/physical-results.json \
  --out /private/path/install-readiness.json
```

`physical-results.json` identifies `device: "DE2118"`,
`environment: "physical"` and the exact `kernel_sha256`. Its `native_ufs`
object records observed `raw_read`, `block_read`, `userdata_write_readback`,
`flush`, `out_of_bounds_rejected`, `userdata_first_block` and
`userdata_blocks`. The full UI gate also needs `root_sha256`,
`native_ufs.hfs_rw_mount`, `setup_completed`, `local_activation_observed`,
`touch_input` and `framebuffer_present`. Its `metal` object must identify
`gpu_chip_id: 0x06010900`, reject a software renderer and record actual
device creation, shader compilation, GPU command completion and matching
reference output. A power probe or Android KGSL marker alone cannot satisfy
that gate. These records must reflect measurements, not planned behavior.

## Partition-write boundary

Readiness does not grant permission to flash. A concrete initial install
requires separate approval for overwriting **userdata** and replacing
**boot_b** with the reviewed, hash-pinned images. Preserve the bootloader,
recovery, modem, persist, existing GPT and original boot backup. Slot A is
not a fallback on this handset. Recheck the actual fastboot target, slot,
partition capacities and download limit before an approved installation;
use chunked sparse flashing within that measured limit. This workflow does
not establish Apple-server activation, iTunes restore or a USB gadget driver.
