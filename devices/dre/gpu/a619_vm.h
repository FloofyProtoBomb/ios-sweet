/* SPDX-License-Identifier: MIT */
#pragma once
#include "a619_smmu.h"
#ifdef __cplusplus
extern "C" {
#endif
struct dre_a619_vm_ops {
    struct dre_a619_smmu_ops mmu;
    /* Prove pinned, exclusively owned, writable kernel bookkeeping storage.
     * The table CPU mapping must additionally match owned table_pa and be NC. */
    int (*owns_cpu)(void *cookie,const void *address,size_t bytes);
    int (*table_mapping)(void *cookie,uint64_t pa,const void *cpu,size_t bytes);
    /* Caller holds the domain/submit/BO lifetime locks throughout each API
     * call. Prove scheduling/submission is blocked, all jobs/firmware DMA are
     * drained, required GPU cache clean/invalidate has actually completed,
     * and power/clock/domain providers remain owned. This callback
     * is complemented by actual GPU idle/fault and active CB/PTE checks. */
    int (*quiescent)(void *cookie);
};
struct dre_a619_vm_mapping {
    uint64_t generation,lease;
    uint32_t first_page,page_count;
    int retired; /* true only after no published PTE or completed TLB removal */
};
struct dre_a619_vm {
    struct dre_a619_smmu *domain;
    struct dre_a619_smmu_page *pages;
    uint64_t *leases,*reverse_pa;
    uint32_t reverse_entries; /* exactly twice the power-of-two window pages */
    uint64_t generation,next_lease;
    uint32_t mapped_pages,mappings;
    int initialized,unsafe;
    /* Immutable ownership snapshot, not caller-supplied readiness flags. */
    struct dre_a619_smmu *owned_domain;
    struct dre_a619_smmu_page *owned_pages;
    uint64_t *owned_leases,*owned_reverse,*owned_tables;
    uint64_t owned_table_pa,owned_va;
    uint32_t owned_pages_count;
    size_t owned_table_bytes;
};
/* Configure a fresh power-of-two 4..256 MiB VA window with all L3 tables
 * allocated/pinned upfront. Metadata/reverse/lease arrays are caller-owned
 * wired allocations. This is BO mapping/lifetime bookkeeping, not an allocator
 * or scheduler, and it performs no BO data-cache maintenance. Preparation
 * and pre-activation mapping touch CPU memory only. */
const char *dre_a619_vm_prepare(struct dre_a619_vm *vm,
    const struct dre_a619_vm_ops *ops);
/* Range must be empty; PA pages are real owned allocations, no W+X, no table
 * overlap or physical alias anywhere in this VM. Replacements require an
 * explicit successful unmap/retirement first. A handle is populated before
 * any GPU publication so uncertain failures retain the entire new BO too. */
const char *dre_a619_vm_map(struct dre_a619_vm *vm,
    const struct dre_a619_vm_ops *ops,uint32_t first,
    const struct dre_a619_smmu_page *pages,uint32_t count,
    struct dre_a619_vm_mapping *mapping);
const char *dre_a619_vm_activate(struct dre_a619_vm *vm,
    const struct dre_a619_vm_ops *ops);
/* A stale/partial/wrong-lease handle is rejected. Once active, invalidates
 * every leaf PTE, barriers and completes real ASID/context TLB synchronization
 * before clearing metadata or returning retired=true. Until that success,
 * no old or new physical page may be freed/reused. Failure requires reset. */
const char *dre_a619_vm_unmap(struct dre_a619_vm *vm,
    const struct dre_a619_vm_ops *ops,struct dre_a619_vm_mapping *mapping);
/* Only an empty VM can detach its owned context. Table/metadata allocations
 * remain pinned until successful CB disable/TLB/context restoration. */
const char *dre_a619_vm_release(struct dre_a619_vm *vm,
    const struct dre_a619_vm_ops *ops);
#ifdef __cplusplus
}
#endif
