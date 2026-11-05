# Native A619-Holi CP/SQE initialization

`a619_cp.c` implements an actual bounded initialization sequence for the
single command-processor ring. Its separate static RBBM provider is described
in [NATIVE_RBBM.md](NATIVE_RBBM.md). The CP helper does not provide the missing
platform power/clock providers, shader execution, an iOS Metal device/runtime,
or hardware validation. A successful synthetic test is not native execution.

## Exact callable integration

Call `dre_a619_cp_start(struct dre_a619_cp *, const struct dre_a619_cp_ops *,
budget)` from a serialized, exclusive kernel bring-up path. The queue must be
fresh. Its ring/result buffers and the CP state's SQE/scratch/power-up buffers
must have distinct GPU and CPU ranges and page-aligned Normal-NC CPU mappings.
The ring is 4–64 KiB, a power of two; scratch, power-up and result are each
4 KiB. SQE storage is a whole-page allocation large enough for the firmware
with its four-byte file prefix excluded. The selected phone's private SQE
uses 32 KiB of storage after rounding; its bytes are not included here.

The callbacks have real obligations:

| Callback | Required provider |
| --- | --- |
| `io.guard` | Exclusive GPU/domain/firmware ownership, pinned unchanged mappings and no active platform/IOMMU fault. Keep the same serialized ownership throughout the call. |
| `platform_ready` | Actually acquired voltage/rail, clock-rate and bus votes; live CX/GX/SPTPRAC; static Holi RBBM/protection/cache/GBIF programming; active owned GPU SMMU domain. IFPC and preemption must remain disabled for this restricted ring. |
| `mapping` | Inspect actual GPU PTE/physical/CPU relationships, allocation ownership and CPU memory type. SQE is GPU read/execute, the other buffers read/write and non-executable. An address from DT or an unchecked capability bit is insufficient. |
| `firmware_verified` | Hash the complete original private firmware against the trusted boot-package manifest. An arbitrary caller-supplied digest is insufficient. Firmware and input bytes remain pinned/immutable during initialization. |
| `authenticate_zap` | Invoke `dre_a619_pas13_authenticate` with the real SCM transport, clocks, reserved memory and trusted private ZAP image. Return success only when the resulting authenticated state is still owned and valid. |
| `io.read32/write32/barrier/now` | Real ordered MMIO, CPU↔GPU memory ordering, and monotonic bounded timing. The same guard must remain valid through the existing marker backend. |

No new kernel staging or driver registration is part of this source change.
Do not call it merely because the GPU device-tree node was found.

## Implemented sequence

1. Validate every buffer size, GPU/CPU overlap, mapping permission and trusted
   firmware digest. Reject firmware without the documented WHERE_AM_I fix.
The original firmware's four-byte prefix is excluded and never uploaded as
   SQE code. The private input must not overlap any writable output buffer.
2. Require an already halted, idle CP with zero ring pointers, no pending IRQ
   or hardware fault, 64-bit CP addressing, register protection enabled, AHB
   error responses and deasserted Holi bus halt. The provider remains
   responsible for the full static RBBM setup; these reads are extra guards.
3. Build the exact Holi power-up list by reading the actual current registers:
   36 IFPC-only pairs and 32 restore pairs, including the intentional duplicate
   a615-family UCHE entry and fixed performance-counter restore value. The
   CPU/GPU spinlock header has zero lock fields, 136 list dwords and offset 72.
4. Copy prefix-excluded SQE to owned NC storage, zero its remaining padding,
   initialize the ring and RPTR scratch, and order all stores. Program and
   read back the RPTR address, ring size/address and 64-bit SQE address.
   Clear SQE halt with the published value 1.
5. Publish the pinned 12-dword `CP_ME_INIT` (mask `0x16f`, including its
   zero-initialized trailing ordinal), followed by `CP_WHERE_AM_I`. Require
   actual MMIO RPTR/WPTR **and** NC scratch completion, idle RBBM, no hardware
   fault or pending IRQ, and continued ownership before authenticating ZAP.
6. Invoke real PAS-13 authentication, then publish `CP_SET_SECURE_MODE(0)` and
   another WHERE_AM_I. Require the same completion checks and read back the
   nonsecure trust state. There is no direct secure-control-register fallback
   or authentication bypass.
7. Only then enable the queue backend's internal readiness and issue its
   restricted memory marker. Real RPTR advancement and exact result memory
   are both required. Recheck faults/idle/ownership before returning
   `NATIVE_CP_STARTED` (stage 6).

CP initialization stages do not increment application-job counters. The
existing marker queue increments its counters only for its real publication
and hardware-confirmed retirement. No Metal device/capability/counter sysctl
is fabricated by this helper.

An uncertain failure after the first device-register store marks the CP unsafe
and poisons the queue. All mappings, firmware, ZAP reservations and power votes
must remain held until an actual hardware reset. A failed MMIO callback might
still have reached the device. There is deliberately no teardown that frees
or restores a potentially active DMA ring. Poll loops have both a caller's
time budget and a finite iteration bound; the platform SCM implementation
must handle its separate secure-call limitations.

## Physical dependencies still missing

The next native GPU test still needs real RPM-SMD/SMEM rail and bus voting,
GCC/GPUCC source-rate/branch/domain ownership, and integration of the new Holi
static RBBM initializer. The existing SPTPRAC, PAS-13 and SID0 SMMUv2 helpers can
then be connected to real allocations/guards/transports. The SMMU helper
requires exclusive context-bank/ASID ownership and verified global SMMU state;
it does not initialize or borrow arbitrary bootloader domains. No secure SID2
mapping or firmware-authentication bypass is permitted. The legacy SMMU
default is a 2 MiB bootstrap window; [NATIVE_VM.md](NATIVE_VM.md) describes
explicitly allocated larger BO windows and controlled TLB retirement. Actual
allocator/lifetime/cache/backend integration is still required.

After native CP completion, actual shader execution, cache maintenance,
fences/scheduling/reset, GPU IOSurface sharing/presentation, and a compatible
Metal 306 device/resource/queue/compiler implementation are still required.
A marker tests command processing and memory writes, not Adreno shader ALUs
or Metal compatibility. The separate real Metal acceptance executable must
succeed on the native GPU before claiming Metal acceleration.

## Validation and sources

`test_backend.py` compiles/runs 70 CP cases plus the optional digest-verified
private SQE upload case. The independent MMIO model checks literal packet
parities, both halves of GPU addresses above 4 GiB, the entire register-list
snapshot, ordering of authentication, secure transition and marker completion,
and injection of ownership/readback/authentication/IRQ/fault/timeout failures.
It verifies that MMIO writes remain confined to the owned CP configuration
registers and WPTR. The CP tests also pass GCC ASan/UBSan; the production
source compiles freestanding ARM64 ELF and Mach-O with warnings as errors.
These checks do not establish hardware operation.

Register and protocol facts were checked against the pinned
[Holi A6xx implementation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_a6xx.c),
[A6xx declarations](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_a6xx.h),
[ring submission](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_ringbuffer.c),
[register definitions](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/a6xx_reg.h),
and [A6xx firmware checks in Linux v6.12](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/msm/adreno/a6xx_gpu.c).
