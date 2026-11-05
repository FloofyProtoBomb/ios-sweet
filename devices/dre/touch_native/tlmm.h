/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only implementation. */
#ifndef DRE_TOUCH_TLMM_H
#define DRE_TOUCH_TLMM_H
#include "native.h"
#ifdef __cplusplus
extern "C" {
#endif
struct dre_tn_tlmm_profile {
    uint64_t base,bytes,pin_base,gicd,gicr,mpm,mpm_bytes,ipc;
    uint32_t pin,parent_intid,parent_trigger,wake_intid,wake_trigger,wake_pin;
};
struct dre_tn_tlmm_snapshot { uint32_t control,input,config,status; };
struct dre_tn_tlmm_ops {
    void *cookie;
    /* Actual exclusive pin/summary-parent, TLMM access/power and awake lease.
     * IRQ-safe; it must not take this pin lock or acquire new policy. No DT,
     * framebuffer, GPIO level or claimed-ready value can establish this owner. */
    int (*guard_fast)(void *);
    uintptr_t (*lock_irq)(void *);
    void (*unlock_irq)(void *,uintptr_t);
    int (*read32)(void *,unsigned offset,uint32_t *);
    int (*write32)(void *,unsigned offset,uint32_t);
    void (*finish)(void *);
};
struct dre_tn_tlmm_pin {
    struct dre_tn_tlmm_profile profile;
    struct dre_tn_tlmm_ops ops;
    struct dre_tn_tlmm_snapshot original,last;
    uint32_t active,configured,mutated,poisoned;
};
enum dre_tn_result dre_tn_tlmm_profile_from_fdt(const void *,size_t,struct dre_tn_tlmm_profile *);
/* Read-only bind accepts an already masked, non-pending exclusive GPIO22 input.
 * It neither masks an unknown inherited owner nor configures panel/reset/wake. */
enum dre_tn_result dre_tn_tlmm_begin(struct dre_tn_tlmm_pin *,const struct dre_tn_tlmm_profile *,
                                   const struct dre_tn_tlmm_ops *);
/* Caller has actually acquired and masked/joined the real GIC summary route.
 * Programs ONLY owned GPIO22 target/type/raw fields, leaves pin masked and
 * acknowledges any setup edge. No foreign GPIO/MPM/PDC/security register. */
enum dre_tn_result dre_tn_tlmm_configure(struct dre_tn_tlmm_pin *);
enum dre_tn_result dre_tn_tlmm_mask(struct dre_tn_tlmm_pin *,int acknowledge);
enum dre_tn_result dre_tn_tlmm_arm(struct dre_tn_tlmm_pin *,unsigned *needs_worker);
enum dre_tn_result dre_tn_tlmm_recheck(struct dre_tn_tlmm_pin *,unsigned *needs_worker);
enum dre_tn_result dre_tn_tlmm_sample(struct dre_tn_tlmm_pin *,unsigned *level);
/* Summary ISR: observes a REAL enabled pending pin and masks it; the actual
 * child handler does its single W0 ACK. A low GPIO alone is not an IRQ event. */
enum dre_tn_result dre_tn_tlmm_pending(struct dre_tn_tlmm_pin *,unsigned *pending);
/* Only after real child+parent IRQ mask/join/unregister and source exclusion.
 * Restores the originally masked config. Unknown writes retain the owner. */
enum dre_tn_result dre_tn_tlmm_end(struct dre_tn_tlmm_pin *);
#ifdef __cplusplus
}
#endif
#endif
