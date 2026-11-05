/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight native source.
 * PRIVATE KERNEL DIAGNOSTIC API. Not an IOUserClient/raw PM4 transport. */
#ifndef DRE_GPU_TRUSTED_QUEUE_H
#define DRE_GPU_TRUSTED_QUEUE_H
#include "dre_gpu_provider.h"
#ifdef __cplusplus
extern "C" {
#endif
struct dre_gpu_trusted_queue;
/* Caller-owned wired first-allocation journal. An invalid/partial initial
 * buffer cannot safely contain the opaque queue yet, but its actual allocator
 * handle/view must still escape to the resident owner before verification.
 * This record grants no reference retirement or allocation-free authority. */
struct dre_gpu_trusted_retained_state {
    struct dre_gpu_native_allocation *allocation;
    struct dre_gpu_native_memory memory;
};
enum dre_gpu_trusted_operation {
    DRE_GPU_TRUSTED_WRITE32=1,
    DRE_GPU_TRUSTED_COPY32=2,
    DRE_GPU_TRUSTED_UCHE_CACHE_ONLY=3
};
enum dre_gpu_trusted_state {
    DRE_GPU_TRUSTED_PENDING=1,DRE_GPU_TRUSTED_COMPLETE=2,
    DRE_GPU_TRUSTED_ERROR_RETAINED=3
};
enum dre_gpu_trusted_cache_scope {
    DRE_GPU_TRUSTED_CACHE_NONE=0,
    DRE_GPU_TRUSTED_UCHE_FLUSH_INVALIDATE=1
};
struct dre_gpu_trusted_completion {
    uint64_t ticket;
    uint32_t state,operation,cache_scope,hardware_sequence;
    uint32_t rptr,wptr,where_am_i,status,irq,fault;
    uint32_t ib1_remaining,ib2_remaining,sds_remaining,mrb_remaining;
    uint32_t roq_ib1,roq_ib2,roq_sds,roq_mrb;
    uint32_t observed_value,expected_value,engine_references_drained,reserved;
    /* Engine-reference retirement is NOT PTE/TLB/mapping/allocation free.
     * These private buffers remain mapped and pinned for the boot lifetime. */
};
/* Must be called by native configure AFTER real boot_memory preparation and
 * BEFORE startup activates its VM or the exclusive task adapter snapshots
 * bootstrap occupancy. Allocates actual private kernel state, GPU-RO/XN command
 * buffer and RW completion/data page through the real native allocator,
 * and stages genuine owned VM mappings. The fresh actual wired journal records
 * the initial allocator output before verification. Later partial failures
 * return the actual opaque object when its storage was safely established.
 * The same actual native owner/SCM/VM lock covers every call below. */
const char *dre_gpu_trusted_prepare(const struct dre_gpu_native_allocator *,
    struct dre_a619_vm *,const struct dre_a619_vm_ops *,uint64_t first_unused_gpu_va,
    struct dre_gpu_trusted_retained_state *,
    struct dre_gpu_trusted_queue **);
/* After actual startup marker confirmation. Requires exactly that cold-start
 * history and takes exclusive ownership of its ring submission sequence. */
const char *dre_gpu_trusted_attach(struct dre_gpu_trusted_queue *,struct dre_a619_startup *);
/* No API accepts command bytes, a user BO/surface token, user GPU VA, compiler
 * output or a packet-validation boolean. Kernel builders author ONLY the
 * fixed private CP write/copy/NOP templates. Numeric value is plain data.
 * All addresses, sizes and resource mappings come from private construction.
 * Commands are immutable from seal/publication through actual retirement.
 * Set published before an uncertain WPTR store. ANY nonzero ticket returned
 * with failure remains held, including an owner loss after ring CPU stores
 * but before the WPTR attempt. Private tickets are NOT UAPI fences. */
const char *dre_gpu_trusted_submit(struct dre_gpu_trusted_queue *,
    enum dre_gpu_trusted_operation,uint32_t value,uint64_t *ticket,uint32_t *published);
const char *dre_gpu_trusted_query(struct dre_gpu_trusted_queue *,uint64_t ticket,
    struct dre_gpu_trusted_completion *);
const char *dre_gpu_trusted_wait(struct dre_gpu_trusted_queue *,uint64_t ticket,
    uint64_t budget_ticks,struct dre_gpu_trusted_completion *);
/* Completed ticket only. It permits the fixed command/data pages to be reused
 * under the same owner lock, but does NOT unmap or free any allocation.
 * A timeout/fault leaves the ticket, mappings and entire owner quarantined. */
const char *dre_gpu_trusted_close(struct dre_gpu_trusted_queue *,uint64_t ticket);
#ifdef __cplusplus
}
#endif
#endif
