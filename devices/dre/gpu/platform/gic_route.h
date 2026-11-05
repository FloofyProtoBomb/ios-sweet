/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source. Native GPU-to-real-SPI connection only.
 * Kernel private; public GPU selector ABI1 and provider v2 are unchanged.
 */
#ifndef DRE_GPU_GIC_ROUTE_H
#define DRE_GPU_GIC_ROUTE_H
#include "native_owner.h"
#include "../../irq/dre_spi.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_GPU_GIC_POLICY_VERSION 1u
struct dre_gpu_gic_route;
struct dre_gpu_gic_policy {
    uint32_t version,struct_bytes;
    void *cookie;
    /* Actual created controller, never an ADT node/INTID or invented pointer.
     * It must use the SAME board owner mutex as the actual upstream broker. */
    struct dre_spi_controller *controller;
    /* Obtain/drop a REAL lifetime reference from the actual controller owner.
     * A token is an acquired retained reference, not a readiness flag. Partial
     * acquisition MUST return it whenever a reference escaped. No default
     * creator, Group1NS policy, CPU route or controller lease is supplied. */
    int (*hold_controller)(void *,struct dre_spi_controller *,uint64_t *hold);
    int (*check_controller)(void *,struct dre_spi_controller *,uint64_t hold);
    int (*drop_controller)(void *,struct dre_spi_controller *,uint64_t hold);
    /* Resolve the genuine source-owner record ALREADY acquired by upstream
     * acquire. Match exact GPU lease/reset epoch and actual held controller;
     * never mint a token from a DT address, counter, GPU lease or IRQ number. */
    int (*source_owner)(void *,uint64_t gpu_lease,uint64_t reset_epoch,
        struct dre_spi_controller *,uint64_t controller_hold,uint64_t *source_owner);
    /* Ownership/generation checks, not GPU powered/ready or raw-MMIO probes.
     * A fault can still retain a valid lease needed for real retirement. */
    int (*check_source)(void *,uint64_t gpu_lease,uint64_t reset_epoch,
        struct dre_spi_controller *,uint64_t controller_hold,uint64_t source_owner);
    /* Primary IRQ: no sleep/allocation/board mutex/peripheral MMIO. Validates
     * the same actual controller/GPU/source lifetimes and reset generations.
     * All policy callbacks are base ownership operations: they must not call
     * this wrapper, native_owner guards or dre_spi recursively as their proof. */
    int (*check_source_fast)(void *,uint64_t gpu_lease,uint64_t reset_epoch,
        struct dre_spi_controller *,uint64_t controller_hold,uint64_t source_owner);
};
/* upstream supplies every NON-IRQ native_owner broker callback, actual common
 * lock and external GPU/MMIO/reset/RPM/retirement policy. Its five IRQ fields
 * are ignored, not used as a success fallback. No callback executes now unless
 * this API is explicitly invoked later. All statuses use DRE_GPU_* (zero OK).
 * On a partially retained controller hold, out remains nonnull on failure. */
int dre_gpu_gic_route_create(const struct dre_gpu_native_broker *upstream,
    const struct dre_gpu_gic_policy *,struct dre_gpu_gic_route **out);
/* Produce a full correctly typed broker with this bridge cookie. Non-IRQ
 * operations forward to the ORIGINAL policy cookie. Acquire retains genuine
 * partial GPU leases; release interlocks even zero-token partial SPI holds.
 * This escapes callbacks: the bridge/controller pin then remain boot-held.
 * Called with the real upstream owner mutex held, before native_owner_create. */
int dre_gpu_gic_route_broker(struct dre_gpu_gic_route *,struct dre_gpu_native_broker *out);
/* Owner-mutex-held private metadata ONLY, including failed partial acquire.
 * Returns the actual upstream outputs; zero means no recorded retained lease.
 * Does not check owns/source/route readiness and cannot authorize hardware. */
int dre_gpu_gic_retained_acquire(struct dre_gpu_gic_route *,uint64_t *gpu_lease,uint64_t *reset_epoch);
/* The five exact native_owner IRQ callback signatures, also usable by a
 * platform already carrying this bridge in its own real policy object. */
int dre_gpu_gic_reserve_irq_route(void *,uint64_t,const struct dre_gpu_owner_resources *,struct dre_gpu_owner_irq_route *);
int dre_gpu_gic_check_irq_route(void *,uint64_t,const struct dre_gpu_owner_irq_route *,const void *target);
int dre_gpu_gic_irq_lease_fast(void *,uint64_t,uint64_t,uint64_t route_lease);
int dre_gpu_gic_mask_owned_irq_fast(void *,uint64_t,uint64_t route_lease);
int dre_gpu_gic_release_irq_route(void *,uint64_t,const struct dre_gpu_owner_irq_route *);
/* Pre-escape disposal only. Failure retains *owner. An escaped callback cookie
 * cannot be freed without a genuine general callback-join provider; this slice
 * deliberately keeps it boot-resident, including after clean GPU release. */
int dre_gpu_gic_route_close(struct dre_gpu_gic_route **owner);
#ifdef __cplusplus
}
#endif
#endif
