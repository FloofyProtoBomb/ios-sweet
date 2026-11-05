/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only native controller. */
#ifndef DRE_TOUCH_XNU_TLMM_H
#define DRE_TOUCH_XNU_TLMM_H
#include "tlmm.h"
#include "../irq/dre_spi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_TOUCH_TLMM_POLICY_VERSION 1u
struct dre_touch_tlmm_policy {
    uint32_t version,struct_bytes;
    void *cookie;
    /* Actual resident native broker IOService, retained for this controller.
     * It owns the supplied real GIC controller's lifetime. Not a framebuffer
     * or HID service being used as evidence of power/route authority. */
    void *owner_service;
    struct dre_spi_controller *gic;
    /* Enter the SAME actual board ownership mutex required by dre_spi. If
     * already held by this thread, return entered=0; otherwise acquire it and
     * return entered=1. This is a real kernel lock operation, not a ready bit.
     * Canonical order is board owner -> controller lifecycle -> pin spinlock.
     * Never enter either sleeping lock from a primary interrupt callback. */
    int (*enter_owner)(void *,int *entered);
    void (*leave_owner)(void *,int entered);
    /* Actual exclusive GPIO22 + held TLMM access/power + awake-only policy,
     * and exclusion of ALL foreign TLMM summary producers. No unregistered
     * GPIO owner may assert this shared parent during the lease. This
     * implementation does not seize, scan/disable foreign pins, replace a
     * global TLMM driver or alter MPM/PDC/security/wake state.
     * The original GPIO22 input must already be MASKED and NON-PENDING by the
     * true owner; this driver does not clear an unqualified inherited owner.
     * Nonzero lease/source_owner must survive any partially retained failure.
     * source_owner is the genuine native source-owner token for dre_spi259. */
    int (*reserve)(void *,const struct dre_tn_tlmm_profile *,void **pin_lease,uint64_t *source_owner);
    int (*check)(void *,void *pin_lease,uint64_t source_owner);
    /* No sleeps, allocations, pin-lock acquisition or peripheral MMIO. */
    int (*check_fast)(void *,void *pin_lease,uint64_t source_owner);
    /* Only after actual parent/child IRQ retirement and originally masked
     * GPIO22 configuration restored. Preserve every shared dependency. */
    int (*release)(void *,void *pin_lease,uint64_t source_owner);
};
/* Creates a REAL IOKit controller backed by leased GIC259 and actual TLMM22
 * registers. It is not automatically published by any default kernel path.
 * On error *owner remains nonnull if any native/IRQ/mapping reference remains;
 * callers must retain that owner and use explicit shutdown, never discard it.
 * All API calls below are task-context and serialize with the real board owner.
 */
int dre_touch_tlmm_publish(const struct dre_touch_tlmm_policy *,void **owner);
int dre_touch_tlmm_nub(void *owner,void **io_service,unsigned *source);
/* Borrowed real pin binding after the adapter has retained/captured the actual
 * nub/controller. Only local source0/vector0 is mapped to physical GPIO22.
 * Both controller and adapter use this ONE actual IOSimpleLock-backed engine. */
int dre_touch_tlmm_pin_for_nub(void *retained_nub,unsigned source,struct dre_tn_tlmm_pin **);
int dre_touch_tlmm_shutdown(void **owner);
#ifdef __cplusplus
}
#endif
#endif
