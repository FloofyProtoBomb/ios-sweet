/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight source. */
#ifndef DRE_TOUCH_QUP_H
#define DRE_TOUCH_QUP_H
#include "native.h"
#ifdef __cplusplus
extern "C" {
#endif
struct dre_tn_qup_pll { uint32_t mode,l,user,opmode,frac; };
struct dre_tn_qup_snapshot {
    uint32_t votes,branches[3],rcg_command,rcg_cfg,rcg_m,rcg_n,rcg_d,dfs;
    uint32_t perf[8][3]; /* CFG,M,N for each real hardware DFS level. */
    struct dre_tn_qup_pll pll[2]; /* GPLL0/GPLL6, actual Fabia readbacks. */
    uint32_t geni_source,geni_clock,geni_status,geni_revision,fifo_disable,wrapper_version,geni_irq;
    uint32_t pin_config[6]; /* GPIO0/1/2/3/21/22, no readiness inferred. */
    uint32_t gpi[5][3],gpi_irq[5],gpi_ch_irq[5],gpi_event_irq[5];
    uint32_t gpi_ieob_irq[5],gpi_global_irq[5],gpi_general_irq[5];
};
enum dre_tn_qup_reason {
    DRE_TN_QUP_NONE, DRE_TN_QUP_ACCESS, DRE_TN_QUP_CHANGED, DRE_TN_QUP_BRANCH_OFF,
    DRE_TN_QUP_ROOT_OFF, DRE_TN_QUP_PARENT, DRE_TN_QUP_PLL_OFF,
    DRE_TN_QUP_MND, DRE_TN_QUP_DFS, DRE_TN_QUP_GENI, DRE_TN_QUP_RATE,
    DRE_TN_QUP_PINS, DRE_TN_QUP_BUSY, DRE_TN_QUP_REGISTERED_OWNER
};
struct dre_tn_qup_clock {
    struct dre_tn_clock serial;
    uint64_t pll_hz,parent_hz,serial_hz;
    uint32_t parent_mux,dfs_level,m,n,hid,oversampling;
    enum dre_tn_qup_reason reason;
};
struct dre_tn_qup_io {
    void *cookie;
    /* Real shared power/source/access lease required BEFORE peripheral MMIO.
     * This cannot be implemented by a DT/GPIO/readiness bit. */
    int (*guard)(void *);
    int (*read32)(void *,uint64_t,uint32_t *);
    void (*barrier)(void *,enum dre_tn_barrier);
};
/* Bind the additional GCC/wrapper resources and exact SPI clock phandles to
 * the boot-held Holi FDT. This is topology validation, not live clock policy. */
enum dre_tn_result dre_tn_qup_profile_from_fdt(const void *,size_t);
/* Read-only MMIO allowlist used by the actual XNU adapter. */
int dre_tn_qup_read_allowed(uint64_t);
enum dre_tn_result dre_tn_qup_observe(const struct dre_tn_qup_io *,struct dre_tn_qup_snapshot *);
/* xo_hz must be qualified by the actual native shared clock owner. The timer
 * frequency or a DT oscillator property alone is not a held clock provider.
 * This computes actual Hertz, never programs a source, PLL, vote or divider.
 * Snapshot + reason remain available to diagnose refusal, without identifiers. */
enum dre_tn_result dre_tn_qup_clock_decode(const struct dre_tn_qup_snapshot *,uint64_t xo_hz,
                                          struct dre_tn_qup_clock *);
uint32_t dre_tn_qup_idle_mask(const struct dre_tn_qup_snapshot *);

/* Concrete bounded software resource registry. All callers MUST hold the
 * same real kernel lock. Native cross-driver/firmware exclusion and power/
 * clock/panel/parent-IRQ policy are additional mandatory ownership, not proved
 * by this registry. It cannot claim an active unregistered Linux/firmware owner.
 * GPI hardware ALLOC/START/control ACK remains in gpi_spi.c after this claim. */
enum { DRE_TN_QUP_REGISTRY_OWNERS=16 };
struct dre_tn_qup_claim { uint64_t generation; uintptr_t owner; uint32_t resources,gpii; };
struct dre_tn_qup_registry {
    uint64_t generation;
    struct dre_tn_qup_claim owner[DRE_TN_QUP_REGISTRY_OWNERS];
};
enum dre_tn_result dre_tn_qup_claim_idle(struct dre_tn_qup_registry *,uintptr_t owner,
    const struct dre_tn_qup_snapshot *,unsigned preferred,struct dre_tn_qup_claim *);
enum dre_tn_result dre_tn_qup_claim_live(const struct dre_tn_qup_registry *,
                                        const struct dre_tn_qup_claim *);
enum dre_tn_result dre_tn_qup_retirement_check(const struct dre_tn_qup_registry *,
    const struct dre_tn_qup_claim *,const struct dre_tn_qup_snapshot *);
/* Only after real GPI STOP/DEALLOC ACKs, IRQ join and SMMU retirement. Snapshot
 * must show an idle GENI and no pending owned control/completion interrupts.
 * STOP state3 can remain after an observed DEALLOC, but is NOT considered a
 * free new context by claim_idle. This source does not invent a reset/reuse. */
enum dre_tn_result dre_tn_qup_release_retired(struct dre_tn_qup_registry *,
    const struct dre_tn_qup_claim *,const struct dre_tn_qup_snapshot *);
#ifdef __cplusplus
}
#endif
#endif
