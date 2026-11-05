# Private A619 firmware provisioning draft

**UNVALIDATED. Do not run recipes during the overnight validation hold.** No
provisioning, build, test, compile, CI, image mutation, phone operation or SCM
call has been performed for this source. Firmware, private pins, images and
per-phone data stay outside git. This does not finalize native GPU or Metal.

## Concrete platform constraints

The existing PAS13 loader requires a contiguous metadata PA below 4 GiB.
Ordinary XNU allocation in this port's upper RAM bank cannot meet that PA
constraint. The opt-in shim therefore reserves a separate **64 KiB, 16 KiB
aligned arena inside its already bootloader-loaded, positively owned image**.
Its PA/size are handed to XNU independently of the firmware package. The
original image, original FDT and active UFS arena are checked for disjointness.
The metadata arena remains boot-held; this code does not invent a low-DDR
allocator or claim free ownership from an arbitrary FDT RAM interval.

The pinned full Holi source describes GPU microcode at `0x8b71a000 + 0x2000`.
The 16 KiB CPU granule covering it begins at `0x8b718000`; its lower 8 KiB
belongs to the separate IPA GSI reservation ending at `0x8b71a000`. This source
derives the actual region and neighbors from the original live FDT and
**refuses the subpage/shared-granule mapping**. It neither resets IPA nor
widens GPU ownership. DT presence cannot prove neighboring cache/alias or
controller ownership. A future bounded byte-slice mapping would need genuine
whole-granule/adjacent-owner evidence or another fully qualified mechanism;
none is supplied here.

This refusal must be handled during native configuration **before cold CP
startup/ME_INIT**. Correct firmware bytes do not resolve the reserved mapping
or SCM ownership constraints.

## Private package and kernel anchor

`provision.py` is an opt-in source recipe. A private descriptor must contain
schema1, the actual A619v1 chip ID, and SQE/ZAP records with exactly `path`,
`bytes` and an independently pinned nonzero lowercase `sha256`. ZAP must be the
complete ELF32 image; split MDT/Bxx input is not accepted. Paths may be
relative to the private descriptor. No source file contains actual firmware,
certificates, serial numbers, private hashes or a permissive wildcard pin.

When explicitly run after validation resumes, the recipe checks those pins
and creates a **fresh external directory** containing:

- `gpu-firmware.package.bin`: canonical two-entry, 16 KiB aligned package.
- `gpu-firmware.anchor.bin`: exact144-byte kernel anchor representation.
- `gpu-firmware.trust.private.h`: private generated initializer for that anchor.
- `provisioning.private.json`: private provenance/length/digest record.

The package header's hashes are self-declared and are never a trust anchor.
`trust_anchor.c` returns NULL unless an actual private initializer is compiled
with `DRE_GPU_FW_PRIVATE_TRUST_HEADER`. The target kernel must contain that
actual anchor before the boot-image recipe accepts the package. The private
anchor pins the whole canonical package and both full firmware files.

The actual XNU native loader computes SHA256 using this port's existing
`libkern/crypto/sha2.h` implementation (`SHA256_Init/Update/Final`), compares
every digest, then invokes the existing strict SQE and complete ZAP parsers.
There is no hash-success stub, no package-supplied alternate trust root and no
claim to secure an unlocked boot chain against replacement of the kernel.
Only genuine Qualcomm PAS13 authentication can authenticate the signed ZAP.

## Optional shim and image source path

`build.py --gpu-firmware-provisioning <private-directory>` is the only new
switch. It requires a native UI/UFS XNU, an exact matching private anchor in
the actual kernel, and a fresh `--out` outside the repository. It rejects a
kernel missing the actual optional firmware-source symbols. The switch does
not enable `dre_gpu_native`, SCM, GPU register access, installation or flashing.

The package is linked into a new retained section before the ordinary XNU
payload. The original payload remains last; the Linux Image size, normal RAM
and reservation guards continue covering the whole linked image. A unique
`/dre-gpu-firmware` ADT node has a fixed64-byte `dre,firmware-handoff` slot. This
distinct property avoids the existing UFS handoff fixup's namespace.
`shim_handoff.c` validates package structure and boot ownership, then fills the
CRC-covered slot. It writes neither GPU reserved RAM nor hardware registers.

Default build arguments/profile paths remain unchanged. Without the switch,
no firmware object, metadata arena, ADT node or firmware macro is added. The
existing non-firmware identity/UFS fixup body remains selected. No previously
prepared image is rewritten by writing these source changes. Actual default
image equivalence still needs deferred validation.

## Native source and PAS lifetime

`native.cpp` obtains the shared boot-held original FDT snapshot, independently
checks the existing storage/image handoff and the firmware handoff, and copies
the bounded package through a **temporary Device mapping using byte loads**.
The mapping is independently PA checked and removed correctly. Hashing and
parsing use only the private resident CPU copy. Firmware is not loaded from an
early mutable filesystem and is never exposed through an IOUserClient or log.

`region.c` follows the unique PAS13 node's `memory-region` phandle to a fixed
no-map reservation, validates RAM coverage, duplicate phandles and overlapping
owners, and records CPU-granule limitations. Geometry alone grants no lease.

Before preparing PAS buffers, the native owner must supply real exclusive
GPU/PAS13 ownership, SCM64 discovery, held SCM clocks, serialized SCM transport
and actual NC/PA proofs. Every proof callback must return exactly1. The current
GPU/IPA shared granule refuses before any mapping or SCM call. Fully owned
whole-granule layouts may use actual removable WCOMB mappings, independent
`kvtophys` checks and a genuine NC proof; no requested-option flag is sufficient.

The optional `boot_nc_pmap.inc` belongs at the **end of actual XNU
`osfmk/arm/pmap/pmap.c`**, with native source compiled using
`DRE_GPU_FW_BOOT_NC_PROOF=1`. It reads real locked PTE PA/AP/XN/AttrIndx, actual
MAIR Normal-NC byte and unmanaged low-DDR status for each full16KiB page. It
modifies no PTE or cache state. It cannot prove adjacent-owner/cache policy and
cannot admit the known shared subpage. No staging change enables it.

Only after actual provider/mapping/file checks does the native adapter derive
the existing PAS13 prerequisite mask and call `dre_a619_pas13_authenticate` at
the established CP phase. The existing helper performs supported/init-image/
memory-setup/segment-copy/auth-reset with real secure firmware. This code
implements no signature, SCM or secure-state bypass. Uncertain ownership,
mapping or SMC state retains resources. Metadata, microcode, source copies and
SCM leases remain held until successful PAS shutdown plus an actual native
proof that CP/source/firmware DMA consumers have retired.

## Deferred integration files

Root's optional source allowlist needs `gpu/firmware/package.c`, `region.c`,
`trust_anchor.c`, `native.cpp` and their headers, plus the existing SQE/ZAP/PAS
helpers and actual `native_board`/storage/libfdt support. The optional pmap
include and private anchor compilation must be explicit profile selections.
`shim_handoff.c` is only an early-shim source; `provision.py` is only a private
host recipe. Neither belongs in a normal kernel compile list. No root staging,
workflow or firmware inputs are changed by this directory.

The remaining native platform/PAS/IPA granule constraints, complete protected
raw-command VM/parser/cache/scheduler, actual shader execution and Metal API
acceptance remain unfinished. All validation is deferred by user instruction.
