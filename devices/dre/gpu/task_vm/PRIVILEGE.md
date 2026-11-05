# A619 task permissions — UNVALIDATED, non-installable

These new sources perform private CPU descriptor arithmetic and bounded
allocation-inventory planning. They write **no** PTE, register, ring or
transaction mode. They do not create a task context, admit an IB/shader or
enable RAW/Metal/Vulkan capability. No tests, compilation, recipes, emulator,
CI or phone operations ran for this source.

The current native single-SID0 VM and its public1/2/4 access model cannot prove
task isolation. Current page descriptors use user-accessible AP bits. A GPU VA
reservation, a caller's role label or a locally set protection flag cannot
establish privileged transactions or protected physical backing.

Grounded distinctions:

| Actual pinned source | Meaning for this proposal |
| --- | --- |
| `adreno-gpulist.h`, `adreno_gpu_core_a619_variant` | Holi advertises PREEMPTION and CONTENT_PROTECTION, **not ADRENO_APRIV**. Do not copy newer A6xx APRIV defaults. |
| `adreno_ringbuffer.c`, ring allocation | Ring is GPU-readonly; privilege is added only for APRIV targets. Holi ring proposal remains user-readable, readonly and XN. CPU's owned NC alias can remain writable. |
| `adreno.c`, `adreno_load_firmware` | SQE is GPU-readonly UCODE. This is the actual anchored executable-fetch role, distinct from a command BO. |
| `kgsl_iommu.c`, `_get_protection_flags` | Default maps include NOEXEC. GPUREADONLY removes write; PRIVILEGED adds IOMMU_PRIV; only UCODE removes NOEXEC. An ordinary immutable IB is readonly **XN**, not executable merely because it holds packets. |
| `io-pgtable-arm.c`, `arm_lpae_prot_to_pte` | Stage1 AP00 is privileged RW, AP10 privileged RO, AP01 ordinary RW, AP11 ordinary RO. The new encoder separates this private class from public BO access/command immutability. |
| `adreno_ringbuffer.c`, scratch; `adreno_a6xx.c`, powerup register list | These use privileged allocations. Candidate task-view leaves are privileged-only RW+XN, pending real end-to-end transaction proof. |
| `adreno_ringbuffer.c`, user IB construction | IB_PRIV bit20 is explicitly excluded. Register protected mode is separately restored for user execution. Neither control is equivalent to SMMU security/PTE ownership. |

The native S2CR currently selects translated SID0 with default PRIVCFG. Pinned
`arm-smmu.c` also selects `S2CR_PRIVCFG_DEFAULT`. The architecture permits a
stream override of privilege independently from page AP: forcing every
transaction privileged would defeat the intended privileged-page boundary.
Default/zero alone does not prove what each A619 bus transaction actually
asserts. Global/TBU/context authority and incoming shader/IB/CP/SQE attributes
still require genuine qualified implementation and later hardware evidence.

The private completion case remains unresolved. Pinned non-APRIV memstore is
ordinary GPU-RW, which cannot justify a trusted shared retirement page against
arbitrary shader stores. This proposal therefore excludes private completion,
translation tables, CPU bookkeeping and other private backing from the task
leaf view. Exclusion is not a working context switch: every outstanding CP,
firmware, SQE, shadow and queue reference must be accounted for before using a
task table. This module deliberately supplies no install/switch API.

`permissions.c` only encodes explicit Stage1 descriptor bits using the existing
native 4KiB, NS/AF/outer-shareable/nG, MAIR-index0 shape. It rejects W+X. The
actual MAIR, TCR, privilege/security and CPU cache/alias state are independent
requirements; computing a descriptor does not certify any of them.

`plan.c` obtains the existing boot-owned original Holi identity and requires a
complete immutable **actual allocation registry** snapshot. Allocation purpose,
task identity, generation, pin lease, granted access, full physical page list
and reserved GPU VA must resolve to genuine private records. The new private
allocator catalog supplies immutable purposes, real registered-client/task/
connection generations and recorded mapping handles for its actual descriptor
data and padding. This is explicitly data-only coverage: heap metadata's CPU
granules and separate firmware/reserved backing are not a complete protected
physical inventory. The registry binding therefore refuses the planner's
stronger requirement. A catalog label cannot close that ownership gap.

The planner verifies all input page-list/output ranges before stores, rejects
mixed16KiB CPU-granule ownership, and includes every allocation's padding and
task-excluded private pages in its physical-alias check. It bounds128 objects
and65536 total catalog pages, sorts physical pages in O(n log n), and checks
at most128² allocation VA intervals. These are planning limits, not advertised
GPU memory capacity. The exact success string
`TASKVM_CPU_PERMISSION_PROPOSAL_ONLY` still means **non-installed arithmetic**.
Rows have no authorization path into the current VM or native user client.

Required shared primitives before any future broadening:

1. Separate actual PTE privilege and SQE instruction fetch from public BO
   access/immutable-command policy. All AP encoders and comparisons must agree:
   SMMU build/live checks, VM map/unmap checks, boot buffer mapping proof and
   exclusive binding. Do not silently assign a new meaning to publicbit4.
2. Extend the real immutable purpose/task-generation/pin registry beyond its
   current descriptor-data scope to **all** protected backing, not only mapped
   VAs or known boot slots. This needs genuine metadata/firmware lifetime and
   physical/CPU-granule authority, rather than a new complete-coverage flag.
   A task BO/import cannot alias table/control/ring/queue/firmware pages or a
   different slice of their16KiB CPU granule.
3. Prove actual owned SID/SMR/CB/ASID and unmodified incoming privilege/security
   semantics. No forced-privileged S2CR, unsupported APRIV write or reused live
   global stream owner is an acceptable shortcut.
4. Implement genuine serialized context/TTBR/ASID switching with GPU/cache/
   firmware reference retirement and real TLB synchronization, keeping every
   bootstrap consumer mapped with correct permissions. Current global SID0
   bookkeeping does not provide a per-task execution context.
5. Validate immutable root/nested/indirect command snapshots and every address
   source; forbid IB_PRIV20, protected-mode disables, privileged register/MMU/
   ring/secure/context operations and unknown packet paths. Existing narrow
   structural PM4 intake is incomplete and keeps raw authorization disabled.
6. Establish an actual hardware-confirmed retirement path whose completion
   storage untrusted shaders cannot forge. No timestamp, RPTR equality, PTE
   change or proposed privilege bit alone authorizes freeing live resources.

Primary references, pinned source read only:

- [Holi GPU variants](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno-gpulist.h)
- [KGSL IOMMU protection conversion](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/kgsl_iommu.c)
- [Ring/IB and scratch policy](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_ringbuffer.c)
- [Firmware allocation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno.c)
- [A6xx register protection and powerup buffer](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/gpu/msm/adreno_a6xx.c)
- [Stage1 descriptor conversion](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/iommu/io-pgtable-arm.c)
- [S2CR selection](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/iommu/arm-smmu.c)
- [Arm SMMUv2 architecture, IHI0062D.c](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb), S2CR privilege override and translation permission distinctions.
