# DE2118 installation image preparation

The installation image uses the existing userdata partition without changing
GPT. Its exact size is **54,431,395,840 bytes** (about 50.69 GiB); the HFSX
filesystem occupies the full partition rather than a fixed 16 GiB prefix.
Building an image on the host does not authorize flashing it onto the phone.

Use the D22AP iPhone X iOS 16.7.16 / 20H392 OS, SystemOS and AppOS images
identified by the matching IPSW BuildManifest. Keep those Apple files and the
resulting filesystem images outside git. Record their ZIP CRC and SHA-256
checks before use.

On Linux, install apfs-fuse, hfsprogs, rsync, Python 3, and sudo. If the host
kernel lacks HFS+, also install libguestfs-tools and a guest Linux kernel with
HFS+ support. The guestfs backend prepares files on the host filesystem and
uploads one tar stream into an appliance; it does not change the host kernel.
It preserves file ownership, modes, hard links and symlinks.

Use a Linux filesystem for both temporary files and the raw image. WSL's
Windows drive mounts are slow for the formatter's small writes. The source
images can also be copied into the task's Linux filesystem to reduce I/O
latency. Allow room for the unpacked root and the populated image.

```sh
export TMPDIR=/large-linux-volume/dre-staging
mkdir -p "$TMPDIR"
export DRE_ROOT_MOUNT_BACKEND=guestfs
export DRE_ROOT_FIRST_RUN_SETUP=1
bash tools/mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg root.img 54431395840
```

If `/dev/kvm` is available, `LIBGUESTFS_BACKEND_SETTINGS=force_kvm` requires
hardware virtualization instead of silently using slow CPU emulation. Use a
dedicated `LIBGUESTFS_CACHEDIR` when enabling KVM after a previous guestfs run,
because guestfs caches the QEMU capability probe. These variables are passed
through sudo by the guestfs backend. No host kernel replacement is needed.

The default `DRE_ROOT_MOUNT_BACKEND=kernel` retains the existing loop-mount
workflow. `DRE_ROOT_FIRST_RUN_SETUP=1` retains Setup.app and clears setup
completion flags through `sweet-root-tweaks.py --first-run-setup`. A fresh
root has no mobileactivationd container, so the activation data seed remains
pending until that container is legitimately created. Image preparation
does not prove activation or Setup completion.

The guestfs path explicitly unmounts and syncs before closing the appliance.
The legacy Linux filesystem checker can append a padded 32 KiB cache line.
The builder removes only padding outside the requested volume after checking
both original volume headers and their exact allocation geometry. It allows
at most three repair passes, each followed by a read-only check. The final
read-only `fsck.hfsplus -fn` must succeed after any repairs. Retain
that exact log, the raw image hash, and an inspection of the actual completed
HFS image with `install_readiness.py --inspect-root-only`.

An optional compiled Metal acceptance probe can be inserted before freezing
the image. Download its matching CI artifact and keep the executable,
metallib, shader source and `SHA256SUMS` together. With the raw image completely
unmounted, run the host-only installer as Linux root:

```sh
python3 devices/dre/install_metal_probe.py --artifacts /private/metal-probe \
  --image root.img --reports /private/reports/metal-probe-install
```

The installer verifies the CI files, rejects an image owned by another
process, installs the three files with fixed ownership and modes, and performs
a final read-only filesystem check. It does not autostart the probe. Installing
this executable does not provide a Metal runtime or GPU driver. After any
image modification, regenerate the mounted-root inspection, filesystem-check
proof, raw hash and sparse transport so they all identify the same image.

Create a complete initial Android sparse transport image:

```sh
python3 devices/dre/install_sparse.py root.img root.simg --manifest sparse.json
```

This encoder emits RAW and zero-FILL chunks only. It never emits DONT_CARE,
so the initial install does not depend on old LineageOS data. It bounds RAW
chunks, refuses overwrite, hashes the complete expanded contents, and records
the transport SHA-256. Real source-file holes are treated as zeros without
reading them from disk. `test_install_sparse.py` verifies the output with
Android's independent `simg2img` decoder.

When copying the raw image to another filesystem, verify the destination
independently. A successful source hash or copy command does not establish
that the destination bytes match. The sparse manifest's `expanded_sha256`
must equal the final raw image's SHA-256. Keep the complete sparse transport;
trimming zero chunks would reintroduce a dependency on old phone contents.

Do not use `tools/sparsediff.py` for the first install: it requires OLD to
match the partition's existing image. Installation still requires the exact
native kernel to pass physical userdata read, write and flush checks, plus
the separate install-readiness audit and explicit partition-write approval.
