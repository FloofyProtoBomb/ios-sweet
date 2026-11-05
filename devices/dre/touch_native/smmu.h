/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight source. */
#ifndef DRE_TOUCH_SMMU_H
#define DRE_TOUCH_SMMU_H
#include "native.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { DRE_TN_SMMU_PAGE = 4096, DRE_TN_SMMU_TABLES = 3,
       DRE_TN_SMMU_MAPS = 5, DRE_TN_SMMU_MAP_PAGES = 4 };
enum { DRE_TN_SMMU_READ = 1u, DRE_TN_SMMU_WRITE = 2u };
struct dre_tn_smmu_profile {
    uint64_t base, bytes, tcu, tbu, iova, iova_bytes;
    uint32_t actlr, sid, cb_irq[64], cb_irqs;
};
struct dre_tn_smmu_page { uint64_t pa; };
/* This is a real platform owner's reservation/evidence, not DT readiness.
 * The platform must atomically reserve an UNUSED nonsecure CB/SMR/ASID and
 * its context-fault IRQ, hold the actual shared TCU/anoc_1 TBU power/clocks,
 * prove global firmware/native initialization and SID0x16 producer quiescence,
 * and retain its opaque domain lease. The component re-reads every observed
 * register below; epoch identifies that real held global-owner transaction.
 * No default constructor or DT-derived evidence provider is supplied.
 */
struct dre_tn_smmu_reservation {
    uint32_t cb, smr, asid, fault_irq;
    uint32_t id0, id1, id2, cr0, acr, tcu_version;
    uint64_t epoch;
};
struct dre_tn_smmu_tables {
    volatile uint64_t *cpu[DRE_TN_SMMU_TABLES];
    uint64_t pa[DRE_TN_SMMU_TABLES];
};
struct dre_tn_smmu_token { uint64_t generation, iova; uint32_t slot, pages; };
struct dre_tn_smmu_entry {
    struct dre_tn_smmu_token token;
    struct dre_tn_smmu_page page[DRE_TN_SMMU_MAP_PAGES];
    uint32_t permissions, used;
};
struct dre_tn_smmu_ops {
    void *cookie;
    /* Zero success only after actual native owner/initialization checks.
     * require_quiet additionally retires ALL SID0x16 GPI producers/IRQs and
     * outstanding device/table references. A free single GPII is insufficient.
     * Shared register/ASID ownership remains serialized by that native owner. */
    int (*guard)(void *, const struct dre_tn_smmu_reservation *, int require_quiet);
    int (*owns_nc)(void *, const void *cpu, uint64_t pa, size_t bytes);
    int (*owns_page)(void *, uint64_t pa);
    int (*read32)(void *, uint64_t, uint32_t *);
    int (*read64)(void *, uint64_t, uint64_t *);
    int (*write32)(void *, uint64_t, uint32_t);
    int (*write64)(void *, uint64_t, uint64_t);
    void (*barrier)(void *, enum dre_tn_barrier);
    uint64_t (*ticks)(void *);
    uint64_t (*ticks_hz)(void *);
    void (*delay_us)(void *, unsigned);
};
struct dre_tn_smmu {
    struct dre_tn_smmu_profile profile;
    struct dre_tn_smmu_reservation owner;
    struct dre_tn_smmu_tables tables;
    struct dre_tn_smmu_ops ops;
    struct dre_tn_smmu_entry map[DRE_TN_SMMU_MAPS];
    uint64_t cb_base, next_generation, saved_ttbr[2];
    uint32_t saved[9], saved_smr, saved_s2cr, num_cb, num_smr;
    uint32_t active, changed, unsafe, last_fsr, last_global_fsr;
    enum dre_tn_result first_error;
};
enum dre_tn_result dre_tn_smmu_profile_from_fdt(const void *, size_t,
                                               struct dre_tn_smmu_profile *);
enum dre_tn_result dre_tn_smmu_begin(struct dre_tn_smmu *,
    const struct dre_tn_smmu_profile *, const struct dre_tn_smmu_reservation *,
    const struct dre_tn_smmu_tables *, const struct dre_tn_smmu_ops *);
enum dre_tn_result dre_tn_smmu_map(struct dre_tn_smmu *,
    const struct dre_tn_smmu_page *, unsigned count, unsigned permissions,
    struct dre_tn_smmu_token *);
enum dre_tn_result dre_tn_smmu_walk(struct dre_tn_smmu *,
    const struct dre_tn_smmu_token *, const struct dre_tn_smmu_page *,
    unsigned count, unsigned permissions);
enum dre_tn_result dre_tn_smmu_unmap(struct dre_tn_smmu *,
                                    const struct dre_tn_smmu_token *);
/* Refuses nonempty/uncertain domains. Faults SID0x16 first, retires context
 * TLBs, restores only this owned CB and UNUSED SMR, and never writes global
 * CR0/ACR/reset, another CB/stream, TBU debug/ATOS, UFS or secure registers. */
enum dre_tn_result dre_tn_smmu_end(struct dre_tn_smmu *);
#ifdef __cplusplus
}
#endif
#endif
