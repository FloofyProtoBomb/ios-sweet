# Native GPU boot memory source

**UNVALIDATED overnight implementation.** No allocation, staging, compilation,
test or hardware operation has been executed for this source.

`boot_memory.c` connects the existing native XNU allocation registry to the
actual A619 VM, SMMU and CP structures. It prepares real storage rather than
accepting CPU/physical addresses or readiness masks from userspace. The native
owner calls it during `configure`, before startup activates the domain.

The native owner must allocate its zeroed private `dre_gpu_boot_memory` as
wired kernel storage recognized by its real `owns_cpu` callback. It supplies
the actual allocator, unified serialized MMIO/VM operations, an exclusively
reserved CB/SMR/ASID and global-SMMU owner, and the full immutable SQE file
already authenticated against the private kernel anchor. Firmware parsing here
checks the actual prefix/length again; CP startup separately requires the
actual full-file identity proof. This helper does not acquire those providers.

The input reservation carries only the actual CB/ASID reservation facts (36).
After proving its fresh NC table allocation, preparation adds only TABLES_NC
(44). It makes no powered/global-initialization/quiescent promise. Startup's
mandatory `qualify_domain` runs after actual power/SPTPRAC/RBBM and requires
real live-domain/quiescent/reservation/table proofs before deriving the other
existing prerequisites and activating hardware. CPU-only preparation needs a
real reservation/control guard, with no speculative pre-power register read.
No hidden second power sequence in configure is required or permitted.

The helper allocates a physically contiguous NC table prefix, separate wired
page/lease/reverse arrays, and actual NC ring/SQE/scratch/powerup/result buffers.
The domain has an explicitly sized power-of-two 4..256 MiB VA window. Bootstrap
buffers occupy only the first 2 MiB block; 16 KiB CPU granules do not mix the
RX SQE mapping with RW buffers. GPU padding remains unmapped. Table/PA/CPU
ownership is checked through the actual allocator registry, and maps are made
by `dre_a619_vm_prepare/map`, retaining their real generation/lease handles.
The ring is GPU-readonly/XN while its exclusively owned NC CPU alias remains
writable. Ordinary immutable IB commands are likewise GPU-readonly/XN; SMMU
execute permission belongs to actual SQE UCODE. Holi lacks ADRENO_APRIV, so no
newer ring-privilege default is copied. Privileged scratch/powerup isolation
and a true task view remain separately unqualified; RAW stays disabled.

`dre_gpu_boot_table_mapping` proves the exact owned contiguous table prefix.
`dre_gpu_boot_cp_mapping` checks actual live SMMU state, original mapping
identity, PA, lease, permissions and actual leaf PTEs for each CP buffer. The
real native owner forwards its existing proof callbacks to these functions
under its lifetime lock. `dre_gpu_boot_memory_bind` installs only the genuine
prepared pointers and MMU operations in the startup plan; policy, power,
IRQ/reset, RBBM and authenticated PAS callbacks remain mandatory.

Before activation, the private trusted-queue helper may stage its own actual
immutable IB and NC completion pages at or above `first_unused_gpu_va`. Both
sets of mappings must precede the exclusive-task adapter's actual bootstrap
snapshot. That adapter protects their populated 2 MiB blocks and never enables
untrusted RAW submission. Boot memory preparation does not submit a command,
publish a Vulkan/Metal device, or qualify shader caches and task isolation.

All allocations, including a partially returned allocator handle on failure,
remain explicitly recorded. This helper provides no automatic free or unsafe
rollback. The owner may discard never-published allocations only while actual
native ownership proves no device could reference them. Once startup can
publish registers or DMA addresses, resources remain held through failure and
require actual retirement/reset proof before reuse. Intended allocation and
alias assumptions still require tomorrow's real kernel/API/lifetime checks.

The actual constructor also requires `DRE_GPU_ALLOCATOR_VERSION`, exact
allocator `struct_bytes` and the real status0 `classify`/`record_mapping` callbacks before it
attempts any allocation. After each real allocation passes its
NC/PA/whole-extent/permission proof,
and before any VM mapping or hardware publication, it tags the exact allocator
handle and exact memory view once. The constructor selects purposes from its
own fixed operations: contiguous page tables, CPU-only page/lease/reverse
metadata, read-only ring, executable SQE UCODE, scratch, powerup and private
completion. No CPU address, PA interval, permission combination or later user
claim is used to guess a purpose. Absent API or rejected classification refuses
preparation and retains every partially returned handle; it does not relabel
the allocation or free it. This is purpose metadata, not a cache-retirement,
task-isolation or GPU-readiness proof. Public selectors and RAW remain disabled.

| Actual constructor operation | Immutable registry purpose |
|---|---|
| Physically contiguous NC table allocation | `DRE_GPU_ALLOC_BOOT_TABLES` |
| CPU-only page, lease and reverse arrays | `DRE_GPU_ALLOC_CPU_METADATA` |
| GPU-readonly/XN bootstrap ring | `DRE_GPU_ALLOC_RING` |
| Full SQE UCODE allocation | `DRE_GPU_ALLOC_SQE_UCODE` |
| CP scratch | `DRE_GPU_ALLOC_CP_SCRATCH` |
| CP powerup list | `DRE_GPU_ALLOC_CP_POWERUP` |
| Bootstrap private result storage | `DRE_GPU_ALLOC_PRIVATE_COMPLETION` |

The complete rounded allocation, including unused padding, receives exactly
one purpose. Logical table prefixes or mapped CP subranges do not turn the
remaining bytes into unclassified task storage. Purpose rejection stops before
VM preparation/mapping/publication and keeps the slot's actual handle/view and
held bit, including all prior successfully classified allocations.

After each actual `VM_MAPPING_STAGED` result, the constructor verifies the
returned generation, mapping lease, exact first/count and inactive domain,
then passes the real allocation handle, exact VM, exact VM-ops table and actual
mapping handle to the registry's `record_mapping` hook. The registry derives
any GPUVA/mapped-prefix record from those actual objects, never a supplied
scalar address or slot label. A failed record leaves the genuinely staged
mapping handle and all allocations intact and refuses further preparation.
Tables and CPU-only metadata are classified but never given a GPU BO mapping.
At this stage leaf PTEs are not published: a staged record describes owned CPU
metadata only, and later active use still requires actual live PTE/domain proof.
