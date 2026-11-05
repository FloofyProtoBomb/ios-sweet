/* SPDX-License-Identifier: MIT */
/* UNVALIDATED overnight draft. Kernel-only integration contract. */
#ifndef DRE_GPU_PROVIDER_H
#define DRE_GPU_PROVIDER_H
#include "dre_gpu_uapi.h"
#include "../a619_startup.h"
#ifdef __cplusplus
extern "C" {
#endif

#define DRE_GPU_PROVIDER_VERSION 2u
#define DRE_GPU_OWNER_LOCK_VERSION 1u
/* Kernel-private actual serialization, never part of selector ABI1. This
 * mutex is the binding's real BO/VM/submit/client/timer lock. Callers must not
 * recursively enter it or call it from interrupt context. check_held returns
 * status0 only for actual ownership by this task-context thread. */
struct dre_gpu_owner_lock {
    uint32_t version,struct_bytes;
    void *cookie;
    void (*enter)(void *);
    void (*leave)(void *);
    int (*check_held)(void *);
};
enum dre_gpu_missing_provider {
    DRE_GPU_MISSING_NATIVE_LEASE=1ull<<0,
    DRE_GPU_MISSING_POWER_POLICY=1ull<<1,
    DRE_GPU_MISSING_SCM_FIRMWARE=1ull<<2,
    DRE_GPU_MISSING_SMMU_DOMAIN=1ull<<3,
    DRE_GPU_MISSING_TASK_VM=1ull<<4,
    DRE_GPU_MISSING_COMMAND_VALIDATOR=1ull<<5,
    DRE_GPU_MISSING_GPU_CACHE=1ull<<6,
    DRE_GPU_MISSING_HARDWARE_FENCE=1ull<<7,
    DRE_GPU_MISSING_PERSISTENT_MAP=1ull<<8,
    DRE_GPU_MISSING_MARKER=1ull<<9
};
struct dre_gpu_native_io {
    void *cookie;
    int (*read32)(void *,uint64_t,uint32_t *);
    int (*write32)(void *,uint64_t,uint32_t);
    int (*read64)(void *,uint64_t,uint64_t *);
    int (*write64)(void *,uint64_t,uint64_t);
    void (*barrier)(void *);
    uint64_t (*now)(void *); /* 19.2MHz physical counter, qualified at attach */
};
struct dre_gpu_native_allocation; /* kernel-owned wired IOBuffer, never user */
#define DRE_GPU_ALLOCATOR_VERSION 1u
enum dre_gpu_allocation_purpose {
    DRE_GPU_ALLOC_UNKNOWN=0,DRE_GPU_ALLOC_BOOT_TABLES,DRE_GPU_ALLOC_CPU_METADATA,
    DRE_GPU_ALLOC_PRIVATE_CONTROL,DRE_GPU_ALLOC_RING,DRE_GPU_ALLOC_SQE_UCODE,
    DRE_GPU_ALLOC_CP_SCRATCH,DRE_GPU_ALLOC_CP_POWERUP,DRE_GPU_ALLOC_PRIVATE_COMPLETION,
    DRE_GPU_ALLOC_IMMUTABLE_IB,DRE_GPU_ALLOC_TRUSTED_STATE,
    DRE_GPU_ALLOC_CATALOG_SCRATCH, /* private RW CPU output; NEVER GPU-mapped */
    DRE_GPU_ALLOC_TASK_BO=0x100 /* INTERNAL registered IOUserClient path ONLY */
};
struct dre_gpu_allocation_catalog_snapshot;
struct dre_gpu_native_memory {
    void *cpu;
    uint64_t bytes;
    const struct dre_a619_smmu_page *pages;
    uint32_t page_count, cache_contract;
};
/* Allocate owned RAM, with no GPU PTE. All aliases must be Normal-NC and each
 * 4KiB PA must independently match the actual kernel descriptor. Caller may
 * retire an allocation only before publication or after real cache/TLB/DMA
 * retirement. Startup buffers remain owned if startup ever touched hardware. */
struct dre_gpu_native_allocator {
    void *cookie;
    int (*allocate)(void *,uint64_t,uint32_t,
        struct dre_gpu_native_allocation **,struct dre_gpu_native_memory *);
    /* Only unpublished allocations. Published allocations are deliberately
     * not exposed through this free callback; their owner must retain them. */
    void (*discard_unpublished)(void *,struct dre_gpu_native_allocation *);
    /* Actual private allocation registry, not caller-supplied ranges. CPU
     * lookup also includes wired private allocation/page-list metadata so
     * VM ownership callbacks can recognize their new kernel bookkeeping. */
    int (*owns_cpu)(void *,const void *,size_t);
    int (*owns_ram)(void *,uint64_t,size_t);
    int (*verify_memory)(void *,const struct dre_gpu_native_memory *);
    /* Bounded startup tables requiring a physically contiguous PA sequence.
     * Same owned NC proof plus real descriptor-segment/page checks. Ordinary
     * BOs should use allocate; CPU virtual contiguity alone is insufficient. */
    int (*allocate_contiguous)(void *,uint64_t,uint32_t,
        struct dre_gpu_native_allocation **,struct dre_gpu_native_memory *);
    /* UNVALIDATED private extension, no public selector/provider layout change.
     * Immutable kernel purpose, exact once before publication. TASK_BO cannot
     * be selected here: only the real registered client path can assign it. */
    uint32_t version,struct_bytes;
    int (*classify)(void *,struct dre_gpu_native_allocation *,
        const struct dre_gpu_native_memory *,enum dre_gpu_allocation_purpose);
    /* Capture only the actual successful VM map record, never inferred VA. */
    int (*record_mapping)(void *,struct dre_gpu_native_allocation *,
        const struct dre_a619_vm *,const struct dre_a619_vm_ops *,
        const struct dre_a619_vm_mapping *);
    /* Synchronous real owner-mutex-held, retained descriptor/client scope.
     * snapshot storage must be genuinely private-classified owned RAM.
     * All borrowed pointers expire and storage is cleared before return. */
    int (*with_catalog)(void *,const void *registered_client,uint64_t connection_generation,
        struct dre_gpu_allocation_catalog_snapshot *,
        int (*visitor)(void *,const struct dre_gpu_allocation_catalog_snapshot *),void *);
    int (*catalog_current)(void *,const struct dre_gpu_allocation_catalog_snapshot *);
    int (*catalog_record_current)(void *,const struct dre_gpu_allocation_catalog_snapshot *,uint32_t index);
    /* EXACT1 retained kernel-only CPU-range proof. Does not certify whole
     * shared heap granules, reserved firmware/global physical completeness,
     * task isolation or write authority over borrowed registry metadata. */
    int (*owns_private_cpu)(void *,const void *,size_t);
};
struct dre_gpu_native_bo {
    struct dre_gpu_native_memory memory;
    uint64_t gpu_va, mapping_lease;
    uint32_t access, reserved;
};
struct dre_gpu_native_ib {
    const struct dre_gpu_native_bo *bo;
    uint64_t offset,bytes;
};
struct dre_gpu_native_resource {
    const struct dre_gpu_native_bo *bo;
    uint32_t access,reserved;
};
struct dre_gpu_native_submit {
    uint32_t ib_count,resource_count,wait_count,reserved;
    struct dre_gpu_native_ib ibs[DRE_GPU_MAX_IBS];
    struct dre_gpu_native_resource resources[DRE_GPU_MAX_BOS];
    uint64_t wait_tickets[DRE_GPU_MAX_WAITS];
};
struct dre_gpu_native_completion {
    uint32_t state,hardware_error;
    uint64_t reset_generation;
    /* Terminal hardware status alone does NOT authorize unpinning. This is
     * true only after firmware references, DMA and GPU dirty cache lines have
     * actually retired, or a real reset proved no further device access. */
    uint32_t resources_retired,reserved;
};
struct dre_gpu_vm_reserved;
struct dre_gpu_native_provider {
    uint32_t version,struct_bytes;
    void *cookie;
    const void *fdt; /* immutable kernel-owned complete original Holi FDT */
    uint64_t fdt_bytes;
    /* Mapping is not ownership. This callback proves an exclusive native
     * reset/IRQ/source/RPM/SMMU owner and returns a nonzero lease. It must
     * refuse Android/inherited-active DMA and missing rail policy. Partial
     * acquisition that retains resources must also return a nonzero lease;
     * it is retained on failure, never mistaken for an unpublished owner. */
    int (*acquire)(void *,uint64_t *lease);
    /* EXACT boolean1 only for the nonrecursive basic map/lease/reset/fault
     * ownership proof. Zero/negative/status values refuse. Never call
     * startup_check_live from here: all its MMIO goes through this. */
    int (*owns)(void *,uint64_t lease);
    /* Fill real provider ops/buffers, not flags standing in for providers.
     * Binding calls the shared dre_a619_startup_begin exactly once. */
    int (*configure)(void *,uint64_t,const struct dre_gpu_native_io *,
        const struct dre_gpu_native_allocator *,struct dre_a619_startup *,
        struct dre_a619_start_plan *);
    /* Description may run while startup is incomplete. Do not touch powered
     * off GPU registers merely because the base native lease is held. */
    int (*describe)(void *,uint64_t,struct dre_gpu_info_reply *);
    /* Both alias proof callbacks return EXACT1 for a real verified alias.
     * They are not DRE_GPU_OK/status APIs; a negative error is not proof. */
    int (*normal_nc_alias)(void *,const void *cpu,uint64_t bytes,
        const struct dre_a619_smmu_page *,uint32_t pages);
    int (*task_nc_alias)(void *,uint64_t task_identity,uint64_t task_address,
        const struct dre_gpu_native_memory *);
    /* A session is a genuinely isolated task address space / submission
     * context. A globally shared bootstrap SID0 window is insufficient for
     * admitting untrusted RAW_IB_SUBMIT. The task cookie is opaque identity. */
    int (*session_open)(void *,uint64_t,uint64_t task_identity,void **session);
    int (*session_close)(void *,void *session); /* all VM/refs already retired */
    int (*bo_map)(void *,void *,const struct dre_gpu_native_memory *,uint32_t,
        uint64_t *gpu_va,uint64_t *mapping_lease);
    int (*bo_retire)(void *,void *,const struct dre_gpu_native_bo *);
    int (*cpu_begin)(void *,void *,const struct dre_gpu_native_bo *,uint32_t);
    int (*cpu_end)(void *,void *,const struct dre_gpu_native_bo *,uint32_t);
    int (*cache)(void *,void *,const struct dre_gpu_native_bo *,uint64_t,
        uint64_t,uint32_t);
    /* Must snapshot command bytes, validate all nested packet/address uses,
     * enforce the task's VM/protected registers and hold immutable command
     * storage until completion. Never enable this with only a marker helper.
     * published must be set before the first device-visible store. On any
     * published failure return a queryable nonzero ticket, retaining all
     * submitted resources. An uncertain unpublished result is a fatal bug. */
    int (*submit)(void *,void *,const struct dre_gpu_native_submit *,
        uint64_t *ticket,uint32_t *published);
    int (*query)(void *,void *,uint64_t ticket,
        struct dre_gpu_native_completion *);
    /* Releases a terminal, resources_retired completion record only. */
    int (*ticket_close)(void *,void *,uint64_t ticket);
    /* Implementation selection only, never a readiness bit. Opt into the
     * bounded existing-startup-VM adapter. It admits a single task BO owner
     * and ALWAYS disables RAW/persistent mapping/hardware-submit capability.
     * Additional native protected ranges must be private owned metadata. */
    const struct dre_gpu_vm_reserved *vm_reserved;
    uint32_t vm_reserved_count,use_exclusive_vm_adapter;
    /* Mandatory before acquire/configure for this version. Called while the
     * actual owner mutex is held. Copy these callbacks and bind native broker,
     * IRQ worker, SCM/RPM/VM/cache/queue serialization to THIS mutex, not a
     * second advisory lock. Set retained=1 before the first escape of its
     * cookie/reference, including partial failures. Successful binding is
     * boot-lifetime and has no unload path; no hardware ownership is implied.
     * Failure with retained=0 promises no reference or future callback remains.
     * Missing binding refuses hardware acquisition. Returns DRE_GPU status. */
    int (*bind_owner_lock)(void *,const struct dre_gpu_owner_lock *,uint32_t *retained);
};
/* Explicit opt-in only, built-in kernel integration. No default provider is
 * installed. Missing callbacks return unsupported; no working Metal service
 * or IOAccelerator personality is created. The provider and root startup
 * state must remain resident through device lifetime / uncertain failures. */
int dre_gpu_native_publish(const struct dre_gpu_native_provider *);
#ifdef __cplusplus
}
#endif
#endif
