# QEMU-only file-backed partition

`qemu_disk.c` provides a 4,096-byte block interface over the fixed relative
host filename `dre-root.img`. It is an explicit simulation provider for
testing HFS and iOS userspace without another handset flash. It is not a
Qualcomm UFS driver or evidence of physical DMA, GPU or storage durability.

The caller must validate the QEMU-only boot arguments and synthetic memory
ownership before calling `dre_qemu_disk_open(1, writable, &size)`. Every
semihost trap independently requires CNTFRQ_EL0 = 62,500,000 Hz; the physical
19,200,000 Hz DE2118 rejects the provider without executing HLT. Calls require
serialization by the caller. The file must already exist, be nonempty and
aligned to 4,096 bytes, and contain at most UINT32_MAX blocks. All block
indices are checked before seeking; read-only opens also reject writes.

The AArch64 ABI uses 64-bit argument fields and pointers with `HLT #0xf000`.
SYS_OPEN modes 1 (rb) and 3 (r+b) preserve the file. Reads publish output
only after a full block succeeds. Seek, read, write, close/reopen or size
errors poison the provider; subsequent I/O fails. The only filename is fixed;
the implementation has no create, remove, rename or shell operation.

`flush()` closes and reopens QEMU's stdio file and revalidates its size. This
tests the virtual filesystem synchronization path, but is not fsync and must
never be reported as physical SYNCHRONIZE_CACHE or persistence proof.

Run QEMU from a dedicated scratch directory containing a **disposable copy**
of the root image, with:

```text
-semihosting-config enable=on,target=native,userspace=off
```

Semihosting exposes the host filesystem to privileged guest code; use a
trusted kernel. Restricting this provider's filename is not a sandbox for
arbitrary guest kernel code. User-space semihosting remains disabled.

## Real AArch64 ABI checks

```sh
python devices/dre/test_qemu_disk.py --zig /path/to/zig \
  --qemu /path/to/qemu-system-aarch64 --out /scratch/qemu-disk-tests
```

Nine bare-metal EL1 tests use the production helper and synthetic scratch
files: read/write/readback, bounds, close/reopen, read-only behavior,
disabled provider with semihosting disabled, phone-frequency rejection with
semihosting disabled, zero and misaligned file sizes, actual file length and
seeks beyond 4 GiB, short-read poisoning without partial output, and a changed
length during virtual flush. Host code verifies modified and unchanged data.
Exception vectors record unexpected guest faults instead of labeling them a
test pass. Case 9 is an optional real-file oversize check needing a filesystem
that permits sparse files larger than 16 TiB; common NTFS/ext4 configurations
reject that fixture before execution.

Specifications: [Arm semihosting ABI](https://github.com/ARM-software/abi-aa/blob/main/semihosting/semihosting.rst)
and [QEMU invocation options](https://www.qemu.org/docs/master/system/invocation.html).
