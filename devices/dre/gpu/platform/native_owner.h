/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight native adapter. */
#ifndef DRE_GPU_NATIVE_OWNER_H
#define DRE_GPU_NATIVE_OWNER_H
#include "../kernel/dre_gpu_provider.h"
#include "profile.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_GPU_NATIVE_BROKER_VERSION 1u
struct dre_gpu_native_owner;
struct dre_gpu_owner_aperture { uint64_t pa,bytes; };
struct dre_gpu_owner_resources {
    uint32_t chip_id,gic_phandle,gic_spi,gic_intid,irq_flags,aperture_count;
    struct dre_gpu_owner_aperture aperture[6];
    uint64_t gicd_pa,gicd_bytes,gicr_pa,gicr_bytes;
    /* Original-FDT topology/source facts only. Aggregate floors and allowed
     * level masks remain zero; this is never acquired rail policy. */
    struct dre_holi_power_profile power;
};
struct dre_gpu_owner_irq_route {
    void *io_service,*interrupt_controller;
    uint64_t route_lease;
    uint32_t source,intid,gic_phandle,reserved;
};
enum dre_gpu_native_owner_fault {
    DRE_GPU_OWNER_IRQ=1,DRE_GPU_OWNER_RESET_CHANGED=2,
    DRE_GPU_OWNER_CLOCK_LOST=3,DRE_GPU_OWNER_LEASE_LOST=4
};
/* All status callbacks return0 only after the actual described operation or
 * proof. This API supplies NO default resource broker. A local ledger or DT
 * match cannot establish external shared regulator/reset/IRQ policy. */
struct dre_gpu_native_broker {
    uint32_t version,struct_bytes;
    void *cookie;
    /* This is the SAME actual lifetime mutex held over coordinator, VM,
     * firmware/SCM and submit operations, not a parallel advisory lock.
     * create/bind/arm/guard/cold/policy/io are called with it held.
     * The deferred IRQ worker uses enter/leave; close is called outside it. */
    void (*enter_owner)(void *);
    void (*leave_owner)(void *);
    int (*owner_lock_held)(void *);
    /* Atomically acquire real external GPU reset/source/bus/MMIO/IRQ policy.
     * Refuse inherited active consumers, unknown shared APSS rail votes and
     * foreign DMA. A nonzero lease/reset_epoch must be returned even on a
     * partial acquisition that retains any resource. No reset is requested. */
    int (*acquire)(void *,const void *original_fdt,size_t,
        const struct dre_gpu_owner_resources *,uint64_t *lease,uint64_t *reset_epoch);
    int (*check_lease)(void *,uint64_t lease,uint64_t reset_epoch);
    /* Fast operations run at primary interrupt context: no allocation,
     * sleeping, owner mutex, policy acquisition or GPU/rail MMIO. mask must
     * compare real route/lease generations and mask ONLY this owned vector;
     * when ownership is lost it must leave a foreign vector untouched. */
    int (*irq_lease_fast)(void *,uint64_t,uint64_t,uint64_t route_lease);
    int (*mask_owned_irq_fast)(void *,uint64_t,uint64_t route_lease);
    /* Supply an actual still-leased IOService nub/controller route, not the
     * GPU ADT node or intid209 used as an IOKit source index. The selected
     * controller must be genuinely connected to DRE's existing GIC dispatcher
     * and own SPI177/INTID209 level-high. No global handler replacement.
     * The source/controller must reject sharing atomically at registration.
     * source is the nub's source index, not guessed from either IRQ number. */
    int (*reserve_irq_route)(void *,uint64_t,const struct dre_gpu_owner_resources *,
        struct dre_gpu_owner_irq_route *);
    /* target=NULL: prove an exclusively reserved, nonsharing, unregistered
     * route; target!=NULL: verify the actual registered sole handler owner.
     * The actual publisher must have already resolved the nub's source cache
     * after registering its real controller. No missing-name lookup is run.
     * Must inspect the nub's ACTUALLY RESOLVED controller/vector and real
     * installed global dispatch/handler ownership, not just properties or a
     * DT flag. Rebinding an old cached controller is not this adapter's job. */
    int (*check_irq_route)(void *,uint64_t,const struct dre_gpu_owner_irq_route *,const void *target);
    int (*release_irq_route)(void *,uint64_t,const struct dre_gpu_owner_irq_route *);
    /* Per-access source/clock/domain/reset ownership, including which GPU
     * apertures are actually powered. GCC/GPUCC may be accessible while GPU
     * core is off; do not infer powered MMIO from one generic lease bit.
     * Must not recurse into this adapter or coordinator MMIO/live checks. */
    int (*mmio_access)(void *,uint64_t,uint64_t,uint64_t pa,size_t,int write);
    int (*cold_exclusive)(void *,uint64_t,uint64_t);
    int (*held_policy)(void *,uint64_t,uint64_t,const struct dre_rpm_votes *,
        const struct dre_holi_power_config *);
    /* Actual CP/firmware/DMA/domain/reset/consumer retirement plus ownership
     * of every retained mapping is required before any resource release.
     * This adapter never performs a reset or invents that proof. */
    int (*retired)(void *,uint64_t,uint64_t);
    int (*release)(void *,uint64_t,uint64_t);
};
/* Original authority comes from dre_native_board_snapshot. No caller FDT,
 * identifiers or synthetic resource broker is installed. out receives a held
 * owner even on failure after partial acquisition. Do not discard it. */
int dre_gpu_native_owner_create(const struct dre_gpu_native_broker *,struct dre_gpu_native_owner **out);
/* Configure-phase bind to the existing real mapped Device IO callbacks. It
 * copies the callbacks; a stack-allocated descriptor need not remain live. */
int dre_gpu_native_owner_bind_io(struct dre_gpu_native_owner *,const struct dre_gpu_native_io *);
int dre_gpu_native_owner_bind_startup(struct dre_gpu_native_owner *,struct dre_a619_startup *);
/* Arm only the already-qualified parent/controller route. No GPU interrupt
 * mask, reset, IRQ affinity or controller-global register is programmed. Any
 * event is treated as a fault in the current polling/INT_MASK0 profile. */
int dre_gpu_native_owner_arm_irq(struct dre_gpu_native_owner *);
uint64_t dre_gpu_native_owner_lease(struct dre_gpu_native_owner *);
int dre_gpu_native_owner_io(struct dre_gpu_native_owner *,struct dre_gpu_native_io *);
/* Exact boolean1/0 wrappers suitable for existing coordinator callback ABI. */
int dre_gpu_native_owner_guard(void *);
int dre_gpu_native_owner_cold(void *,uint64_t lease);
int dre_gpu_native_owner_policy(void *,uint64_t,const struct dre_rpm_votes *,const struct dre_holi_power_config *);
/* Task context, owner mutex held. Invalidates admission, never frees/resets. */
void dre_gpu_native_owner_invalidate(struct dre_gpu_native_owner *,enum dre_gpu_native_owner_fault);
/* Outside owner mutex AND worker loop. Synchronously joins ISR/work, removes
 * only this registered owner after actual retirement, then drops broker refs.
 * A nonzero result retains the entire owner; zero consumes and NULLs *owner. */
int dre_gpu_native_owner_close(struct dre_gpu_native_owner **);
#ifdef __cplusplus
}
#endif
#endif
