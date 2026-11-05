/* SPDX-License-Identifier: MIT
 * UNVALIDATED OVERNIGHT DRAFT: no compilation or execution has been run.
 * This coordinates actual providers; it creates none and publishes no Metal
 * service. The native owner must keep one lifetime lock over every call. */
#pragma once
#include "platform/holi_power.h"
#include "a619_holi_power.h"
#include "a619_rbbm.h"
#include "a619_cp.h"
#include "a619_vm.h"
#ifdef __cplusplus
extern "C" {
#endif

enum dre_a619_start_phase {
    DRE_A619_START_EMPTY=0,
    DRE_A619_START_POWER,
    DRE_A619_START_SPTPRAC,
    DRE_A619_START_RBBM,
    DRE_A619_START_DOMAIN,
    DRE_A619_START_CP,
    DRE_A619_START_MARKER_CONFIRMED,
    DRE_A619_START_FAILED
};
struct dre_a619_start_plan {
    struct dre_rpm_mmio_ops mmio;
    struct dre_a619_ops io;
    struct dre_a619_rbbm_ops rbbm_ops;
    struct dre_a619_smmu_ops smmu_ops;
    struct dre_a619_cp_ops cp_ops;
    const struct dre_rpm_votes *votes;
    struct dre_holi_power_config power_config;
    struct dre_a619_rbbm *rbbm;
    struct dre_a619_smmu *smmu;
    struct dre_a619_cp *cp;
    struct dre_a619_vm *vm;             /* optional real larger BO manager */
    const struct dre_a619_vm_ops *vm_ops;
    uint64_t cp_budget_ticks;            /* checked 19.2-MHz counter, <=1s */
    /* Native proof of the same exclusive reset/IRQ/mapping lease, actual
     * inherited APSS aggregate policy, source/bus ownership and live FDT
     * supply graph. EXACT1 proves ownership; zero or negative refuses. An ACK
     * or DT min/max alone must never satisfy this. */
    int (*policy_held)(void *cookie,uint64_t lease,const struct dre_rpm_votes *,
        const struct dre_holi_power_config *);
    /* Cold acquisition only: no inherited, scheduled or firmware GPU DMA.
     * EXACT1 only. It is NOT called by the running CP provider check. */
    int (*cold_owner)(void *cookie,uint64_t lease);
    /* Called only after real power/SPTPRAC/RBBM and held cold ownership, just
     * before domain activation. EXACT1 must prove this actual reserved CB/SMR/
     * ASID, currently powered and globally initialized SMMU, real quiescence/
     * DMA/cache retirement and the exact owned NC table prefix. No DT bit or
     * future provider promise. Input has only reserved+fresh-NC prerequisites;
     * callback observes/proves without setting them. On success coordinator
     * derives the remaining existing prerequisites, then hardware activation
     * independently checks real global/CB/SID/fault/table state. */
    int (*qualify_domain)(void *cookie,const struct dre_a619_smmu *);
};
struct dre_a619_startup {
    struct dre_a619_start_plan plan;
    struct dre_holi_power power;
    struct dre_a619_holi_power sptprac;
    uint64_t lease;
    enum dre_a619_start_phase phase,failed_phase;
    const char *error;
    int hold_resources,reset_required;
};
/* Start once from zeroed private kernel state. Actual acknowledged rail holds,
 * source/bus/IRQ ownership and all memory/SCM/domain providers must already
 * exist. No automatic voltage request, clock-rate guess or resource release.
 * The return NATIVE_START_MARKER_CONFIRMED means only the real native marker
 * passed; it does not mean shader execution, a Vulkan device or Metal ready. */
const char *dre_a619_startup_begin(struct dre_a619_startup *,
    const struct dre_a619_start_plan *);
/* For providers_ready/platform_ready adapters on the native kernel cookie.
 * These observe live providers and never repeat cold acquisition or reset.
 * Raw MMIO/ownership guards must not recurse into these functions. */
int dre_a619_startup_rbbm_ready(struct dre_a619_startup *);
int dre_a619_startup_cp_ready(struct dre_a619_startup *);
const char *dre_a619_startup_check_live(struct dre_a619_startup *);
/* Mark a real native reset, suspend, remote restart, MMIO or ownership fault.
 * This only invalidates software admission. It does NOT reset hardware or
 * authorize unpinning. There is deliberately no unsafe cleanup shortcut. */
void dre_a619_startup_invalidate(struct dre_a619_startup *,const char *reason);
#ifdef __cplusplus
}
#endif
