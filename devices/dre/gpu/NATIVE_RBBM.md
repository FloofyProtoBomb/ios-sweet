# Restricted Holi static RBBM provider

`dre_a619_rbbm_initialize` programs the actual static GPU register state
required before `dre_a619_cp_start`. This is new source and host verification;
it has not run on the N200 or been registered as a kernel driver.

The selected profile is the pinned A619-Holi DT/core definition: chip
`0x06010900`, highest bank bit 14, 512 KiB GMEM, 32-byte minimum memory access
and UBWC 2. Other profiles are refused. The caller must first acquire the real
voltage/rail, GCC/GPUCC source-rate/branch/bus and SPTPRAC providers, hold
exclusive reset-device ownership, and select the non-LLCC SMMUv2 path. A
device-tree node or an old active-state bit is not sufficient. The callback
must also invalidate readiness after a reset, ownership loss or fault.

The helper requires an idle, halted CP with zero ring pointers, no pending
IRQ/hardware fault and GPU interrupts masked. It does not install an IRQ
handler or enable interrupts. IFPC/preemption and secure application buffers
are outside this restricted single-ring profile.

It then performs 144 ordered, read-back-verified device stores:

- Nonsecure block address modes, UCHE trap/write-through/GMEM ranges,
  filter/cache settings, A619 ROQ/memory pool, primitive FIFO, AHB error
  response, performance counter and pinned bank/UBWC configuration.
- The Holi wrapper's busy mask and power-counter selection, plus all 32
  published protected-register spans with access-fault protection enabled.
- The SMMUv2 non-LLCC cache allocation override. No SMMU table, context,
  stream, global policy or secure SID2 register is touched.
- All 63 published a615-family per-block HWCG settings, using the Holi SP
  clock gate/restore sequence, UCHE/GBIF L2 control and the A619 top-level
  clock-gating value. These are internal clock-gating settings, not PLL
  programming, voltage requests or a replacement GPUCC clock provider.
- The documented GBIF/Holi bus-halt clears, followed by another idle/fault
  and ownership check.

The source deliberately leaves secure-buffer base/size/address-mode and trust
registers untouched. The later CP helper uses authenticated PAS-13 and the
documented secure-mode packet; there is no direct trust-register bypass.
This RBBM helper does not publish a ring WPTR or launch firmware or shaders.

After the first attempted store, failed MMIO, a mismatched readback or lost
ownership marks the state unsafe. Preserve all resources and perform an
actual hardware reset before another initialization. No stale register image
is replayed as rollback. `active` means this serialized static programming
completed with readbacks; it does not mean that clocks/rails can subsequently
be released, mappings resized, or a GPU job has executed.

The caller's CP `platform_ready` callback may use the resulting still-owned
static state together with the actual held platform/domain providers. It
must not reapply this initializer's *cold CP* precondition while CP is running.

`test_backend.py` includes 601 synthetic RBBM cases. The test checks literal
settings/order, preserved RMW bits and untouched secure/IRQ/DMA registers,
then injects a failed store, corrupted immediate readback and ownership loss
at every one of the 144 store positions. Every uncertain-store case fails
closed. GCC ASan/UBSan and freestanding ARM64 ELF/Mach-O builds also pass.
None of these tests establishes physical GPU operation or Metal support.

The remaining bring-up dependencies include real power/clock/vote providers,
active owned SMMU translations and trusted private firmware/SCM allocations.
The legacy three-page SID0 map defaults to a 2 MiB bootstrap window; the new
[BO mapping manager](NATIVE_VM.md) adds explicitly allocated larger windows
and controlled TLB retirement. The observed 1125x2436 BGRA surface uses a
4608-byte row stride (about 10.7 MiB), beyond the legacy bootstrap window.
Production Metal still needs its actual allocator/lifetime/cache integration,
shader execution, synchronization/presentation and userland/compiler API.

Register/value facts and ordering were checked against the pinned
[Holi DT](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/holi-gpu.dtsi),
[A6xx implementation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_a6xx.c),
[core/register tables](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno-gpulist.h),
[register definitions](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/a6xx_reg.h)
and [GMEM-base selection](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno.c).
