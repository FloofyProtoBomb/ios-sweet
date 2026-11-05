# Actual allocation inventory — UNVALIDATED

Source only. No tests, compilation, recipes, CI, emulator or phone operation
ran. Public selectorABI1 and outer native-provider version2 stay unchanged.
The private allocator descriptor now has explicit version1/struct_bytes plus
typed classification, mapping-record and synchronous observation callbacks.

The registry is the service's existing real IOBuffer allocation list, protected
by its actual IOLock/current-thread ownership checks. Each successful allocation
gets a monotonic nonzero generation and an immutable original granted-access
record, independent of the mutable physical-page metadata. Verification checks
actual descriptor segments, kernel translation/NC proof and every page's access
against that original record. Full descriptor backing includes16KiB padding.

Kernel constructors can classify exactly once, before publication, after an
exact handle/memory-view/PA/NC check. Purpose is not a mapping permission or
privilege claim. UNKNOWN remains unknown/excluded; the external callback cannot
choose TASK_BO. That purpose is assigned only inside the actual BO_CREATE path
after matching the allocation to exactly one BO slot of the genuinely
registered IOUserClient, its retained task and current connection generation.
The allocation retains that actual client until real retirement/release.
User-provided numeric roles or task identities do not establish this record.

`record_mapping` consumes the actual VM, exact ops and map handle AFTER genuine
VM staging. It validates owned private metadata, immutable VM identity,
generation/lease, prefix pages, original PA/access and PTEs. Inactive staging
requires zero leaves and stays STAGED, not translation-ready. Active views
recheck actual live SMMU/PTE state. A still-valid retired handle is represented
as RETIRED only after observing zero leaves/leases/pages; lost/reused handles
refuse rather than being cleared into an invented unmapped state. No scalar
bo_map VA, role, slot or reserved range supplies a mapping proof. The actual
exclusive-VM path can record its real entry; generic provider scalars remain
NO_MAPPING_RECORD, meaning unknown rather than proof of no mapping.

`with_catalog` accepts only a real registered client/generation and genuinely
owned CATALOG_SCRATCH output storage. That purpose is immutable private RW CPU
data, cannot be TASK_BO and is rejected by record_mapping; it cannot back live
VM-control dependencies. All allocator mutations are refused during the scope.
Actual descriptors and the target client/service acquire real retained
references before the visitor. Physical lists and record pointers are borrowed
only inside that synchronous real-mutex-held scope. The service validates its
actual registry/pins/current records, releases those real refs, and clears the
borrowed snapshot before returning. Dropping the mutex inside a visitor is an
illegal hard fault: the scope and all references remain retained, with no
unsafe cleanup. No general asynchronous snapshot/unload API is offered.

The data snapshot is explicitly **ALLOCATOR_DATA_ONLY**. It covers every actual
descriptor-backed data allocation and padding; unknown and foreign-task records
are preserved, never silently retagged or omitted. Registry/page-list/sorted-PA
bookkeeping uses real retained IOMalloc ranges. Exact kernel-only CPU ownership
of those ranges does **not** establish private ownership of their surrounding
shared16KiB heap granules, nor inventory private reserved firmware/global
backing. `owns_cpu` is the existing general allocator metadata/data proof;
`owns_private_cpu` restricts task-visible data and live table output, while
remaining a CPU-range proof, never a global physical-isolation certificate.

Consequently this catalog cannot satisfy the non-installable task planner's
complete protected-physical-inventory requirement. The observer must return
the explicit full-inventory refusal rather than promoting DATA_ONLY coverage
to a protected VM or RAW capability. A shared bootstrap window is labeled as
such, using the actual exclusive binding; absent genuine reservation metadata
stays absent. Real task isolation/context switching/privileged transactions,
immutable command validation and unforgeable hardware retirement remain
separate unresolved prerequisites. No Metal device or public capability is
enabled by these source changes.
