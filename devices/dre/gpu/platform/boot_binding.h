/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only callback binding. */
#ifndef DRE_GPU_BOOT_BINDING_H
#define DRE_GPU_BOOT_BINDING_H
#include "io_router.h"
#include "boot_memory.h"
#include "../firmware/native.h"
#include "../kernel/trusted_queue.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Genuine external domain reservation. Exactboolean1 callbacks, never DT or
 * future prerequisite flags. The opaque token/metadata remain held on every
 * partial failure; this binding creates/releases no domain reservation. */
struct dre_gpu_boot_domain_ops {
    void *cookie;
    uint64_t reservation_lease;
    /* Actual unused nonsecure CB/SMR/ASID reservation and exclusion of other
     * global/stream owners, held throughout the native lifecycle. No power,
     * global initialization or future table-NC claim is implied. */
    int (*reserved)(void *,uint64_t common_lease,uint64_t reservation_lease,
                     const struct dre_a619_smmu *);
    /* Additionally actual powered, initialized global/TBU/domain ownership.
     * No cold-idle requirement here: CP may genuinely be executing later. */
    int (*live)(void *,uint64_t common_lease,uint64_t reservation_lease,
                 const struct dre_a619_smmu *);
    /* Actual scheduling exclusion, job/firmware/DMA retirement and completed
     * required GPU-cache operations, not only RPTR==WPTR or TLB completion. */
    int (*quiescent)(void *,uint64_t common_lease,uint64_t reservation_lease,
                      const struct dre_a619_smmu *);
};
/* Caller allocates BOTH this object and boot_memory in actual private wired
 * allocator-owned storage. No IOMalloc/range exception is invented for them.
 * Router is resident with router.context=THIS EXACT binding; outer policy
 * context is retained through the real allocator/domain/firmware cookies.
 */
struct dre_gpu_boot_binding {
    struct dre_gpu_io_router *router;
    struct dre_gpu_native_allocator allocator;
    struct dre_gpu_boot_domain_ops domain;
    struct dre_a619_smmu reservation;
    struct dre_gpu_fw_native *firmware;
    struct dre_gpu_fw_native_ops firmware_ops;
    struct dre_a619_startup *startup;
    struct dre_gpu_boot_memory *memory;
    struct dre_gpu_trusted_queue *private_queue;
    struct dre_gpu_trusted_retained_state private_queue_state;
    struct dre_a619_rbbm *rbbm;
    const struct dre_rpm_votes *votes;
    struct dre_a619_vm_ops vm_ops;
    const void *sqe_file;
    size_t sqe_bytes;
    const char *error;
    uint32_t firmware_attempted,firmware_prepared,bound,preparing_cpu;
    uint32_t memory_attempted,memory_prepared,plan_bound,checking_domain;
    uint32_t queue_probe_attempted;
};
/* Owns the ONE actual native PAS-buffer preparation call. May run before
 * router/RPM construction so the known GPU8KiB/IPA shared16KiB mapper refusal
 * occurs before RPM/rail/clock/CP stores. Exact native status is returned.
 * No external "prepared" bool is accepted and no refusal is bypassed.
 */
const char *dre_gpu_boot_binding_firmware_prepare(struct dre_gpu_boot_binding *,
    struct dre_gpu_fw_native *,const struct dre_gpu_fw_native_ops *);
/* Actual firmware preparation above must have succeeded. Fresh binding only;
 * supplied domain prerequisite state must describe acquired CB+ASID only (36).
 * Real NC tables and actual power/global proof are established at their phases.
 * Status0 success; missing policy/control/allocator proof is an error. */
int dre_gpu_boot_binding_init(struct dre_gpu_boot_binding *,struct dre_gpu_io_router *,
    const struct dre_gpu_native_allocator *,struct dre_a619_startup *,
    struct dre_gpu_boot_memory *,const struct dre_a619_smmu *,
    const struct dre_gpu_boot_domain_ops *);
/* Actual CPU-only private allocation, table construction and staged CP maps.
 * It calls boot_memory_prepare, then trusted_prepare immediately afterward,
 * exactly once in one private synchronous CPU-only scope. The latter uses
 * &memory->ops, the exact immutable table later bound to startup. Only inside
 * these two known helpers does the MMU guard select reserved-only ownership;
 * afterward live is needed. No public mode setter or external scoped callback
 * exists. The first allocation's wired journal and any safely established
 * partial private_queue pointer are retained on failure.
 * No power sequence, domain activation, SCM authentication or startup runs. */
const char *dre_gpu_boot_binding_prepare(struct dre_gpu_boot_binding *,uint32_t window_pages);
int dre_gpu_boot_binding_vm_ops(struct dre_gpu_boot_binding *,struct dre_a619_vm_ops *);
/* Wire actual VM/CP/firmware/coordinator callbacks onto one router cookie.
 * Existing votes/power/RBBM and true external policy remain supplied by caller.
 * This does not call startup_begin. All partial allocations remain held.
 * Success statuses are BOOT_BINDING_CPU_STAGED and BOOT_BINDING_PLAN_BOUND,
 * respectively. They describe actual preparation only, not GPU/Metal ready. */
const char *dre_gpu_boot_binding_plan(struct dre_gpu_boot_binding *,struct dre_a619_start_plan *);
/* Mandatory coordinator DOMAIN-phase callback: actual reserved identity,
 * real live+quiescent provider and real NC table prefix, after actual power/
 * SPTPRAC/RBBM acquisition. It NEVER writes prerequisite flags. */
int dre_gpu_boot_binding_qualify_domain(void *router_cookie,const struct dre_a619_smmu *);
/* Once-only, explicit kernel probe after cold-start marker completion.
 * Uses the real private ring and bounded COPY32 template. Caller must hold
 * the common owner lock. This never accepts user addresses or sets RAW. */
const char *dre_gpu_boot_binding_probe_submission(struct dre_gpu_boot_binding *,
    uint64_t budget_ticks);
#ifdef __cplusplus
}
#endif
#endif
