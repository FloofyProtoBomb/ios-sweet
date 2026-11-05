# Actual allocation-catalog observation — UNVALIDATED

The actual service registry now supplies descriptor data/padding, immutable
purposes, task/connection generations and typed VM mapping records. It does
**not** supply the complete protected physical inventory required by `plan.c`.
This source therefore adds a genuine synchronous inspection/normalization
connection, with explicit planner refusal. It contains no proposal entrypoint,
PTE encoder/application, context switch, queue operation or capability change.
No tests, checks, builds, recipes, CI, emulator or device actions ran.

## Real authority, not caller labels

`dre_taskvm_inspect_registered_catalog` calls the allocator's actual version1
`with_catalog` API with a registered IOUserClient object and connection
generation. The service resolves that object in its real client registry under
the same service mutex used for task, connection, allocation and mapping
lifetimes. It retains actual descriptors/client/task references for the visit.
The adapter cannot substitute a task integer, public BO handle or VA whitelist.

Every source record is checked through actual `catalog_record_current`, and
the complete descriptor-data snapshot through `catalog_current`, before and
after inspection. All exact1 callbacks require **exact1**, never a nonzero
negative status. The status0 visitor result means inventory inspection only.

The binding, workspace, sorting array and output must resolve genuine immutable
`DRE_GPU_ALLOC_CATALOG_SCRATCH` records with no task owner, user mapping, GPU
mapping or publication, and must also pass actual `owns_private_cpu` checks.
Callers must allocate whole private wired scratch allocations and classify them
through the genuine allocator before use. The real mapping API refuses that
purpose. A live PRIVATE_CONTROL range or translation table is not writable
inspection storage. The snapshot itself has the same genuine scratch restriction.
Actual borrowed page-list metadata must pass the exact private CPU range check,
without being classified as writable scratch. Every authoritative page-list
range is checked for output/scratch overlap before the first adapter store;
private writable storage is not permission to corrupt another registry record.
A CPU pointer, task-visible BO or stack wrapper is not promoted to private
storage merely because the kernel can address it.

## What is genuinely inspected

The bounded128-record/65536-page walk includes every descriptor's full4KiB PA
list and padding. It checks whole16KiB **descriptor-data** granules, granted
access consistency and physical duplicates using bounded heapsort. These checks
do not certify the surrounding heap granules used by metadata itself.

Record observations preserve actual allocator purpose and generation, visit
pin, granted access, complete/prefix page counts, mapping state and actual
mapping lease/generation/VA. Unknown-purpose records are retained and counted.
Foreign task/connection records are retained and counted with a separate owner
relation. Neither is omitted or relabeled to manufacture task-plan acceptance.
The output contains no allocation object, descriptor, CPU/page-list pointer,
physical address or proposed leaf.

Only the catalog provider's real typed VM handle establishes mapped VA/prefix.
It verifies actual VM PA/permission/PTE/generation/lease state. Generic `bo_map`
scalars or slot arithmetic are not accepted as that proof. Mapping state0 means
**no authoritative mapping record**, not proof that an allocation is unmapped.
Retired records preserve their genuine historical lease/generation with zero
current VA/prefix. A shared bootstrap window remains shared, never task isolation.
The inspector does not create a window when the actual native source lacks one.

## Exact unresolved authority

`DRE_GPU_CATALOG_ALLOCATOR_DATA_ONLY` covers real IOBuffer data allocations and
full padding. Page-list/registry metadata currently uses retained IOMalloc
storage whose surrounding16KiB heap granules can be shared. Private reserved
firmware backing is also outside this descriptor-data registry. An exact
kernel-only metadata range check cannot certify all those physical exclusions.

Consequently the only completed observation result is
`TASKVM_REGISTRY_DATA_OBSERVED_PLANNER_REFUSED`. **No call to
`dre_taskvm_build_proposal` exists in this adapter.** Unknown coverage kinds also
refuse; there is no invented complete-coverage enum, always-true verification
callback, unreachable pretend-success proposal branch or ready bit.

Connecting the unchanged planner later requires a genuine complete retained
private/reserved/firmware and descriptor physical inventory, including metadata
granule exclusions and actual task reservation authority. That source must
resolve every purpose/owner/generation/page against real records under the real
mutex. It cannot be replaced by flipping a field on this data-only snapshot.
The planner's separate privilege/cache/firmware/TLB requirements still apply;
even a future non-installable proposal will not prove protected execution.

## Synchronous expiry

The provider clears the borrowed snapshot and drops retained visit references
before returning. Copied observations are historical generation-tagged facts,
not live pins, transferable authority, a protected address space or a mapping
token. All outputs must be ignored for any result other than the exact factual
observation string, including any final provider generation/scope failure.
Failure before real private output ownership proof can leave earlier caller
scratch untouched. No shared planner/provider/runtime/staging source is changed
by these three new files; source inclusion and any future invocation are separate.
