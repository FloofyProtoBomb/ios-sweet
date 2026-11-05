/* SPDX-License-Identifier: MIT -- private SCM/PAS transport, never user ABI. */
#ifndef DRE_GPU_SCM_NATIVE_H
#define DRE_GPU_SCM_NATIVE_H
#include "../firmware/native.h"
#include "../kernel/dre_gpu_provider.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_GPU_SCM_REQUEST_POLICY_VERSION 1u
struct dre_gpu_scm_native;
struct dre_gpu_scm_request_policy {
    uint32_t version,struct_bytes;
    void *cookie;
    /* Status0, genuine acquired request lease (also on partial failure).
     * Prove PAS13/GPU producer exclusion, actual boot-loan ownership, PIL
     * bandwidth/interconnect holds, reset/source lifetime and real security
     * references. This is AP request authority, NOT future EL3 permission or
     * firmware authentication. No invented token/default policy is supplied. */
    int (*acquire)(void *,uint64_t common_lease,const struct dre_gpu_fw_relocation_view *,uint64_t *request_lease);
    int (*check)(void *,uint64_t common_lease,uint64_t request_lease,
        const struct dre_gpu_fw_relocation_view *); /* EXACT1 */
    int (*consumers_retired)(void *,uint64_t common_lease,uint64_t request_lease); /* EXACT1 */
    int (*release)(void *,uint64_t common_lease,uint64_t request_lease); /* status0 */
};
struct dre_gpu_scm_profile { uint32_t clock_mask,scm_node,pil_bus_node; };
/* Only actual generic qcom,scm with absent named clock properties is supported.
 * This qualifies the empty SCM clock set, never PIL interconnect/bus policy. */
const char *dre_gpu_scm_profile_from_fdt(const void *,size_t,struct dre_gpu_scm_profile *);

/* Portable logical engine. All proofs return EXACT1. Transactions are actual
 * registry reservations protected by the XNU adapter's real global mutex.
 * No ordinary mutex may remain held when an API/callback returns. Unknown
 * secure continuations quarantine the logical reservation and retain refs. */
struct dre_gpu_scm_channel_ops {
    void *cookie;
    int (*guard)(void *);
    int (*reserve)(void *);
    int (*reservation_held)(void *);
    int (*finish)(void *,int uncertain); /* EXACT1 committed release/quarantine */
    int (*smc64)(void *,struct dre_a619_smc_regs *);
    void (*barrier)(void *);
    uint64_t (*now)(void *);
};
enum dre_gpu_scm_pas_phase { DRE_SCM_PAS_OFF=0,DRE_SCM_PAS_INIT_PUBLISHED,
    DRE_SCM_PAS_INITIALIZED,DRE_SCM_PAS_MEMORY,DRE_SCM_PAS_AUTHENTICATED };
struct dre_gpu_scm_channel {
    struct dre_gpu_scm_channel_ops ops;
    struct dre_gpu_fw_relocation plan;
    uint64_t calls,continuation,saved[8];
    uint32_t available,command;
    enum dre_gpu_scm_pas_phase phase;
    int bound,discovered,inflight,uncertain;
};
int dre_gpu_scm_channel_init(struct dre_gpu_scm_channel *,const struct dre_gpu_scm_channel_ops *,
    const struct dre_gpu_fw_relocation *);
const char *dre_gpu_scm_channel_discover(struct dre_gpu_scm_channel *);
int dre_gpu_scm_channel_pas_ops(struct dre_gpu_scm_channel *,struct dre_a619_pas13_ops *);
int dre_gpu_scm_channel_discovered(struct dre_gpu_scm_channel *);
int dre_gpu_scm_channel_retirable(struct dre_gpu_scm_channel *);

/* Actual XNU adapter. create observes the original immutable FDT and copies
 * a bounded snapshot from the opaque verified firmware object; no SMC occurs.
 * acquire explicitly obtains genuine policy then performs real discovery.
 * Both require the actual common owner mutex held. Partial state is retained. */
int dre_gpu_scm_native_create(const struct dre_gpu_owner_lock *,uint64_t common_lease,
    struct dre_gpu_fw_native *,const struct dre_gpu_scm_request_policy *,struct dre_gpu_scm_native **);
const char *dre_gpu_scm_native_acquire(struct dre_gpu_scm_native *);
int dre_gpu_scm_native_firmware_ops(struct dre_gpu_scm_native *,struct dre_gpu_fw_native_ops *);
const char *dre_gpu_scm_native_release(struct dre_gpu_scm_native *);
#ifdef __cplusplus
}
#endif
#endif
