# Source-qualified PAS13 relocation

This optional path implements a separately owned destination. It does not
establish physical PAS authentication, XPU permission, native GPU execution or
Metal support. No firmware, certificates, private hashes or identifiers are
included here. The original GPU/IPA shared granule is never mapped or written.

## Actual input and primary implementation

Read-only inspection of the exact privately pinned complete ZAP ELF and split
MDT found three program headers. Only header 2 is `PT_LOAD`; its flags are
`0x08000007`, including `QCOM_MDT_RELOCATABLE` bit 27, address `0x5000`, file and
memory size 1984, and `p_align=0x100000`. Entry `0x5000` lies inside that load
segment. The rounded loaded span is 4096 bytes. The other two headers describe
ELF/hash metadata, which must not be copied into the destination as code.
The existing extractor reproduced the private MDT byte-for-byte from the
complete ELF; neither metadata nor segment bytes were changed.

At pinned Lineage commit `98a38783f42f5588bf00cd933e299aedc589bc10`:

- [mdt_loader.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/soc/qcom/mdt_loader.c)
  recognizes bit 27, calls `PAS_INIT_IMAGE` with original metadata, calls
  `PAS_MEM_SETUP` with the selected destination and rounded image span, and
  copies each segment at `p_paddr-min_addr` before authentication.
- [peripheral-loader.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/soc/qcom/peripheral-loader.c)
  relocates entry and segments into `region_start`; its entry check requires
  the entry to remain inside a relocated segment. The selected vendor path
  obtains `region_start` from `memory-region`; this establishes the existing
  loader's choice, not a fixed address cryptographically required by the ELF.
- [subsys-pil-tz.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/soc/qcom/subsys-pil-tz.c)
  passes actual region address/size to PAS13 memory setup and holds the real
  SCM bus/regulator/clock resources during authentication/reset.
- [qcom_scm.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/firmware/qcom_scm.c)
  requires contiguous aligned noncacheable metadata because secure firmware
  changes its protection. Its wrappers accept physical relocation address and
  size; successful hardware acceptance remains independently necessary.

The new path conservatively honors the signed 1 MiB alignment even though
the vendor fixed-carveout path does not enforce base alignment from `p_align`.
Alignment is a bounds requirement, never PAS/XPU authorization.

## Actual boot storage and v2 record

`DRE_GPU_FW_RELOCATED=1` enables a dedicated initialized writable
`.data.dre_gpu_fw_load` object of `1 MiB + 16 KiB`. Existing `image.ld` includes
`.data.*` before bss, stack and payload. The object is referenced and `used`;
it is not NOLOAD BSS. The shim aligns its **actual runtime pointer** upward to
1 MiB and checks that the whole 16 KiB target fits inside that dedicated
object and the independently checked original loaded image. Relative linker
alignment at origin zero would be insufficient when Android loads the image
with a different text offset. The whole object and selected slice also pass
the original live-DT boot-owned-RAM exclusions. Padding remains boot-held.

The exact compatible string is `oneplus,dre-gpu-firmware-v2`, with one 96-byte
little-endian property. Its u64 words are:

| Word | Meaning |
| --- | --- |
| 0 | Existing `DRE_GPU_FW_HANDOFF_MAGIC` |
| 1 | `(96ULL << 32) | 2` |
| 2, 3 | Private package PA and bytes |
| 4, 5 | Separate metadata PA and 65536 bytes |
| 6 | Actual A619v1 chip ID |
| 7, 8 | Selected load PA and 16384 bytes |
| 9 | 1048576-byte load alignment |
| 10 | Reserved zero |
| 11 | CRC32 of the preceding 88 bytes |

Every complete range must be below 4 GiB and inside the original boot image;
package, metadata and destination must be mutually disjoint and exclude the
UFS arena and original FDT. CRC proves transport integrity, not ownership or
authenticity. The separate private compiled anchor still pins package/SQE/ZAP.
Without the new macro, the existing 64-byte v1 layout remains unchanged.

## Native policy and lifetime

The native loader revalidates the v2 record, original live FDT, original image
ownership, pinned package hashes, complete strict relocation plan and entry
before any PAS mapping. It maps only the new destination and separate metadata
as whole Normal-NC CPU granules, verifies each PA and actual PTE/cache proof,
and retains them on uncertainty. It does not map the original GPU reserved PA.

The appended kernel-private `owns_relocated_pas13` callback must return exactly
1 for actual held PAS13 request exclusion, security references, reset/source/
clock lifetime and memory-loan ownership over this exact plan and original
image. It must not assert a future EL3/XPU grant or authenticate firmware;
those outcomes follow only from actual PAS setup/authentication replies.
Zero, negative, a missing callback, a DT node,
signed relocation bit, format pass or supported-query result cannot substitute
for that authority. Existing SCM discovery, clock, transport guard, counter
and consumer-retirement proofs remain mandatory. No default policy is supplied.

PAS order and continuation/busy/error handling remain in the existing actual
`a619_pas13.c` implementation: initialize original signed metadata, register
the real chosen PA/span, copy unchanged load bytes, then authenticate/reset.
An uncertain SMC retains all buffers and security/source resources. No failed
authentication or ordinary object release authorizes freeing/reusing firmware.
Original image storage remains boot-held even after successful retirement.

## Local verification and limits

`test_relocation.c` exercises strict entry/relocatable/alignment/overflow checks,
all 64 possible 16 KiB runtime offsets within a 1 MiB alignment interval,
whole-object containment, CRC/version/range/overlap refusals and optional
read-only inspection of the private full ELF. Synthetic fixture providers are
never linked into a device image. Existing independent PAS fixtures exercise
failed/partial SCM publication, busy/continuation and retained ownership paths.
These host checks and local object builds do not establish actual PAS13/XPU
acceptance or native graphics readiness.
