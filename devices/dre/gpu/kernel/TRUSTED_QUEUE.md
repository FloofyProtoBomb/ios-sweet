# Private trusted IB/cache/fence draft

**UNVALIDATED.** No tests, compilation, CI, recipe, emulator, phone operation
or GPU/SCM instruction has been executed for these files. Native platform,
firmware/PAS, protected raw-command and Metal gaps remain.

`trusted_queue.c/.h` implements a bounded **private kernel diagnostic path**.
It is not connected to `IOUserClient::externalMethod`, native raw submission,
Vulkan, IOSurface import or the Metal device factory. No capability or shared
provider layout changes enable it.

## Inputs and ownership

The opaque queue can be prepared only through the real native allocator and
an already prepared, inactive owned VM. It allocates separate16KiB state,
command and completion/data buffers. Commands are GPU READ-only and XN;
only the distinct actual SQE UCODE role needs executable PTEs. Data is
GPU READ|WRITE. Every allocation requires actual NC/PA proofs. The actual VM
mapper stages the two GPU mappings, checks physical/table aliasing, and
records real generation/lease handles. These are private construction-time
allocations, never user BOs or imported surface memory.

The caller supplies a fresh wired `dre_gpu_trusted_retained_state` journal
recognized by the actual allocator/VM control owner. Initial allocation writes
its handle and memory view directly into that resident journal before any
verification. A partial or invalid first buffer therefore remains reachable
even when no safe opaque queue can be constructed. Later partial failures keep
the actual safely established queue as well. Neither record authorizes freeing
an allocation or dereferencing an unverified returned buffer. Boot binding
retains this journal inside its actual allocator-owned control object.

The real `DRE_GPU_ALLOCATOR_VERSION`, exact allocator `struct_bytes` and
status0 `classify`/typed `record_mapping` APIs are required before the first allocation. Once the first
state's exact NC/PA/permission proof succeeds, the
constructor tags its recorded allocation handle and memory view as private
`DRE_GPU_ALLOC_TRUSTED_STATE`, before constructing an opaque queue in that memory. Failure leaves
the first-allocation journal intact even when no safe queue pointer exists.
The distinct command and data allocations are classified once as immutable
GPU-readonly/XN `DRE_GPU_ALLOC_IMMUTABLE_IB` storage and
`DRE_GPU_ALLOC_PRIVATE_COMPLETION` data storage after their real
proofs, before VM mapping/publication. A rejected purpose preserves the queue,
handles and memory views. Neither addresses, requested access bits nor a
later caller's claim substitute for a genuine constructor-owned tag. The tag
is not immutability enforcement, device completion, cache retirement or a user
submission capability; RAW stays off.

Only after each genuine `VM_MAPPING_STAGED` result and its generation/lease/
first/count checks does the constructor call `record_mapping` with that exact
allocation, actual VM, exact VM-ops table and actual mapping handle. The
registry must derive the recorded prefix/GPUVA from real owned VM metadata;
the constructor supplies no scalar VA or invented mapping certificate. A
failure preserves the already staged handle and all private objects. State
remains CPU-only and unmapped. The inactive staged record does not claim live
PTEs: actual domain/PTE proof is still mandatory after genuine activation.

There is no function accepting command bytes, a shader/compiler blob, user
GPU VA, BO handle, surface token, resolved PM4 graph or "trusted" boolean.
The module itself authors exactly three fixed programs:

- `WRITE32`: one `CP_MEM_WRITE` to its own private data word.
- `COPY32`: one source32-bit `CP_MEM_TO_MEM`, control0, between its own words.
- `UCHE_CACHE_ONLY`: a fixed inert NOP IB surrounded by the real cache events.

The only varying numeric value is plain32-bit data. Addresses and packet sizes
come exclusively from the private constructor. This makes it impossible for
an unvalidated userspace command stream to enter through these functions.

The command buffer is immutable from sealing/publication through real engine
drain. Its kernel alias remains writable for controlled reuse after ticket
close; it is never mapped into a task or returned through a BO lookup. The
single native owner lock prevents another module operation from rewriting it.
The module compares its fixed expected command words before publication and
on queries, and checks the entire zero-padded command buffer at completion.
This ownership/immutability contract cannot be extended to mutable application
commands merely by calling them trusted.

## Initialization order

Call `dre_gpu_trusted_prepare` in the actual native configure path **after**
root's `platform/boot_memory` has prepared/staged the real CP/bootstrap VM
allocations, **before** startup activates that domain and before the exclusive
task adapter snapshots protected bootstrap occupancy. Pass that preparation's
actual `first_unused_gpu_va`; allocations search only at/above this checked
floor, preserving intentional empty protected holes below it. The function stages real
private mappings without MMIO or DMA. Partial failures retain the opaque owner
and uncertain objects; they do not erase a mapping handle or free its pages.

Then run the ordinary `dre_a619_startup_begin`. Only its actual marker-confirmed
state can be attached with `dre_gpu_trusted_attach`. Attachment requires exactly
one completed startup ring submission, active owned SQE/CP/SMMU/power state,
the expected ring geometry/security/protection registers and no outstanding
IB/SDS/MRB/ROQ work. It rejects an unknown prior command history.

All APIs require the **same real native owner/VM/SCM lifetime lock** used by
the coordinator, allocator and mapping code. The module takes exclusive ring
submission ownership: changes to its expected submitted/completed counters
refuse further admission. No independent marker or external raw queue may
silently share the ring after attachment.

## Actual packets and completion

Each fixed IB is submitted by genuine three-word `CP_INDIRECT_BUFFER` with its
20-bit length and no privileged-IB bit. The ring sequence emits:

1. `WAIT_FOR_IDLE`, A6XX `CACHE_INVALIDATE` UCHE event, `WAIT_FOR_IDLE`.
2. The real private indirect buffer.
3. `WAIT_MEM_WRITES`, `WAIT_FOR_ME`, `WAIT_FOR_IDLE`.
4. `CACHE_FLUSH_TS` with its supplied32-bit sequence/fence address, with both
   timestamp-override and interrupt bits clear.
5. `WAIT_FOR_IDLE`, UCHE invalidate, `WAIT_FOR_IDLE`, and actual `WHERE_AM_I`
   firmware acknowledgement to a separate word.

Ring space follows pinned KGSL's contiguous-packet/two-word-gap convention. If
needed, one NOP consumes the old tail and the packet starts at0; no packet is
blindly straddled across the ring boundary. Every publication journal is kept
before a potentially stateful WPTR write. A failed MMIO callback may already
have reached hardware, so it poisons the native owner and retains everything.

Querying checks actual ring base/control/RPTR/WPTR, SQE/protection/security,
RBBM idle/IRQ/fault, IB/SDS/MRB remaining counts, and their ROQ remaining counts.
It orders NC CPU reads after DMA, verifies the actual cache-event sequence,
`WHERE_AM_I` end pointer, expected private data value and full-page canaries,
then rechecks real startup/domain/PA/PTE/lease ownership. Software counters,
an advanced RPTR alone or a stale memory value cannot satisfy completion.

The wait helper has an actual19.2MHz budget capped at1second and a finite poll
ceiling. Timeout, stalled clock, hardware faults, altered commands or lost
ownership quarantine the ticket and coordinator; no software reset/free path
pretends to retire unresolved DMA.

## Ticket contract and limits

Tickets are a private nonwrapping32-bit object generation plus a nonwrapping
hardware32-bit sequence packed into uint64. At most one ticket is held.
`query`/`wait` return explicit pending, hardware-complete or error-retained
status. A nonzero ticket on any failed attempt remains owned even when the
failure preceded the WPTR attempt. These values are **not** DRE user-client
fence handles and must not be returned to Vulkan as if they represented its
command submission.

Completion's `engine_references_drained` covers only these fixed private IB
programs. The cache scope is explicitly `UCHE_FLUSH_INVALIDATE`. A successful
close permits their pages to be reused under the same owner lock. All private
allocations and GPU mappings remain pinned for the boot lifetime. This module
provides no PTE/TLB unmap or published-allocation free operation; any future
teardown must coordinate actual VM/TLB retirement and bootstrap-snapshot
consumers before freeing a physical page.

## Exact evidence and remaining gaps

Packet meanings/fields come from Mesa commit
`85fda3b36405bcaea8bffc64aa4f2651dc90770d`,
`src/freedreno/registers/adreno/adreno_pm4.xml` and `a6xx.xml` plus
`src/freedreno/vulkan/tu_cs.h`/`tu_cs.cc`. The XML defines UCHE flush/invalidate,
timestamp-vs-value control and A6XX remaining/ROQ registers. The pinned primary
file hashes are recorded in `../vulkan/pm4/sources.json`.

The actual 64-bit IB wrapper, supplied-value cache-event fence and ring-wrap
ordering are grounded in Lineage commit
`98a38783f42f5588bf00cd933e299aedc589bc10`,
`drivers/gpu/msm/adreno_ringbuffer.c`; the same cache-event-fence mechanism is
used by Linuxv6.12 `drivers/gpu/drm/msm/adreno/a6xx_gpu.c`. This source writes no
arbitrary context/global register packet, changes no page table mid-job and
enables no IRQ while the existing no-handler native profile is selected.

The path does **not** execute shaders, construct a graphics/compute pipeline,
retire CCU/SP/LRZ/UBWC rendering state, or qualify application-resource cache
ownership. Its cache history is restricted to cold startup and these fixed CP
programs. Other queue histories, shader/render cache scopes and protected task
domain transitions are unsupported. A real shader diagnostic needs a genuine
trusted pipeline builder and stage-specific barriers before this can become a
shader execution path. Full raw PM4 isolation, scheduling, IOSurface sharing,
persistent-map qualification and Metal API acceptance remain independent work.
