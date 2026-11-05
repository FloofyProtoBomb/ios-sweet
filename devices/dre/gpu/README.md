# A619-Holi GPU and Metal work

**Native GPU acceleration and Metal are not implemented.** The Android KGSL
test passed on the DE2118 while LineageOS supplied all GPU services. XNU has
only identified the A619 and observed its power domains off. Neither result
establishes native queue execution or an iOS graphics API.

A later Android Vulkan test executed actual project metallib-derived shaders:
257 compute results, 255 input/255 output guard words and 4,096 offscreen BGRA
pixels all matched, with positive hardware GPU timestamp intervals. Qualified
SPIR-V 1.4 inputs preserved the original translated instruction words. This
is physical compiler/shader evidence using LineageOS's driver; it does not
execute the offline Mesa IR3 binaries or prove native XNU/Metal support.

## Executable backend pieces

`a619_backend.c` implements a restricted initialized-ring submission path.
It validates the chip, all five prerequisite assertions, owned GPU address
ranges, hardware ring base/size and idle pointers before publishing work.
Its only MMIO write is `CP_RB_WPTR`. The stream writes one marker and waits
for memory writes and idle. Completion requires both the read pointer and
uncached result memory to change correctly. Submission/ownership/timeout
errors poison the queue: retain every mapped page until a hardware reset.
This is not a general command parser or userspace-accessible interface.

The caller supplies `dre_a619_queue`, serializes access and implements
`dre_a619_ops` with the actual kernel MMIO mappings, ownership/fault guard,
barriers and monotonic timer. Do not set `ready_mask` from DT identification.
The bits represent verified power/clocks, initialized SQE/CP, authenticated
ZAP/nonsecure transition, an owned SMMU domain and uncached CPU mappings.
None of those platform providers are supplied by this queue module.

`dre_a619_sqe_view()` validates a firmware layout and excludes the initial
four bytes exactly as the matching KGSL loader does. Before code is loaded,
its private expected digest must also match. The function does not authenticate
the ZAP firmware or prove that SQE is running. Firmware stays outside git.

`a619_holi_power.c` implements the Holi-specific SPTPRAC on/off register
sequence after the platform has acquired regulator corners, source rates,
interconnect votes and exclusive quiescent ownership. It checks the live
CX/GX domains and branch clocks, bounds every transition to 10 ms, and
recovers failed enablement with the published retention/power-off sequence.
Release restores only the retention bit this transaction changed. Lost
ownership or failed recovery requires reset while retaining all resources.
An inherited already-on block is borrowed without changing its power state.
The module does not enable voltage rails or program PLLs, an SMMU or SCM.

`a619_zap.c` validates full ELF32 Hexagon ZAP images, builds the exact
header-plus-hash PAS metadata, and bounds relocated segment copies to the
owned reserved microcode range. It rejects overlapping segments, truncated
files, unsupported layouts, wrong alignment and overlapping CPU buffers.
The private DE2118 firmware produces metadata byte-for-byte equal to its
private MDT. The firmware and metadata bytes remain outside the repository.

`a619_pas13.c` executes the legitimate supported/init-image/memory-setup/
auth-and-reset sequence for peripheral 13 only. It uses the parser above and
provides shutdown/recovery. Standard SMC64 register arguments, Qualcomm A6
continuations, busy backoff, error propagation and exclusive ownership are
checked. Its optional EL1 `a619_smc64.c` transport preserves the continuation
register; it is never invoked by the host tests. The platform must first prove
SMC64 discovery, SCM clocks, the exact reserved region, private firmware hash,
contiguous non-cacheable mappings and exclusive PAS/GPU ownership. Unknown
completion or failed shutdown requires reset with memory retained. Retry
loops are bounded, but secure firmware can itself hang inside an SMC. No
secure authentication bypass is implemented.

`a619_smmu.c` constructs a restricted three-level 39-bit/4KiB domain and
programs one exclusively owned, disabled nonsecure context bank for SID0.
It verifies live capabilities, all stream matches, CB sharing, idle/fault
state and the pinned Holi global implementation configuration before writes.
Page-table RAM and every mapped page need an actual ownership callback.
Mappings support RO executable firmware, RW non-executable data and guard
holes; physical aliases, writable executable pages and table self-mapping
are rejected. TCR/TTBR/MAIR/ACTLR writes, readback, context TLB retirement
and rollback are implemented. A faulting stream is established before any
translation disablement, avoiding an intermediate physical bypass path.

The SMMU module never changes global CR0, the global implementation registers,
other SMRs/CBs, or secure SID2. The platform must supply CX power/memnoc clocks,
exclusive quiescent GPU ownership, ASID/CB reservations, Normal-NC table RAM
and a legitimate initialized global state. It refuses unknown global state
rather than resetting secure mappings. The default bootstrap window is 2MiB;
the optional owned VM manager below supports larger explicit windows. This
is not yet a per-process graphics memory manager. Unsafe
failure retains all pages until reset; release requires the GPU to remain
quiescent through power-down or reassignment.

Run the production C against the synthetic device and compile both ARM64
formats:

```sh
python devices/dre/gpu/test_backend.py --zig /path/to/zig --out /tmp/a619-tests
# Optional private input; only metadata is printed, nothing is copied.
python devices/dre/gpu/test_backend.py --zig /path/to/zig --out /tmp/a619-tests \
  --firmware /private/a630_sqe.fw --firmware-manifest /private/manifest.json \
  --zap /private/a615_zap.elf --zap-mdt /private/a615_zap.mdt
```

This covers 56 queue/firmware cases and 16,384 independently checked packet
parities. It includes missing prerequisites, ownership changes, ring mismatch,
stale data, pointer-only/data-only completion, timeout and a stopped timer.
There are also 38 Holi power/ownership/recovery, 46 ZAP parser/relocation,
35 PAS-13 ABI/authentication/recovery, 74 SMMUv2 activation/table-walk/
secure-isolation/rollback, 70 CP initialization, 601 RBBM and 76 VM checks:
996 production-code cases in total. These are synthetic hardware tests,
not native handset execution. The separate RPM platform transport has its
own 6,806 synthetic tests described below.

The dedicated `dre-metal-probe` CI also runs these existing portable C
fixtures with the host compiler via `tools/check-dre-gpu-host.py`, compiles
native helpers against the official iPhoneOS SDK and builds the frontend
translation units. Reports keep native execution and acceleration false.
The [official SDK run](https://github.com/FloofyProtoBomb/ios-sweet/actions/runs/41987783607)
passed those checks and linked the read-only factory observer. The frontend
is an object build; its complete Rust/Turnip link and native runtime are
still unfinished.

## IOSurface runtime fixes

[IOSURFACE_ABI.md](IOSURFACE_ABI.md) records the selected iOS framework ABI,
the real CPU allocation/mapping implementation, and the incremental kernel
lifetime/use-count patch with production-method concurrency tests. This is
separate from A619 GPU mapping, synchronization and Metal support.

[UI_BOOT_DIAGNOSIS.md](UI_BOOT_DIAGNOSIS.md) records the actual CPU-frame boot
evidence and LaunchServices dependency observed in frozen QEMU memory. The
included configuration-derived, read-only saved-stack inspector verifies
capture hashes before inspection; it does not attach to or modify the phone.

## Native CP/SQE initialization slice

[NATIVE_CP.md](NATIVE_CP.md) describes `dre_a619_cp_start`, its real Holi
register-list/SQE/ring initialization, authenticated transition and hardware
marker requirement. Seventy synthetic cases plus an optional private-firmware
upload case pass; the CP source also compiles as freestanding ARM64 ELF/Mach-O.
Physical platform providers and Metal remain missing. No kernel registration
or phone operation is enabled by adding the helper.

[NATIVE_RBBM.md](NATIVE_RBBM.md) describes the separate static Holi RBBM
provider: 144 real ordered/register-readback writes and 601 synthetic failure/
profile/order cases. It still needs actual acquired rail/clock/bus/SPTPRAC
providers before any physical MMIO access, and is not kernel-registered here.
The legacy SMMU's 2 MiB default is a bootstrap limit. The optional
[BO mapping manager](NATIVE_VM.md) adds explicitly owned larger windows,
generation/lease guards and actual ASID/TLB retirement. Its 76 synthetic cases
include two actual-stride iOS framebuffer BOs and a densely mapped 256 MiB
window; this does not establish full native Metal resource capacity.

## Metal acceptance executable

The [offline compiler path](compiler/README.md) has translated actual
project-owned Metal AIR/metallib into independently validated SPIR-V, then
compiled compute and render pipelines to A619 IR3 through pinned Mesa Turnip.
This proves offline compilation, without a device submission. The generated
SPIR-V 1.5 requires a qualified runtime; the existing Android vendor driver
advertises core Vulkan 1.1 and cannot simply receive those binaries unchanged.

The [Metal306 runtime work](runtime/README.md) qualifies the actual 20H392
factory, `_MTLDevice` layout and compiler envelopes. Its adapted public
frontend reference still needs the native Darwin GPU transport, truthful
feature limits, ownership, synchronization and presentation. No device or
plugin is published by the observer or compiler helpers.

The [RPM platform transport](platform/README.md) implements actual bounded
GLINK/FIFO/IPCC requests with matching RPM ACKs and a hold-only policy. Host
protocol tests do not prove physical firmware acceptance or a qualified
shared-rail voltage and clock plan. Kernel IRQ/reset/resource ownership and
the remaining platform acquisition hooks are still needed.

`metal_probe.m` calls Apple's real Metal API. `build_metal_probe.sh OUT`
requires macOS Xcode with the iPhoneOS SDK and builds an arm64 iOS 16
executable, an offline AIR/metallib and the matching project-owned MSL source.
This is an acceptance program, not a replacement Metal.framework.

On the target iOS userspace:

```sh
dre-metal-probe dre-metal-probe.metallib dre-metal-probe.metal report.json
```

The program requires a real `MTLDevice`, the native A619 chip/counters below,
an offline metallib and runtime MSL compilation, private-buffer compute and
blit with 257 exact results, an offscreen draw with 4,096 checked pixels,
and a CoreVideo IOSurface-backed render with the same pixel checks. It uses
public CoreVideo allocation/Metal texture-cache APIs and verifies an IOSurface
is actually present; private IOSurface headers are not required. Each command
buffer has a five-second completion deadline. Missing APIs, nil objects,
failed compilation, mismatched results or missing native completion progress
produce a nonzero exit with the failing stage. No CPU-rendered fallback is
allowed to claim a pass.

The eventual native driver must expose read-only sysctls:

| Name | Type | Meaning |
| --- | --- | --- |
| `kern.dre_gpu_chip_id` | uint32 | Hardware-validated A619-Holi identity, `0x06010900` |
| `kern.dre_gpu_submitted` | uint64 | Monotonic native GPU job publications |
| `kern.dre_gpu_completed` | uint64 | Monotonic hardware-confirmed retirements |

Those providers do not exist yet. The test requires at least four new
submissions and retirements, and should run in an isolated bring-up session
with no other graphics client. Its workload is a first integration gate,
not complete Metal conformance or proof that every iOS app is supported.
Compiling it on a Mac does not establish Metal acceleration on the N200.

## Remaining initialization and API dependencies

The pinned Holi driver uses `a6xx_holi_init` and a GMU wrapper. Do not replace
it with the generic A6xx GMU/HFI initialization sequence. The cold-start
path needs RPM-SMD parent/voltage votes, CX/GX domains, GPUCC source rates,
clocks/interconnect, SPTPRAC, a private GPU SMMUv2 domain at `0x05940000`,
SQE code, ring/CP configuration and authenticated PAS-13 `a615_zap` loading.
The PAS metadata buffer must be contiguous, 4K aligned and non-cacheable;
the matching SCM implementation also enables its own clocks.

The iOS 20H392 userspace contains Metal/MTLCompiler 306.6.6, IOGPU 65.60.1
and AGXMetalA11 227.6.5. An Adreno backend needs a compatible device/resource/
queue interface plus MSL/AIR/metallib compilation/translation, synchronization,
textures and IOSurface presentation. A powered GPU and working marker queue
do not provide these APIs. MoltenVK translates Vulkan to Metal, the opposite
direction. MoltenMTL is a source-level Swift/Vulkan experiment that consumes
SPIR-V rather than existing iOS MSL/AIR/metallib assets; it is not a drop-in
binary-compatible iOS driver.

Primary references:

- [Pinned Holi device tree](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/holi-gpu.dtsi)
- [Pinned A6xx driver](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_a6xx.c)
- [Pinned Qualcomm MDT metadata loader](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/soc/qcom/mdt_loader.c)
- [Pinned Holi SMMUv2 bindings](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/msm-arm-smmu-holi.dtsi)
- [Pinned ARM SMMU implementation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/iommu/arm-smmu.c)
- [Pinned Qualcomm SCM transport](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/firmware/qcom_scm-smc.c)
- [Pinned PM4 packet definitions](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_pm4types.h)
- [Apple Metal compute workflow](https://developer.apple.com/documentation/metal/performing-calculations-on-a-gpu)
- [Mesa Freedreno/Turnip](https://docs.mesa3d.org/drivers/freedreno.html)
- [MoltenMTL scope and limitations](https://github.com/Nishad-Sharma/MoltenMTL)
