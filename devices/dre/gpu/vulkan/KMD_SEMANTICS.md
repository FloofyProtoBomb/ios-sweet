# Direct KMD ownership details — UNVALIDATED overnight source

No new source has been compiled, linked, tested, prepared or executed during
the overnight period. No native Vulkan device, Metal acceleration or physical
GPU result is established. The notes below come from source inspection of the
matching pinned Mesa85fda3b3 and the authored ABI1 transport, not runtime tests.

## Native publication is independent of error status

The kernel's `DREAdrenoGPUClient::submit` creates a bounded owned job, pins its
full resources/dependencies, calls the actual native provider and returns its
opaque fence when publication/ticket ownership exists. A provider can return
any typed negative status **after** publication. BUSY, TIMEOUT, LIMIT and
NO_MEMORY therefore cannot be treated as proof that nothing happened.

The direct C transport records every nonzero fence from a complete validated
ABI reply, independently of that status. It preserves one caller reference,
poisons new work on a published error and returns LOST. The caller must release
that reference even on failure; pending zero-reference records still belong to
the connection/native retirement owner. Mach failure, truncated output,
unknown status, invalid generation/reset or malformed handles do not create a
fabricated fence; unknown ownership stays with kernel orphan retention when
the connection closes. Source uses an explicit per-call typed-reply flag so a
partially filled error buffer cannot be mistaken for an accepted ABI reply.

The same distinction protects a CPU epoch returned with a typed partial error.
The epoch is retained for cleanup and new work is poisoned. A recognized caller
fence reference is consumed by release even if collection afterwards fails;
the sync wrapper clears its own payload before release, preventing a second
release of that same ownership on a retry. Reset/signal/move cleanup failures
are returned as errors; they do not silently become a successful logical signal.

## Wait snapshots and actual Mesa lock behavior

Pinned `tu_queue.cc::queue_submit` holds `dev->submit_mutex` when entering the
KMD queue callback. The DRE `pending_wait` condition deliberately releases that
mutex so CPU signal/move and other submit progress can occur. Every already
resolved native wait now owns an independent reference **before** a later wait
can sleep. A bounded RAII snapshot retains at most16 distinct native fences
and releases them on all callback exits. This avoids depending on a mutable
sync object's payload surviving a condition wait. Actual accepted native
submission then retains its own dependency tickets in the kernel.

Empty submissions wait for their real dependencies in the existing threaded
worker. They may reference the last real FIFO fence, but do not create a new
GPU fence or claim work ran. Debug queue cookies stay local32-bit values;
Vulkan sync payloads retain the full64-bit native fence. u_trace remains
unsupported; an old debug cookie is not silently rebound to another job.
`tu_queue_finish` stops the generic submit worker before native queue/session
closure. The new ownership code still needs real concurrency/fault testing.

## BO size, mappings and executable admission

Mesa's common allocator aligns to its host `os_page_size`; native ABI1 returns
the actual16KiB-rounded allocation. The BO record and debug-memory bookkeeping
now use that returned size. Requested bytes alone cannot account for a larger
physical/task mapping or the native per-connection byte limit.

The BO's `gem_handle` remains a separate local sparse-array index. It is never
a truncated native64 handle; its `darwin_handle` is a full native object handle.
All current live BOs are conservatively pinned as submission resources. This
is bounded and may reject a real workload; it does not omit indirectly reached
shader/data/descriptor storage to make the submit pass.

Pinned `tu_cs.cc::tu_cs_add_bo` allocates ordinary read-only command streams
with `TU_BO_ALLOC_GPU_READ_ONLY`, but also supports GPU-writable streams and
patch points. The draft native permission model uses READ|EXECUTE without WRITE
for potential command/code backing, and READ|WRITE without EXECUTE for data.
The KMD intake now refuses a GPU-writable/non-executable IB and misaligned or
out-of-range entries before native publication. It does **not** relabel a
writable stream as immutable executable storage. READ_ONLY is merely a mapping
permission, never proof that the command contents, shader descriptors or nested
references have been validated. Kernel-owned immutable snapshots and a full
protected command path remain missing; RAW capability stays absent.

`tu_bo_unmap` in common Mesa ordinarily calls POSIX munmap. The draft marks
native BO mappings `never_unmap` and closes the actual kernel IOMemoryMap lease
only during explicit native retirement, avoiding a fake POSIX/DRM mapping.
Keeping that pointer alive does not keep CPU access ownership valid forever.

## Persistent host-memory/cache integration is still incomplete

Three actual common-source behaviors constrain the remaining implementation:

1. `tu_physical_device_init` creates HOST_VISIBLE|HOST_COHERENT memory type0
   unconditionally. No native ownership protocol is consulted there.
2. `tu_bo_map` returns immediately when `bo->map` already exists, without
   reentering the KMD or acquiring another CPU-access epoch.
3. Common mapped-range cache operations are skipped when the cached
   non-coherent-memory flag is false (normally ARM64); the native persistent
   epoch contract cannot be implemented merely by replacing a cache-line helper.

The direct transport deliberately refuses generic partial cache calls on a
persistent map. An arbitrary range flush is not the actual full-BO CPU/GPU
ownership transition, UCHE barrier, immutable-snapshot lifetime or retirement
operation. Unknown cache-operation enums fail instead of defaulting to an
invalidate. Native cache failures remain explicit.

Blindly ending every live CPU map at queue submission would be incorrect:
pointer lifetime and CPU-access epochs are separate, and unrelated mapped BOs
can have independent host activity. A genuinely qualified KMD needs actual
per-submit reachable-resource/access ownership, legal host-coherent memory
semantics or a real non-coherent memory profile, explicit Vulkan map/flush/
invalidate/fence-wait integration and actual provider GPU cache/retirement
operations. The selected kernel still masks PERSISTENT and RAW, so this draft
cannot enumerate a physical device while those integrations are missing.

No new capability, provider callback, ABI selector or alternate software/fake
device is added by this KMD ownership slice. Tomorrow's validation must cover
all typed published-error statuses, reply truncation/generation/reset errors,
counter/capacity bounds, pending waits interleaved with payload moves/resets,
one-time release on collection failure, physical allocation-size accounting,
writeable IB refusal and the real CPU/GPU mapping/cache transitions separately.
