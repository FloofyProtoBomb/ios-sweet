/* SPDX-License-Identifier: MIT
 * UNVALIDATED source draft. Mapping lifetime is not a CPU access epoch.
 * No tests/compiles/native execution. This private state machine publishes no
 * commands/capabilities and cannot establish raw PM4/task protection.
 */
#pragma once
#include "dre_gpu_provider.h"
#ifdef __cplusplus
extern "C" {
#endif
enum dre_gpu_access_phase {
    DRE_GPU_ACCESS_IDLE=1, DRE_GPU_ACCESS_CPU_TRANSFERRING=2,
    DRE_GPU_ACCESS_CPU_OWNED=3, DRE_GPU_ACCESS_GPU_PREPARED=4,
    DRE_GPU_ACCESS_GPU_PUBLISHED=5, DRE_GPU_ACCESS_QUARANTINED=6
};
struct dre_gpu_access_epoch {
    uint64_t bo,session_generation,gpu_va,gpu_mapping_lease,bytes,reset_generation;
    uint64_t map_lease,task_address,next_epoch,active_epoch,ticket;
    uint32_t granted_access,cpu_access,gpu_access,phase;
    uint32_t initialized,unsafe;
};
struct dre_gpu_access_ops {
    void *cookie;
    /* Proofs return exactly1, never a truthy negative status. */
    int (*owns_memory)(void *,const struct dre_gpu_access_epoch *);
    int (*owns_task_map)(void *,const struct dre_gpu_access_epoch *);
    /* Status callbacks return DRE_GPU_OK only after actual provider operation
     * and completion. Missing cache/ownership callbacks are unsupported, not
     * successful Normal-NC cache no-ops. They must use a real bounded native
     * operation/completion budget under the actual owner lock. No EL0 DC
     * instruction is needed. */
    int (*cache)(void *,uint32_t operation);
    int (*cpu_begin)(void *,uint32_t access);
    int (*cpu_end)(void *,uint32_t access);
    int (*barrier)(void *);
    int (*unmap_actual)(void *); /* actual IOMemoryMap::unmap/release */
    int (*query)(void *,uint64_t ticket,struct dre_gpu_native_completion *);
};
/* Called with the same real BO/job/VM owner lock throughout. The initial
 * fields are actual kernel identities; an already initialized/unsafe object
 * cannot be erased. Persistent pointer remains mapped across IDLE/CPU/GPU
 * epochs, but the application must honor access synchronization. This is not
 * hardware access revocation and cannot make mutable IBs safe. */
int dre_gpu_access_attach(struct dre_gpu_access_epoch *,const struct dre_gpu_access_ops *,
    uint64_t bo,uint64_t generation,uint64_t gpu_va,uint64_t gpu_lease,
    uint64_t bytes,uint32_t granted_access,uint64_t reset_generation,
    uint64_t map_lease,uint64_t task_address);
int dre_gpu_access_cpu_begin(struct dre_gpu_access_epoch *,const struct dre_gpu_access_ops *,
    uint32_t access,uint64_t *epoch);
int dre_gpu_access_cpu_end(struct dre_gpu_access_epoch *,const struct dre_gpu_access_ops *,uint64_t epoch);
int dre_gpu_access_gpu_prepare(struct dre_gpu_access_epoch *,const struct dre_gpu_access_ops *,
    uint32_t access,uint64_t *epoch);
int dre_gpu_access_gpu_publish(struct dre_gpu_access_epoch *,const struct dre_gpu_access_ops *,
    uint64_t epoch,uint64_t actual_ticket);
/* Only the actual native submit returned published0/ticket0 can authorize
 * this operation. A caller readiness flag cannot prove nonpublication. */
int dre_gpu_access_cancel_unpublished(struct dre_gpu_access_epoch *,const struct dre_gpu_access_ops *,uint64_t epoch);
int dre_gpu_access_gpu_retire(struct dre_gpu_access_epoch *,const struct dre_gpu_access_ops *,
    uint64_t epoch,uint64_t actual_ticket);
/* Revokes only CPU map lifetime, retaining any GPU-published epoch/pins. */
int dre_gpu_access_detach(struct dre_gpu_access_epoch *,const struct dre_gpu_access_ops *);
#ifdef __cplusplus
}
#endif
