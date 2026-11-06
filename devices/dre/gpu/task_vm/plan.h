/* SPDX-License-Identifier: MIT -- UNVALIDATED private NON-INSTALLABLE plan. */
#ifndef DRE_TASKVM_PLAN_H
#define DRE_TASKVM_PLAN_H
#include <stddef.h>
#include <stdint.h>
#include "permissions.h"
#include "../a619_smmu.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_TASKVM_MAX_ALLOCATIONS 128u
#define DRE_TASKVM_MAX_CATALOG_PAGES 65536u
/* Immutable allocator-registry PURPOSES, not caller-chosen protection flags.
 * A GPU VA range, public BO access mask or supplied enum proves no purpose.
 */
enum dre_taskvm_role {
    DRE_TASKVM_TASK_DATA=1,DRE_TASKVM_TASK_COMMAND_SNAPSHOT,
    DRE_TASKVM_BOOT_RING,DRE_TASKVM_ANCHORED_SQE,
    DRE_TASKVM_BOOT_SCRATCH,DRE_TASKVM_POWERUP_REGISTERS,
    DRE_TASKVM_PRIVATE_COMPLETION,DRE_TASKVM_CPU_BOOKKEEPING,
    DRE_TASKVM_TRANSLATION_TABLE,DRE_TASKVM_OTHER_PRIVATE
};
struct dre_taskvm_allocation {
    const void *allocation; /* actual private registry object, never user handle */
    uint64_t allocation_generation,pin_lease,owner_task;
    enum dre_taskvm_role role;
    uint32_t gpu_access; /* authoritative granted READ/WRITE/EXEC bits */
    const struct dre_a619_smmu_page *pages;
    /* FULL real CPU allocation, incl unmapped/padding4K pages. Whole16KiB
     * ownership is required; partial CPU-granule privacy cannot be assumed. */
    uint32_t page_count,mapped_prefix_pages;
    uint64_t gpu_va;
};
struct dre_taskvm_catalog {
    uint64_t common_lease,reset_epoch,task_identity,catalog_generation;
    uint64_t va_base;
    uint32_t window_pages,allocation_count;
    const struct dre_taskvm_allocation *allocations;
};
struct dre_taskvm_proof_ops {
    void *cookie;
    /* EXACT1 actual board/VM/registry mutex ownership. No lock acquired here. */
    int (*owner_held)(void *);
    /* EXACT1 only for the real complete immutable private-allocation catalog
     * and task-owned reservations. Include ALL control/table/queue/firmware/
     * reserved/private backing and padding which a task must never alias.
     * A range whitelist, caller role enum or "bootstrap protected" flag cannot
     * satisfy this. Absent actual registry/lifetime implementation refuses. */
    int (*catalog_current)(void *,const struct dre_taskvm_catalog *);
    /* Resolve allocation object+generation+pin in that ACTUAL registry and
     * verify every supplied role/owner/access/page/VA field against it. This
     * must also hold real NC CPU/PTE/cache attributes and reservation lifetime.
     * Never make the caller-provided descriptor its own ownership authority. */
    int (*allocation_current)(void *,const struct dre_taskvm_catalog *,
                               const struct dre_taskvm_allocation *);
    /* Real PRIVATE owned wired CPU scratch/output metadata, no task CPU/GPU
     * aliases or live DMA. Not a guessed KVA range or any user-visible BO
     * which merely happens to have a kernel mapping. */
    int (*owns_cpu)(void *,const void *,size_t);
};
struct dre_taskvm_row {
    uint64_t va,pa,proposed_leaf,allocation_generation,pin_lease;
    uint32_t allocation_index,page_index;
    enum dre_taskvm_role role;
    uint32_t reserved;
};
struct dre_taskvm_workspace {
    uint64_t *sorted_pa;uint32_t pa_capacity;
    struct dre_taskvm_row *rows;uint32_t row_capacity;
};
struct dre_taskvm_proposal {
    uint64_t common_lease,reset_epoch,task_identity,catalog_generation;
    uint32_t row_count,excluded_pages;
};
/* Read-only actual original-Holi identity + actual pinned catalog inspection.
 * Does not allocate/pin, write PTE/registers, switch contexts, alter public
 * permissions, prove privileged transactions or authorize any user stream.
 * Exact TASKVM_CPU_PERMISSION_PROPOSAL_ONLY means arithmetic/inventory only.
 * Caller keeps every input reference pinned/stable through the whole call.
 */
const char *dre_taskvm_build_proposal(const struct dre_taskvm_proof_ops *,
    const struct dre_taskvm_catalog *,struct dre_taskvm_workspace *,
    struct dre_taskvm_proposal *);

struct dre_taskvm_table_ops {
    void *cookie;
    /* EXACT1 for this private CPU mapping of the full NC table allocation and
     * fresh ASID reservation under the catalog owner. The table must be
     * unpublished, writable only by the owner and inaccessible to GPU DMA. */
    int (*owns_unpublished)(void *,const struct dre_taskvm_catalog *,
        const struct dre_a619_smmu_view *,const uint64_t *);
    void (*barrier)(void *);
};
/* Build an unpublished SID0 table from the complete live allocation catalog.
 * The caller keeps the catalog and reservations pinned under the same lock.
 * Success does not prove privilege behavior or authorize any command stream. */
const char *dre_taskvm_stage_table(const struct dre_taskvm_proof_ops *,
    const struct dre_taskvm_catalog *,struct dre_taskvm_workspace *,
    struct dre_taskvm_proposal *,const struct dre_taskvm_table_ops *,
    const struct dre_a619_smmu_view *,uint64_t *table);
#ifdef __cplusplus
}
#endif
#endif
