# A619-Holi GPU and Metal work

**Native GPU acceleration and Metal are not implemented.** The Android KGSL
test passed on the DE2118 while LineageOS supplied all GPU services. XNU has
only identified the A619 and observed its power domains off. Neither result
establishes native queue execution or an iOS graphics API.

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

Run the production C against the synthetic device and compile both ARM64
formats:

```sh
python devices/dre/gpu/test_backend.py --zig /path/to/zig --out /tmp/a619-tests
# Optional private input; only metadata is printed, nothing is copied.
python devices/dre/gpu/test_backend.py --zig /path/to/zig --out /tmp/a619-tests \
  --firmware /private/a630_sqe.fw --firmware-manifest /private/manifest.json
```

This covers 56 queue/firmware cases and 16,384 independently checked packet
parities. It includes missing prerequisites, ownership changes, ring mismatch,
stale data, pointer-only/data-only completion, timeout and a stopped timer.
There are also 38 Holi power/ownership/recovery checks. These are synthetic
hardware tests, not native handset execution.

## Metal acceptance executable

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
and an IOSurface-backed render with the same pixel checks. Each command
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
- [Pinned PM4 packet definitions](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_pm4types.h)
- [Apple Metal compute workflow](https://developer.apple.com/documentation/metal/performing-calculations-on-a-gpu)
- [Mesa Freedreno/Turnip](https://docs.mesa3d.org/drivers/freedreno.html)
- [MoltenMTL scope and limitations](https://github.com/Nishad-Sharma/MoltenMTL)
