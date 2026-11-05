# Native A619 kernel transport draft

**UNVALIDATED.** Written during the user's overnight source-only window. No
compiler, test, CI, QEMU, phone diagnostic, flash or install has been run for
these files. They are not in the staged kernel and provide no working Metal
device. The first full iOS phone boot remains held.

`DREAdrenoGPU.cpp` implements a built-in, explicitly opted-in IOKit control
service named `DREAdrenoGPU`. It does not publish `IOAcceleratorES`, an AGX
personality or Metal device-factory properties. `dre_gpu_native_publish` also
requires `dre_gpu_native=1`; the boot argument alone installs no provider.

## Actual binding boundaries

The kernel-private provider version is now2; selector ABI remains1. A mandatory
`bind_owner_lock` receives callbacks for the service's actual IOLock, so native
IRQ workers, broker, SCM/RPM, VM, BOs, queues and retirement use the same real
mutex. Missing lock binding refuses hardware acquisition. This connects actual
serialization; it grants no device/rail/reset ownership. Partial escaped lock
references remain boot-held. See [OWNER_LOCK.md](OWNER_LOCK.md).

If the native provider has no FDT view yet, the binding obtains the actual
boot-held snapshot from `dre_native_board_snapshot`, without initializing UFS.
The binding validates a complete original Holi FDT, board family IDs, chip ID,
GPU interrupt and exact GPU/wrapper/SMMU/GPUCC/GCC apertures. It maps registers
as Device with removable `ml_io_map_unmappable`, merging the overlapping
GPUCC/wrapper aperture. Each aligned register operation verifies the same
exclusive native lease. No external method accepts register or physical
addresses. Register mapping is not evidence of device ownership or power.

`configure` supplies actual acquired RPM/clock/reset/IRQ policy, SCM firmware,
SMMU state, buffer mappings and module operations. The binding passes those
to the shared `dre_a619_startup_begin`; it contains no second power/CP startup
sequence. A successful startup confirms the existing restricted native marker
only. Failed startup retains all resources and never performs speculative
voltage reductions, DMA cleanup or partition operations.

Owned BO allocation uses a wired `IOBufferMemoryDescriptor`, explicitly
requests WriteCombine, prepares it, records every 4 KiB physical page using
`kIOMemoryMapperNone`, independently compares `kvtophys`, and rejects FDT
reserved RAM or addresses outside the supported PA range. Requested cache
options are insufficient proof: either the native `normal_nc_alias` provider
or the explicitly integrated pmap proof below must verify actual Normal-NC
aliases before allocation is accepted. Device memory
is never offered as a BO. Allocations are rounded to the actual 16 KiB CPU
granule; padding remains zeroed, owned and pinned with the whole allocation.

CPU leases hold a real `IOMemoryMap` created in the owning task. A second lease
or GPU-pinned BO is rejected. The native provider must actually prepare GPU
cache ownership and verify the user alias's cache type/PA before the address
is returned. CPUUnmap explicitly calls `IOMemoryMap::unmap`; unsuccessful
revocation/cache retirement quarantines the BO. Submission does **not**
silently invalidate a caller's persistent pointer: it returns BUSY while an
exclusive CPU lease remains. Turnip requires persistent mappings, so its
separate capability and cache contract remain absent in this implementation.

There are 64 task-scoped BO slots, 128 fence slots, at most 64 unique BO
resources, 16 IBs and 16 prior-fence dependencies per atomic submission.
Handles contain a nonwrapping generation and are checked against a separate
connection generation. BO size is limited to 64 MiB and wired task allocation
to 128 MiB. Closing a BO drops user visibility but retains its slot/mapping
while jobs hold it. Closed/dead tasks remain strongly held by the service and
are polled for actual hardware retirement; uncertain jobs are retained.

The command request is copied once from IOKit input. IB ranges and complete
read/write resource lists are checked and pinned before provider submission.
Wait tickets remain referenced by dependent jobs. Raw commands are admitted
only when an actual kernel provider proves immutable command snapshots,
complete nested PM4/address validation and task protection; an initialized
ring or marker helper cannot supply that capability. No such complete raw
provider is installed by this draft.

## Exclusive bootstrap VM adapter

`dre_gpu_vm_binding.c` binds actual BOs into `startup.plan.vm` through the
existing `dre_a619_vm_map` and `dre_a619_vm_unmap` implementations. The native
provider must explicitly select `use_exclusive_vm_adapter=1`. It does not
fabricate an isolated command-execution domain: raw submission, hardware
submit/fence and persistent-map capabilities are forcibly absent.

After real startup, it validates the initial leaf PTE/PA/permission/lease
metadata and permanently reserves every initially occupied 2 MiB block. Native
protected ranges can reserve additional whole blocks, including empty guard
space; these ranges never come from a user request. Task BOs use only actual
empty PTE/page/lease ranges, aligned to whole 16 KiB CPU granules. Each search
is linear in the window size, not quadratic in BO page count. The VM mapper
still proves actual owned PAs, excludes table/PA aliasing, publishes real PTEs
and performs hardware TLB synchronization.

One exclusive task identity plus a connection generation can own this adapter
at a time. It snapshots the VM generation and bootstrap mapping/page counts;
unexpected external mapping changes, altered ownership or failed map/unmap
postconditions quarantine the binding and retain all uncertain allocations.
A whole BO can retire only through real cache/firmware quiescence, PTE clear
and TLB retirement. CPU-page padding is never freed separately. Closing a
session requires every task BO to have retired while original bootstrap
resources remain held. There is no reset-recovery/free shortcut.

The native allocator now exposes private registry-backed `owns_cpu`,
`owns_ram` and `verify_memory` hooks for actual VM operation adapters. CPU
ownership includes the wired IOBuffer and its private wired slab metadata.
PA lookup uses a separately sorted private PA index and binary search, with
a cached allocation owner; it does not rescan all BO pages for each queried
page. Startup tables can request `allocate_contiguous`, which requests physical
contiguity and separately verifies the descriptor segment and complete PA
sequence; ordinary BOs use the noncontiguous allocator. CPU virtual contiguity
alone never proves a table PA span. The registry never includes the FDT, register mappings or arbitrary
caller ranges. `verify_memory` independently rereads descriptor PAs and
`kvtophys` before the actual alias proof. A completed GPU/TLB retirement is
remembered separately from descriptor completion, so a failed IOKit completion
retains the buffer and can retry without pretending its VM mapping is live.

## Optional actual pmap proof

`dre_gpu_nc_pmap.inc` is source intended for the **end of the actual XNU
`osfmk/arm/pmap/pmap.c` translation unit**. It uses that file's real private
geometry and shared pmap lock functions, not fixed guessed offsets. No staging
change enables it. An optional native profile must copy this include into that
directory, append its include under `#if defined(DRE)`, and build the binding
with `DRE_GPU_NATIVE_PMAP_PROOF=1`. Both parts must be linked together; there
is no weak-success fallback when the symbols are absent.

The proof reads the actual MAIR byte (Normal Inner/Outer NC, `0x44`), leaf
AttrIndx, output PA, kernel/user access permissions and execute-never bits. It
checks each 16 KiB CPU PTE against its four expected 4 KiB GPU PAs, managed
`VM_WIMG_WCOMB` state, and the actual physical-aperture alias. The pinned pmap
implementation updates this alias and managed PV aliases when changing cache
attributes. A WB direct alias is rejected. Locks cover one CPU page at a time;
no allocation, sleep or external callback occurs while holding them. The task
proof obtains an actual `vm_map` reference across pmap lookup and releases it.
An unsupported PPL/physical-aperture scheme, block mapping, page granule,
cache type or permission layout refuses proof. No PTE/cache/TLB is modified.

These are read-only proofs under an actual owned descriptor/lease lifetime;
they do not perform GPU cache flushes or prove device DMA completion.

Fence state comes from the native hardware query callback, never a software
submission counter. A terminal CP/ring status alone does not release memory:
the provider must prove cache, firmware-reference and DMA retirement, or a
real reset that ended access. Completion is kept pending for users until that
retirement succeeds. A published submission error still returns a fence and
retains its resources. FenceClose returns BUSY while pending or referenced by
a dependent job. A failed resource/TLB retirement never frees BO memory.

## Integration contract

`dre_gpu_uapi.h` is ABI version 1 shared with `gpu/vulkan/**`. Every request has
an exact structure length, version, connection generation and zero reserved
fields. Requests contain opaque handles, lengths and BO offsets; callers
cannot choose PA or IOVA. CPUMap returns an explicit mapping lease and task
address, not an IOConnectMapMemory token. Complete request/reply structures
fit the normal IOKit struct-message path, without indirect user arrays.

`dre_gpu_provider.h` is kernel-only. The native integration owner must:

1. Keep the immutable original FDT and all callback contexts resident.
2. Acquire a real exclusive native reset/IRQ/source/RPM/SMMU lease. Refuse
   inherited Android activity, unknown APSS rail votes and unqualified clocks.
3. Supply `configure` with the shared startup object so real RBBM/CP provider
   adapters can call the coordinator's live checks through one native cookie.
   The base `owns` predicate must remain nonrecursive.
4. Supply actual owned page-table/firmware allocations, private firmware
   digest/PAS13 authentication, cache ownership, task VM and submission/fence
   providers. Forward/combine the allocator's actual ownership hooks in VM
   callbacks so newly allocated private metadata is recognized. The optional
   exclusive VM adapter covers basic BO map/unmap only. Physical addresses and
   firmware never cross the user ABI.
5. Keep `submit`, task isolation or cache functions absent when unimplemented.
   Service existence and marker completion must not enable raw submission.
6. Build the binding and actual module sources in an explicit optional kernel
   profile and call `dre_gpu_native_publish` from an owned native platform
   attach path. This directory changes neither staging nor workflow defaults.

The source uses the configured XNU IOKit APIs already used by this port's
SweetSurface implementation. Cache option mapping is grounded in XNU
`iokit/Kernel/IOMemoryDescriptor.cpp` (`kIOWriteCombineCache` maps to
`VM_WIMG_WCOMB`), with actual alias proof still required. Resource bounds and
interrupt values are the pinned full Holi `holi-gpu.dtsi`/`holi.dtsi` values,
not an Apple GPU model. See the platform source pin manifest one directory up.

## Remaining blockers

The concrete native platform ownership/rail/source/SCM/IRQ binding is not
finished. The new basic BO adapter and pmap proof are unvalidated. Neither an
isolated untrusted task GPU VM/PM4 validator nor a real
GPU cache/fence provider is supplied here. Persistent CPU mapping is absent.
No native shader submission, IOSurface sharing/presentation, Vulkan driver
execution or iOS Metal API acceptance has passed. Compilation and all further
validation are intentionally deferred until the user resumes validation.
