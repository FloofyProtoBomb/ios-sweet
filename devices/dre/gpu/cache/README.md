# A619 private cache-boundary planning — UNVALIDATED

No test, compile, recipe, CI, emulator, phone, cache instruction or GPU command
has run for these new files. `a619_cache.c/.h` authors bounded private packets
only; it cannot publish a queue, observe completion, retire references, grant RAW
submission or enable Metal. Existing trusted_queue files/provider/staging remain
unchanged. The native firmware GPU/IPA shared-granule and other startup blockers
still prevent physical CP operation.

## Grounded distinctions

The pinned [Mesa A6XX event table](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/common/freedreno_gpu_event.h)
maps UCHE clean to CACHE_FLUSH_TS with an address/value payload, and UCHE
invalidate to CACHE_INVALIDATE. CCU color/depth clean events also carry that
payload; their invalidations do not. A6XX and A7XX event numbering/meaning must
not be interchanged. `sources.json` records exact names, values and pins.

Mesa's[`tu6_emit_flushes`](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/vulkan/tu_cmd_buffer.cc)
cleans CCU color/depth before invalidating them. Its regular barrier path then
handles UCHE, pending bindless descriptors, CP writes, WFI and WFM. The new
conservative planner also cleans UCHE before requested UCHE invalidation.
It never treats an invalidation as preserving unknown dirty data.

The same source's CCU mode transition requires an actual known mode, flush/
invalidate and A6XX WFI before programming RB_CCU_CNTL. A flush packet alone
does not establish that render state. This planner cannot change CCU mode.

The[`A6XX register definition`](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/registers/adreno/a6xx.xml)
defines SP_UPDATE_CNTL's bindless fields as clearing pending CP_LOAD_STATE6
loads. The optional PRE_READ packet touches only those exact CS/GFX fields,
with a real following descriptor re-emission requirement. It is not a shader
instruction-cache flush, state reset or proof that lazy references retired.

The[Lineage ring implementation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_ringbuffer.c)
uses a supplied-value CACHE_FLUSH_TS end-of-pipeline write. Its HLSQ_FLUSH0x07
path is explicitly A3XX, so this source does not copy it as an A619 fix. The
matching[A6XX implementation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_a6xx.c)
also uses CCU invalidate events24/25. IRQ and timestamp-override bits remain
clear in the authored cache fence; no unowned IRQ path is enabled.

## Actual API and ownership boundary

The kernel-private API takes normalized producer/phase/cache-state metadata,
four actual owner/domain/queue/state generations, a nonwrapping sequence, and
two mandatory provider callbacks. There is no IOUserClient entry, user PM4,
user BO, surface token, compiler/shader blob or caller physical address input.
Callbacks must return **exact1** after actual validation; native status0 must
not accidentally be treated as ownership proof. No callback implementation or
provider integration is supplied by this source.

Under the real native owner/VM/queue lifetime lock, callbacks must prove complete
command history, current state and actual dirty-domain ledger, pin/disjointness/NC permissions for a real
private16KiB completion allocation, ordered CPU stores and fresh fence/ack
baselines. The page callback only describes already owned/prepared memory;
planning callbacks cannot allocate/release or submit/change hardware.
Conditional command history, missing callbacks or changed generations
refuse. Request booleans are normalized private state, never user assertions
and never a substitute for those real checks. Callbacks are synchronous and
cannot retain the planner's request pointer.

The private page reserves offset0 for event dummy writes,4 for the supplied
sequence fence and8 for WHERE_AM_I. The provider must actually initialize
fence=~sequence, ack=UINT32_MAX and its own canaries before publication. The
builder initializes no real memory or hardware. Its local output is bounded
to64 words/32 packets (currently at most39/17), with capacity checked before
each append. It returns packet boundaries so a real ring wrapper can preserve
packets across ring wrap. Words remain untouched on failure; a cleared plan
word count prevents old output from being mistaken for a new authored plan.

The authored sequence orders CP writes/WFI/WFM, scoped CCU/UCHE events and
optional bindless update, then CP writes/WFI/WFM, a fresh UCHE-clean value fence,
WFI/WFM and WHERE_AM_I. The final fence is this boundary's tag, not completion
of future inserted work. Even a descriptor-only boundary includes the final
UCHE clean and reports that in its planned scope. Plans must remain immutable
and pinned until actual execution/ownership retirement. Scratch output is not
an executable object: the real executor must copy/seal the exact words into its
own immutable private pinned command allocation and verify it before publication.

## Explicit refusal and retirement limits

Unknown, GMEM or active/unclean LRZ state refuses. A SYSMEM render boundary needs
closed render-pass state and actual validated CCU bindings/history. A private
CP/compute boundary cannot silently inherit unresolved graphics work. The
planner never emits a GMEM resolve, depth clear, LRZ direction update or a
general SP-state write. These stateful paths need their actual providers.

Mesa's[`tu_lrz_tiling_end`](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/vulkan/tu_lrz.cc)
selects LRZ enable/fast-clear/direction handling and may need a clear before
its flush. Emitting LRZ0x26 without that state would be a guessed operation.
Likewise UCHE/CCU cleaning cannot write unresolved GMEM attachment contents
back to their intended resource automatically.

Output contains **requirements**, not observed-ready/retired flags. A future
executor must establish fresh fence/firmware ack, actual ring/IB/SDS/MRB/ROQ
drain, fault-free unchanged ownership, NC CPU read ordering and complete pinned
resource history. Shader work additionally needs actual shader-pipeline/lazy-load
reference retirement. Render work needs real CCU bindings and event-retirement
semantics; an observed UCHE fence alone cannot substitute for them. Mapping
removal, SMMU/TLB retirement and allocation free are separate obligations.

The existing trusted queue proves only its own fixed CP/UCHE templates once
physically executed; this new planner does not broaden that scope. A complete
native cache API still needs source/provider integration and bounded fault/order
tests, exact SDK/kernel compilation, render/LRZ/GMEM state owners, hardware
shader/attachment visibility and lifetime tests, recovery/IRQ handling and real
post-event firmware-reference observations. None is satisfied by authored words.
