# DRE Darwin Turnip transport — UNVALIDATED draft

This is source implementation for the overnight branch. **No new test,
compile, source-preparation recipe, link, CI, QEMU session or phone operation
has run.** Earlier tested Metal306 frontend/host-Rust/compiler reports describe
their original artifacts and do not qualify these new sources. No native
Vulkan physical device, Metal acceleration, GPU completion or driver readiness
is asserted.

The source replaces the pinned Darwin recipe's A740 `NativeAdrenoABI` transport
with the project's actual, versioned `gpu/kernel/dre_gpu_uapi.h`. It does not
emulate Linux DRM, publish an AGX service, probe sandbox selectors or override
`MTLCreateSystemDefaultDevice`.

## Native ownership and transport

`dre_darwin.c` opens only `DREAdrenoGPU`, type0, through real IOKit calls. Typed
replies require the exact structure size, version, connection generation and
reserved-field contract. A Mach transport error or malformed/changed ownership
reply poisons new work. Cleanup and observation of already known native objects
remain possible. An absent provider or missing capability is an explicit
failure, not a software device fallback.

BO creation accepts a byte count and access flags; the kernel allocates, pins
and returns an opaque64 handle and actual GPU VA. No physical address or client
IOVA is accepted. The transport checks both native bounds and local handle/VA
collisions. CPUMap returns an actual task address and an opaque kernel-owned
IOMemoryMap lease; it is **not** an IOConnectMapMemory token. CPUUnmap revokes that
mapping. Failed or uncertain unmapping never releases the allocation for reuse.

ABI1 currently offers exclusive Normal-NC leases. It refuses submissions with
an active exclusive CPU lease. Turnip keeps CPU pointers across submissions, so
the adapter additionally requires `PERSISTENT_CPU_MAP` and the explicitly
reserved persistent cache contract. Neither is supplied by the current kernel
draft. No automatic revocation leaves stale pointers in Mesa.

The later source-only access-epoch extension appends selectors10..12 without
changing ABI1 selectors0..9. Mapping lifetime and explicit CPU access epochs
are tracked separately in the transport; partial-error epochs are retained.
Kernel pointer/CPU transition entrypoints and a private GPU ownership state
machine are authored, but persistent capability remains masked and mapped BOs
still cannot enter raw submission. Generic partial cache calls on a persistent
map fail instead of pretending an ownership transfer happened. See
[PERSISTENT_ACCESS.md](../kernel/PERSISTENT_ACCESS.md) for the precise unvalidated
contract and remaining native/Vulkan synchronization integration.

The subsequent source-only IOSurface extension appends selectors13..16. A real
Mach send right resolves the selected SweetSurface object in the owning task;
the kernel retains its actual descriptor/table user and inventories prepared
pages privately. The transport can retain/describe/close this backing through
opaque connection tokens, including partial-error cleanup. `dre_darwin_open(0)`
opens only that control connection, without requiring or inventing GPU
capabilities/geometry. Vulkan enumeration still requires every native GPU bit.

`dre_surface_iosurface.c` is an optional **unbuilt** IOSurface framework wrapper
that creates/deallocates its own real Mach send right. It never reads a pixel
base address. It is not yet linked into Turnip or the Metal frontend. The real
import-attempt selector currently returns Unsupported with zero BO/VA because
live default/WB aliases, cache ownership, task-VM import and cross-process event
synchronization are unresolved. See [SURFACE_BACKING.md](../kernel/SURFACE_BACKING.md)
for source ownership, bounds, lifetime and refusal details. No host pointer or
snapshot fallback is added, and external-memory/IOSurface capabilities stay off.

Each submission contains the full bounded resource handle/access list and up
to16 IB handle/offset/length records plus16 explicit wait fences. Requests are
zero-initialized and capacity overflow fails atomically; the transport never
silently splits one Vulkan submission into unrelated jobs. The native provider
must validate/snapshot untrusted PM4 and protect each task's GPU address space.
The existing kernel-authored marker operation cannot satisfy `RAW_IB_SUBMIT`.

Native fences stay64 bits throughout. Local reference counts allow queue and
multiple Vulkan sync objects to share a fence. Pending or dependency-retained
FenceClose returns BUSY and preserves the record. A negative submit reply with
a nonzero fence means publication occurred: its reference and kernel BO pins
remain tracked. Error, reset, timeout, an empty submit or an IOKit-success reply
alone never becomes a successful hardware completion. Only the typed native
terminal result establishes completion; provider resource/TLB retirement is
still required before BO reuse. Connection death leaves uncertain native DMA
resources to the kernel's retained/quarantine ownership.

BOClose may acknowledge closing the user's handle while native retirement is
still asynchronous. That acknowledgment is not a physical-page or GPU-VA
retirement proof. The native allocator/provider must continue reserving the
mapping, pins and underlying storage until its real GPU/cache/TLB retirement
finishes; userspace never supplies a physical page or forces IOVA reuse.

The source uses real kernel cache operations. It contains no EL0 cache
maintenance instruction, guessed cache-line constant or successful cache no-op.

The later source-only KMD ownership correction retains any valid typed
published-error fence, including kernel BUSY/TIMEOUT/NO_MEMORY; it poisons new
work while preserving cleanup ownership. Native wait snapshots hold their own
references across condition waits, and BO size accounting uses the actual
native allocation. Sync payload cleanup errors and GPU-writable IBs fail
explicitly. [KMD_SEMANTICS.md](KMD_SEMANTICS.md) documents the exact pinned Mesa
lock/mapping behavior and remaining host-coherent/cache integration. These
changes are unvalidated and do not enable persistent mappings, raw submission
or any Vulkan/Metal publication.

## Turnip integration

`tu_knl_dre.cc` implements the actual Mesa KMD hook shape from pinned
[Mesa85fda3b3](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/85fda3b36405bcaea8bffc64aa4f2651dc90770d/).
It requires every ABI1 Vulkan capability, including validated raw PM4 and
persistent mappings, before allocating a Vulkan physical device. Today's
provider therefore cannot enumerate one. Chip06010900/619, GMEM, UBWC, trap,
VA and memory-budget fields come from actual native GetInfo; A740 values,
AGX names, eight-core claims and arbitrary user VA allocation are not copied.
Device creation checks the native reset epoch against enumeration.

The adapter uses separate local Mesa BO indices; a64-bit kernel handle is never
cast into `gem_handle`. Every currently live BO is conservatively included in
the submit resource list, covering indirect shader/data/descriptor resources.
The first bounded implementation supports at most64 BOs,64MiB per BO and128MiB
per task. It does not claim full phone RAM or production UI capacity. Precise
per-submit residency can later reduce this conservative pinning, but must not
omit reachable resources. CPU mappings end explicitly on retirement; native
pending ownership survives BUSY close.

The common Vulkan threaded submit path is explicitly enabled so an unresolved
semaphore dependency does not block application `vkQueueSubmit`. Binary syncs
retain actual native fences, support pending observation only after accepted
kernel submission, and use Mesa's existing timeline wrapper. Empty submissions
respect earlier real dependencies without inventing a new GPU job. The generic
32-bit Turnip debug fence holds only a local latest-fence cookie; u_trace is
explicitly unsupported, rather than truncating or aliasing a native fence.

`dre_mesa_policy.h` withholds the known missing fd/dma-buf and host/IOSurface
imports, external sync handles, Metal WSI, placed maps, capture replay,
performance selection, calibrated/native timestamps, sparse mappings,
preemption and ray tracing. Other inherited Mesa feature claims remain
**unqualified**, not validated by this filter. The development build requires
`platforms=[]`; Metal WSI can recurse into the same factory and is refused.

## Source/build recipes for deferred validation

`prepare_turnip_dre.py` is an unrun recipe that creates a new checkout of
Mesa85fda3b3/tree b72ff1eb, applies all17 SHA-pinned reference patches from the
matching32446c8f orchestrator recipe, and overlays this transport/UAPI/policy.
It routes `tu_bo_sync_cache` through the native cache call and removes the
reference's `-undefined dynamic_lookup` link waiver. The original checked-out
sources and earlier tested artifacts are not modified. All17 patch behaviors
still require individual A619 audit; merely applying them proves no runtime
correctness. The separately tested compiler219f0136/Mesa26.2.6 path is a
different source stack and supplies no binary compatibility proof here.

`build_turnip_dre.py` is an unrun recipe for a real macOS/Xcode
`arm64-apple-ios16.0` dylib. It requires existing Meson, Ninja and
glslangValidator prerequisites, preserves real configure/build/link logs, uses
two jobs and does not download unqualified dependency substitutes. Matching
native Meson/Python/subproject prerequisites and this new recipe itself must be
qualified tomorrow. A builtin pack is optional; when provided it must match
the exact source/compiler profile and chip619/06010900. An A740 or
Mesa26.2.6/offline probe pack must never be relabeled. No A619 pack for this new
profile has been generated.

The runtime draft `package_dre_metal.py` requires actual iPhoneOS frontend
objects, a true iOS Rust archive and this exact Turnip dylib, checks their
source/hash correspondence, then attempts a strict SDK link. Its new
`dre_metal_factory.mm` follows the selected528-byte Metal306 class/init ABI and
refuses missing native capability, cache or registry identity. It does not
install the bundle or publish any service. See runtime/OVERNIGHT_DRAFT.md.

## Remaining blockers

- Connect and validate real RPM/power/clock/RBBM/PAS/SMMU/CP/interrupt providers.
  MMIO helper/model success is not the physical cold-start result.
- Implement a complete untrusted PM4 validation/snapshot path and task GPU
  protection before setting `RAW_IB_SUBMIT`; nested commands, arbitrary GPU
  writes, shader/code lifetime and recovery must all be bounded.
- Implement/qualify persistent CPU mapping and actual UCHE/cache ownership,
  immutable IB snapshots, completion and TLB retirement. ABI1 exclusive mapping
  support alone cannot sustain Turnip's persistent `bo->map` pointers.
- Qualify this source's C/C++ ABI, SDK compile, Meson contexts, strict dylib and
  frontend/Rust link, real software sync behavior, resource limits, negative
  replies, published-error ownership and timeout/retirement paths.
- Add genuine IOSurface page/plane/format/stride import and cross-process
  resource ownership, cache and shared-event synchronization. Host pointers,
  copied snapshots or cached rendered frames cannot impersonate these.
- Qualify all remaining Vulkan/Metal feature answers and compiler/runtime
  workarounds against A619, actual render/compute/readback workloads and system
  clients; implement bounded BO residency/cache/memory-pressure behavior.
- Add a real original-Metal-factory accelerator publication path only after
  native acceptance. The current control service deliberately does not publish
  `IOAcceleratorES`; a bundle draft alone changes no device enumeration.

Deferred checks should first exercise malformed protocol, generation/reset,
foreign/stale handles, map leases, full-resource bounds, pending dependencies,
published-error fences, no-reuse after uncertain retirement and real SDK/source
integration. None has been executed for this draft.
