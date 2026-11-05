/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source. Single exclusive task BO mapper only.
 * Shared bootstrap SID0 is not an untrusted RAW command address space. */
#ifndef DRE_GPU_VM_BINDING_H
#define DRE_GPU_VM_BINDING_H
#include "dre_gpu_provider.h"
#ifdef __cplusplus
extern "C" {
#endif
struct dre_gpu_vm_entry {
    struct dre_gpu_native_memory memory;
    struct dre_a619_vm_mapping mapping;
    uint64_t gpu_va;
    uint32_t access,held,uncertain,reserved;
};
struct dre_gpu_vm_binding {
    struct dre_a619_startup *startup;
    struct dre_a619_vm *vm;
    const struct dre_a619_vm_ops *ops;
    uint64_t generation,base,table_pa,task_identity,task_generation;
    uint32_t page_count,bootstrap_pages,bootstrap_mappings,entries_live;
    /* Any bootstrap/protected occupancy reserves its WHOLE 2MiB block for
     * the lifetime of this binding. It never becomes a task allocation hole. */
    uint64_t protected_blocks[DRE_A619_SMMU_MAX_PAGES/512/64];
    struct dre_gpu_vm_entry entries[DRE_GPU_MAX_BOS];
    uint32_t initialized,unsafe;
};
struct dre_gpu_vm_reserved { uint64_t gpu_va,bytes; };
/* Storage must be wired, private kernel memory recognized by ops->owns_cpu.
 * Holds the same outer lifetime lock as startup/submission on EVERY call.
 * Additional protected regions are native-owned, not supplied by userspace. */
int dre_gpu_vm_binding_init(struct dre_gpu_vm_binding *,struct dre_a619_startup *,
    const struct dre_gpu_vm_reserved *,uint32_t reserved_count);
int dre_gpu_vm_binding_live(struct dre_gpu_vm_binding *);
int dre_gpu_vm_binding_open(struct dre_gpu_vm_binding *,uint64_t task_identity,
    uint64_t connection_generation);
int dre_gpu_vm_binding_map(struct dre_gpu_vm_binding *,uint64_t task_identity,
    uint64_t connection_generation,const struct dre_gpu_native_memory *,uint32_t,
    uint64_t *gpu_va,uint64_t *mapping_lease);
int dre_gpu_vm_binding_retire(struct dre_gpu_vm_binding *,uint64_t task_identity,
    uint64_t connection_generation,const struct dre_gpu_native_bo *);
int dre_gpu_vm_binding_close(struct dre_gpu_vm_binding *,uint64_t task_identity,
    uint64_t connection_generation);
#ifdef __cplusplus
}
#endif
#endif
