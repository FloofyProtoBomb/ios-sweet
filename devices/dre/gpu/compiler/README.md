# Metal bytecode to A619 compiler work

This directory contains a bounded, executed **AIR → SPIR-V → NIR → A619 IR3**
offline compiler path. It consumes real metallib/AIR bytecode. It does not
substitute the original MSL source or advertise a Metal device.

On 2026-11-05 the actual CI-built `dre-metal-probe.metallib` passed the following
host checks with the pinned sources in [sources.json](sources.json):

| Pipeline | Actual AIR decoding | Independent SPIR-V validation | Turnip A619 IR3 compilation | GPU execution |
| --- | --- | --- | --- | --- |
| `dre_transform` compute | PASS | PASS, Vulkan 1.2 | PASS | Unproven |
| `dre_vertex` + `dre_fragment` render | PASS | PASS, both stages | PASS | Unproven |

The input is the project's own probe, compiled for `air64-apple-ios16.0.0` /
MSL 2.3. Its metallib has container version 2.7 and three functions. This does
not establish compatibility with every library shipped in iOS 16.7.16 Metal
306, compiler correctness on a GPU, or the iOS Metal runtime ABI.

The host compiler target is Mesa's static A619 profile, chip ID `0x06010900`.
The no-op DRM fixture has 512 KiB GMEM, matching the pinned A619-Holi definition
in [Lineage's source](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno-gpulist.h).
It is a compiler fixture, with no physical GPU attached. No command buffer,
dispatch, draw, queue submission, render-pass execution, or hardware success is
produced by this path.

## Components and bounds

- `metal_input.py` checks fixed container ranges, versions and byte budgets.
  LLVM bitcode/wrappers and executable iOS 16.0–16.7 metallib v2.0–2.7 are
  admitted to the real decoder. It intentionally does not claim to verify LLVM
  instructions, per-function hashes, or Metal semantics.
- `translate_offline.py` uses a caller-pinned binary built from the selected
  metal2vulkan commit, freezes the input/profile, invokes the real decoder and
  translator, and requires independent `spirv-val --target-env vulkan1.2` on
  every output. A deliberately corrupted function HASH in the real private
  metallib was refused by that decoder, with no successful output report.
- `compile_a619_offline.py` verifies the translation report, SPIR-V hashes and
  reflection before creating a pipeline through actual Mesa Turnip. A reflected
  compute layout can have up to 32 sequential storage-buffer bindings in set 0
  and one bounded push range. Compute specialization uses the defaults encoded
  in SPIR-V; no dispatch size or execution result is selected here. The render
  path accepts only procedural vertices, no resources/push constants,
  triangle-list topology, one unblended BGRA8 color target, and the exact
  baseline graphics contract in that file. Unsupported state fails closed.
- `offline_ir3.c` creates only host compiler objects. It retrieves actual NIR
  and IR3 text through `VK_KHR_pipeline_executable_properties`; no raw binary is
  exported. The shader/pipeline objects are then destroyed. The wrapper selects
  the isolated no-op ICD; the C fixture also checks its environment, device
  name and chip ID.

Each external worker has a 20-second total deadline, 500 MiB address-space
limit, CPU limit, no core dumps, and a private process group. The supervisor
terminates its own group after completion or timeout. Shader input is at most
64 MiB; profile and metadata are at most 4 MiB; each shader/IR representation
is at most 16 MiB. Output directories must be new and are never overwritten.
Mesa's no-op frontend `ftruncate`s a **sparse 4 GiB memfd aperture**; that worker
therefore receives a 4 GiB logical file limit while retaining the 500 MiB
address-space limit. This aperture is not resident GPU or system memory.

The supplied Vulkan 1.2 floor profile is a declared compile contract. It has
not been queried from a DE2118. Real device capabilities, runtime shader
specialization, descriptor mapping and synchronization must be qualified by
the native driver/runtime before this compiler can service Metal applications.
The separate read-only Android reference reports vendor Vulkan 1.1.128,
`VK_KHR_spirv_1_4`, and subgroup size 64. These offline outputs use SPIR-V 1.5;
`spirv-val --target-env vulkan1.1spv1.4` correctly refuses them. The reference
therefore does not qualify this profile or these shaders for that Android
driver. A new native Turnip backend may expose a different capability set,
which remains unmeasured.

## Reproduce on a Linux build host

Keep Apple firmware, `.air`, `.metallib`, translated shaders and their text IR
outside the repository. No Apple input is fetched or uploaded by these tools.
The following paths are placeholders for private working directories.

Build the pinned translator with its repository toolchain and retain the
generated lockfile, toolchain version and binary SHA256 as build provenance:

```sh
git clone https://github.com/steelbrain/metal2vulkan "$BUILD/metal2vulkan"
git -C "$BUILD/metal2vulkan" checkout 219f01365dc4c8eb346816c405b5b17403793764
cd "$BUILD/metal2vulkan"
cargo build --release -p metal2vulkan --features cli
sha256sum target/release/metal2vulkan Cargo.lock
```

The exercised build used Rust 1.98.1. It produced translator SHA256
`13fb5a11fa561229c82d4d94cce7b35d3536577fdbe99f886787a8aec9ac7016` and
lockfile SHA256 `ff61db7630d931e9636730c1f74c2e51e1009ea8587bb2e69691874ee59a5c4a`.
A different binary must be pinned to its own build evidence. Install
SPIRV-Tools separately; the exercised validator was Ubuntu's 2025.1 package.

Translate the actual bytecode, first compute and then render:

```sh
python3 "$REPO/devices/dre/gpu/compiler/translate_offline.py" \
  --input "$PRIVATE/dre-metal-probe.metallib" \
  --profile "$BUILD/metal2vulkan/crates/metal2vulkan-validation/profiles/vk12-floor.json" \
  --translator "$BUILD/metal2vulkan/target/release/metal2vulkan" \
  --translator-sha256 "$TRANSLATOR_SHA256" --spirv-val /usr/bin/spirv-val \
  --stage compute --function dre_transform --out "$PRIVATE/translated-compute"
python3 "$REPO/devices/dre/gpu/compiler/translate_offline.py" \
  --input "$PRIVATE/dre-metal-probe.metallib" \
  --profile "$BUILD/metal2vulkan/crates/metal2vulkan-validation/profiles/vk12-floor.json" \
  --translator "$BUILD/metal2vulkan/target/release/metal2vulkan" \
  --translator-sha256 "$TRANSLATOR_SHA256" --spirv-val /usr/bin/spirv-val \
  --stage render --function dre_vertex --fragment dre_fragment \
  --color-format 0:BGRA8Unorm --out "$PRIVATE/translated-render"
```

Fetch Mesa's official `mesa-26.2.6.tar.xz` and verify SHA256
`b9436b3e1a8be76fbd4823dc884f90596ab7c6ac5e1867a305e66ffccfb31a92` against
the [official release notes](https://docs.mesa3d.org/relnotes/26.2.6.html).
Unpack it into `$BUILD/mesa-26.2.6`. Install Mesa's documented build dependencies
and the Vulkan development headers/loader. The exercised build used Meson
1.9.2, Ninja, GCC and Ubuntu 24.04. The small patch below adds only the A619
target to the no-op compiler fixture; it is not a native device driver patch.

```sh
patch -d "$BUILD/mesa-26.2.6" -p1 < "$REPO/devices/dre/gpu/compiler/mesa-26.2.6-offline-a619.patch"
meson setup "$BUILD/mesa-build" "$BUILD/mesa-26.2.6" \
  -Dbuildtype=release -Dplatforms= -Dgallium-drivers= \
  -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm -Dtools=drm-shim \
  -Dllvm=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dvideo-codecs= \
  -Dbuild-tests=false
ninja -C "$BUILD/mesa-build" -j4 \
  src/freedreno/vulkan/libvulkan_freedreno.so \
  src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so
cc -std=c11 -O2 -Wall -Wextra -Werror \
  "$REPO/devices/dre/gpu/compiler/offline_ir3.c" \
  -lvulkan -o "$BUILD/offline_ir3"
python3 "$REPO/devices/dre/gpu/compiler/compile_a619_offline.py" \
  --translated "$PRIVATE/translated-compute" --mesa-build "$BUILD/mesa-build" \
  --fixture "$BUILD/offline_ir3" --out "$PRIVATE/a619-compute"
python3 "$REPO/devices/dre/gpu/compiler/compile_a619_offline.py" \
  --translated "$PRIVATE/translated-render" --mesa-build "$BUILD/mesa-build" \
  --fixture "$BUILD/offline_ir3" --out "$PRIVATE/a619-render"
python3 -m unittest discover -s "$REPO/devices/dre/gpu/compiler" -p 'test_*.py' -v
```

The tests use owned synthetic envelopes and subprocesses, not Apple shaders.
They cover malformed ranges/versions/counts, output pinning and preservation,
SPIR-V bounds, unsupported descriptor/render contracts, and real worker
timeouts/memory limits. The separate private integration runs exercise the
actual metallib decoder, validator and Turnip compiler.

## Remaining integration

The next compiler integration needs a real iOS-callable service/library with
the runtime's Metal function constants, binding reflection, pipeline/cache
keys and error propagation. It also needs a measured A619 capability profile,
iOS-compatible Turnip/kernel transport, memory residency, fences and native
GPU submissions. The Objective-C Metal/IOGPU ABI and native GPU power/CP/MMU
bring-up are separate work streams. Actual compute/readback and render/readback
through an authentic `MTLDevice`, with hardware counters and IOSurface
presentation, are still required. Offline compilation cannot certify Metal.

`metal2vulkan` supplies real AIR decoding and SPIR-V translation under
`GPL-2.0-or-later OR LGPL-3.0-or-later`; it is a separate pinned dependency,
not relicensed or copied into these MIT wrappers. Keep its source, notices,
license choice and required redistribution/relinking materials with any future
runtime integration. Mesa's selected compiler/no-op sources are MIT; the Mesa
tree also contains components under other licenses. AirNir was reviewed as an
alternative, but its pinned strict straight-line vertex/fragment frontend was
not used for the executed compute/render path.

Primary references: [metal2vulkan](https://github.com/steelbrain/metal2vulkan/tree/219f01365dc4c8eb346816c405b5b17403793764),
[Turnip architecture](https://docs.mesa3d.org/drivers/freedreno.html),
[MetalLibraryArchive format research](https://github.com/YuAo/MetalLibraryArchive),
[AirNir](https://github.com/UniXNU/AirNir/tree/b356371a74cd158c2902f4886f373ebb0f833a36).
