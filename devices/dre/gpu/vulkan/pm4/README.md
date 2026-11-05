# A619 PM4 intake — UNVALIDATED structural subset

This is a concrete private command-copy and packet-intake implementation draft.
**No tests, compile, source-preparation recipe, CI, QEMU or phone execution has
run.** The source and its ABI have not been qualified. It does not complete a
raw Turnip command validator or permit GPU submission. Every successful result
is named `STRUCTURAL_SUBSET_ONLY`; `raw_submission_authorized` remains zero.
Nothing here changes a provider capability or installs a native submit hook.

## Source-derived admission

`sources.json` records the exact public Mesa85fda revision and reference-file
hashes used during source authoring. The
[PM4 XML](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/registers/adreno/adreno_pm4.xml)
and [packet helpers](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/common/freedreno_pm4.h)
define opcode numbers, TYPE7 odd parity/header fields,64-bit address forms and
the20-bit nested-IB word count. The
[A6XX register table](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/registers/adreno/a6xx.xml)
supplies privileged CP/ring/SQE/protection/context, RBBM/reset, UCHE, VBIF and
SECVID register references. The XML itself warns that packet IDs can be reused
across generations; old WAIT_REG_GTE opcode53 must not be mistaken for safe
waiting on A619, where53 is SMMU table update.

The narrow admitted subset is:

- TYPE7 NOP, with its bounded payload treated strictly as skipped data.
- Zero-payload WAIT_MEM_WRITES and WAIT_FOR_IDLE. These are not completion
  proofs and still require an actual provider's hangcheck/ownership handling.
- MEM_WRITE with a64-bit destination, bounded inline32-bit values, and the full
  destination range in a submitted, pinned, writable task data BO.
- MEM_TO_MEM only in its exact five-word, control0, one-source32-bit copy form.
  Both source and destination addresses are checked. Negation,64-bit forms,
  additional operands and unknown controls are unsupported.
- The three-word INDIRECT_BUFFER form, resolved to an EXECUTE/READ-only task
  BO and copied into its own private immutable snapshot. High size/control
  bits, conditional/chained/prefetch variants and unknown packet shapes fail.

Every TYPE4 register packet is denied. Privileged-range classification only
improves the error; a register outside those conservative windows is still
unsupported. Explicit privileged TYPE7 opcodes include ME_INIT, SMMU update,
secure/protected mode, firmware bootstrap, register RMW/copies, pseudo/context
state, preemption and internal jump-table controls. Other unsupported opcodes
also fail, including draws, dispatch, LOAD_STATE6, SET_DRAW_STATE, event writes,
predication, conditional command paths, resource lists and future packets.
There is no skip-unknown success or inherited register-state allowance.

## Private kernel contract

The public IOUserClient request must already have been copied, its full
resource list resolved to actual kernel BOs, and all BO/job pins acquired.
`dre_pm4_request` contains only trusted, stable kernel descriptions and
generation-scoped handles. A provider may assign private parse IDs to the
already resolved native BO objects; those IDs are not user-supplied address
authority. Original GPU VAs must remain fixed through native retirement.

`owns_resource` must prove the real task/session, ordinary pinned RAM, mapping
lease, permissions and exclusion of secure/MMIO/bootstrap aliases. BO grants
require READ with optional WRITE or EXECUTE, never W+X. Submitted resource
access is READ/WRITE only. The complete protected VA map must come from actual
bootstrap ring, SQE, powerup, page-table, marker and other protected allocations.
`owns_protected_ranges` proves that map's completeness for the current VM;
checking a caller ready bit is insufficient. All proof callbacks accept exactly1
as success; negative error values are refusal.

The caller supplies private wired workspace and result arrays. The source
checks bounds, ownership and array/input overlap before snapshot acquisition.
The word arena is16KiB aligned and each snapshot reserves whole16KiB pages.
The callback copies **actual BO bytes** into that pre-reserved destination and
seals them against task, GPU and other-thread mutation, returning an actual
private snapshot lease. No direct user-memory parse or original mutable IB is
accepted. `owns_immutable_snapshot` must prove that lease, not merely compare a
hash while another mapping can still mutate the bytes.

Every snapshot slot and its rounded arena reservation are recorded **before**
the copy callback. A partial/seal failure must expose any retained lease in the
reserved slot; the callback cannot lose hidden ownership on failure. The
parser leaves partial results and all leases intact. It allocates/frees no
storage, performs no MMIO/DMA, and never resets `workspace.in_use`. The owner
may dispose/reuse it only when unpublished ownership is certainly released or
actual published-job GPU/cache/TLB retirement permits it. A second call with
an in-use workspace returns BUSY without destroying earlier cleanup metadata.

Limits are16 roots,64 BOs,32 snapshots, nesting depth8,16,384 words per IB,
65,536 total copied words,64 snapshot pages/1MiB arena including alignment,
16,384 packets,512 memory references and64 nested edges. The walk uses a fixed
explicit stack; it has no C recursion, heap allocation or unbounded packet loop.
Cycles and repeated/overlapping original IB ranges fail. Repeated acyclic
subprograms are deliberately not accepted: accepting a shared graph once could
hide exponentially repeated execution behind a small static parse budget.

Address checks cover the entire byte range,48-bit VA bounds, alignment, actual
resource inclusion, protected overlap and requested permissions. Data writes
cannot target executable BOs. Roots and all nested children are independently
copied. Each memory reference and child-IB relocation is retained with its
source snapshot and packet/address word offset. Ownership/seals are rechecked
before returning the structural plan, while the caller's locks/pins must still
remain held through later materialization and retirement.

## Materialization remains mandatory

The parser's CPU snapshots are not executable GPU addresses. A real provider
must create separate kernel-owned executable GPU BOs, copy the snapshots, patch
each child address using the returned edge graph, preserve the validated word
counts and protect those new command copies from all user/GPU mutation.
Publishing the original root or nested user BO addresses would discard the
snapshot guarantee. All code/data references, descriptor resources, commands
and leases must survive uncertain publication, reset, cache/TLB and completion
paths. The existing single-exclusive-task bootstrap VM has not established
full per-task isolation and keeps RAW disabled.

The original
[Turnip CS source](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/85fda3b36405bcaea8bffc64aa4f2651dc90770d/src/freedreno/vulkan/tu_cs.cc)
also supports GPU-writable command streams and visibility patching. Those
cannot be transparently admitted as RO/EXEC immutable snapshots. Draw-state
indirection, register bits/state, shader ISA/code bounds, shader descriptor
addressing, rendering/compute memory effects, GPU cache semantics, scheduling,
robustness and recoverable task protection remain unsupported. This subset
cannot run a real Metal render/compute workload or qualify a Vulkan feature.

## Deferred checks

Tomorrow's independent fixtures should cover canonical versus bad parity and
reserved header bits; truncated packet payloads; source-derived opcode variants;
privileged/register/unknown denials; exact full-range READ/WRITE/EXEC/resource
and protected-boundary checks; cycle/repeated-IB/depth/word/page/reference
limits; partial-copy leases; negative proof-callback values; stale generation,
mutable snapshot and ownership loss; output/workspace/input aliases; and
retained cleanup records after every failure. Actual immutable GPU copy and
relocation correctness, native BO pins, IRQ/error-after-publication, cache/TLB
retirement and hardware workloads require separate integration checks. None
has been run for these new sources.
