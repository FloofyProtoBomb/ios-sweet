# Project Metal shaders on Android Vulkan

The DE2118 physically passed this test on 2026-11-05 while running LineageOS,
as UID 2000. This validates the selected project metallib's translated compute
and render shaders on the A619 using the existing Qualcomm Android Vulkan
driver. It is not execution of the offline Mesa/Turnip IR3 binary, a native
XNU GPU driver, Metal.framework or the iOS Metal acceptance executable.

Observed results:

- All 257 compute values matched bit-for-bit. Both the input and output had 255
  surrounding guard words checked unchanged. Data passed through GPU buffer
  copies and device-local storage buffers; the input/output were not CPU-mapped.
- All 4096 pixels of the 64x64 offscreen BGRA target matched the project pattern.
- Four GPU timestamp queries were available, with 48 valid bits and a
  52.0833321 ns period. The two complete command batches measured 986 and 1146
  ticks (about 51.35 and 59.69 microseconds, including their copies/barriers).
- The driver reported Adreno (TM) 619, vendor `0x5143`, device `0x06010900`,
  Vulkan 1.1.128 and subgroup 64. Device-local allocations also exposed host
  visibility/coherence/cache flags 15 on this unified-memory implementation;
  that is not proof of a native Metal private-buffer implementation.
- The helper/result were removed. A separate postcheck confirmed UID 2000,
  matching private phone identity, completed Android boot and running Lineage.
  There were no root, reboot, fastboot, installation or partition operations.

The executed ARM64 PIE was 38,248 bytes, SHA256
`09726d9d854fcad2ac7c7d39d944a4749bc75e515692a8c93f7a1fab8d5f1318`.
This is one selected project workload, not conformance for arbitrary Metal
programs or texture/synchronization features. No generated executable, SDK,
metallib, SPIR-V or private device dump is stored
in this directory. Only project-owned source and reproducible tooling belong
in git; actual reports remain in private working/output artifact locations.

## Shader provenance and qualification

The input is the project's actual iOS 16 target metallib, SHA256
`ec8abcbebb371a09d3b5b90275924571f7e753bb59fb580c8de3d12ac44273f2`,
translated by the pinned real metal2vulkan implementation. The managed
translation reports and frozen input are hash-checked against the later
offline-A619 reports. The project's current MSL bytes also matched the CI
artifact source. The test does not compile a hand-written GLSL replacement.

The original translated outputs are SPIR-V 1.5. This phone advertises
Vulkan 1.1 plus `VK_KHR_spirv_1_4` and `VK_KHR_shader_float_controls`.
`qualify_shaders.py` disassembles and **reassembles** each selected shader
using Khronos tools for `vulkan1.1spv1.4`, then runs the corresponding
`spirv-val` target. It requires only the `Shader` capability and standard
GLSL450 instructions, unchanged complete instruction words, identical
canonical disassembly and unchanged ID bounds/schema. This is not an
unvalidated binary-header patch or a replacement of unsupported features.
The distinct qualified 1.4 hashes and original 1.5 hashes are retained in the
report and embedded into the executable.

## Reproducing the build

Use Linux/WSL. The Linux NDK needs a case-sensitive filesystem; NTFS cannot
preserve every pair of differently cased kernel header names. The official
NDK r30 archive is pinned by its published size/SHA1 and our recorded SHA256;
only its LLVM/sysroot portion is extracted. Source: [Android NDK downloads](https://developer.android.com/ndk/downloads).

```sh
python3 fetch_ndk.py --download /private/android-probe \
  --extract /private/case-sensitive-ndk

python3 qualify_shaders.py --results /private/metal-compiler-results \
  --out /private/new-qualified

python3 build.py \
  --ndk /private/case-sensitive-ndk/android-ndk-r30 \
  --ndk-verification /private/android-probe/ndk-verified.json \
  --qualified /private/new-qualified --out /private/new-build
```

The build validates the exact generated header against every qualified
shader and report hash, uses Android API 24, and verifies AArch64 little-endian
PIE output and system-only Vulkan/libc dependencies. It uses no iOS SDK to
build the Android helper.

## Authorized device execution

`run_android.py` defaults to read-only identity/UID checks and a local plan.
It takes the serial selector from a private identity record and never prints
it or raw ADB errors. `--run` is a separate explicit invocation after reviewing
the binary and shader qualification. It accepts only UID 2000 and the exact
private phone identity. It has no root/reboot/fastboot/raw-MMIO implementation.

```sh
python run_android.py --adb /path/to/adb --identity /private/de2118.json \
  --build /private/new-build --out /private/new-plan

# Only within the separately authorized hardware-diagnostic scope:
python run_android.py --adb /path/to/adb --identity /private/de2118.json \
  --build /private/new-build --out /private/new-run --run
```

Execution writes the SHA-checked helper only under a fresh generated
`/data/local/tmp/dre-shader-*` directory. The C program rejects a CPU/software
device and requires the measured Adreno ID/extensions/queue/counter support.
Compute uses the real translated offset-aware shader with 64-thread groups,
not a CPU fallback. Render uses its real translated vertex/fragment stages
with the exact BGRA pattern. Allocation requests are bounded, waits have
five-second deadlines, and the process has a 30-second alarm. On failed waits
it reports failure and exits rather than freeing in-flight Vulkan objects or
waiting indefinitely. Shader compilation/execution still uses the existing
Android vendor stack and its power/firmware/scheduling/cache implementation.

Reports retain exact binary/shader/provenance hashes, Vulkan return/stage
errors, first value/pixel mismatch, actual counters and completed checks.
The harness verifies UID/identity and running Lineage afterward, preserves
reports on failure and only removes the two exact generated files; it never
performs recursive deletion. An unexpected UID/identity change stops further
device writes. Private identifiers are not queried from Vulkan or included
in its report. `test_guards.py` exercises these side-effect/privacy guards
using synthetic ADB responses and never contacts a phone.

The first full iOS phone boot remains a separate held milestone. This result
does not resolve native power/clock/SCM/SMMU/BO/scheduling/cache/IOSurface or
Metal runtime integration and cannot publish a working native `MTLDevice`.
