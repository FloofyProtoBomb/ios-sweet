/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source. Kernel-private; not a userspace ABI.
 * Status operations return zero on success. No callback is a default policy.
 */
#ifndef DRE_NATIVE_SPI_H
#define DRE_NATIVE_SPI_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
class IOService;
class IOInterruptController;
extern "C" {
#else
typedef struct IOService IOService;
typedef struct IOInterruptController IOInterruptController;
#endif
#define DRE_SPI_BROKER_VERSION 1u
#define DRE_SPI_LEVEL_HIGH 4u
#define DRE_SPI_EDGE_RISING 1u
enum dre_spi_status {
    DRE_SPI_OK=0,DRE_SPI_ARGUMENT=-1,DRE_SPI_POLICY=-2,DRE_SPI_PLATFORM=-3,
    DRE_SPI_BUSY=-4,DRE_SPI_MEMORY=-5,DRE_SPI_LOST=-6,DRE_SPI_TIMEOUT=-7,
    DRE_SPI_NAMESPACE=-8,DRE_SPI_QUARANTINED=-9
};
struct dre_spi_controller;
struct dre_spi_route;
struct dre_spi_hw {
    uint64_t distributor_pa,distributor_bytes,redistributor_pa,redistributor_bytes;
    uint64_t irouter;
    uint32_t intid,ctlr,typer,pidr2,group,groupmod,configuration,priority;
    uint32_t enabled,pending,active,reserved;
};
struct dre_spi_request {
    uint32_t intid,trigger;
    /* Genuine native source/reset/power owner, not a DT pointer or pin number.
     * For TLMM259 it owns/excludes EVERY summary producer, not just GPIO22. */
    uint64_t source_owner;
};
struct dre_spi_broker {
    uint32_t version,struct_bytes;
    void *cookie;
    /* All slow API calls require this SAME actual board ownership mutex held.
     * The caller supplies serialization, not a second advisory GPU/TLMM lock. */
    int (*owner_lock_held)(void *);
    /* Acquire actual per-SPI/producer/CPU-affinity/security policy, excluding
     * all foreign owners. Hold target CPU online, preserve PMR/security and
     * group1 delivery until release. No route/trigger/global GIC reset occurs.
     * A nonzero lease/epoch MUST be exposed on partially retained failure. */
    int (*acquire)(void *,const void *original_fdt,size_t,
        const struct dre_spi_request *,const struct dre_spi_hw *,
        uint64_t *lease,uint64_t *epoch);
    int (*check)(void *,uint64_t lease,uint64_t epoch,const struct dre_spi_hw *);
    /* GICD_IGROUPR can be RAZ in NS view. This must qualify actual Group1NS
     * authority (e.g. an acquired EL3/platform policy), never infer it from
     * DT, a zero read, a local ledger, a generic ready bit or a copied number. */
    int (*group1ns_authority)(void *,uint64_t lease,uint64_t epoch,
        const struct dre_spi_hw *);
    /* IRQ-safe: no sleep/allocation/owner mutex or peripheral MMIO. Lease
     * invalidation must FIRST mask+join while GIC access is still owned.
     * Returning failure later does not authorize touching a foreign vector. */
    int (*check_fast)(void *,uint64_t lease,uint64_t epoch,uint32_t intid);
    /* Prove producer quiet and held until release, including shared summary
     * sources, reset/clock lifecycle and no pending foreign handler. */
    int (*source_quiet)(void *,uint64_t lease,uint64_t epoch,uint32_t intid);
    int (*release)(void *,uint64_t lease,uint64_t epoch);
};
/* No global handler is installed/replaced. create only obtains original FDT
 * and objects. No controller is published until reserve actually owns a SPI
 * and the linked SLEH registry owns its real dispatch slot. */
int dre_spi_controller_create(const struct dre_spi_broker *,struct dre_spi_controller **);
int dre_spi_reserve(struct dre_spi_controller *,const struct dre_spi_request *,struct dre_spi_route **);
/* Borrowed boot/route-held objects, only after namespace registration and
 * actual IOService cache resolution. source=0; specifier vector=physical INTID.
 * Register via nub->registerInterrupt(0,...); sharing is explicitly rejected.
 * No caller may edit/rebind properties or the cached source. */
int dre_spi_route_nub(struct dre_spi_route *,IOService **nub,unsigned *source,
                      IOInterruptController **controller);
/* target NULL requires no handler; non-NULL requires that exact sole owner. */
int dre_spi_route_check_handler(struct dre_spi_route *,const void *target);
int dre_spi_route_check_fast(struct dre_spi_route *,uint64_t token);
uint64_t dre_spi_route_token(const struct dre_spi_route *);
/* Fast mask writes only its held ICENABLER bit; no pending clear/EOI/reset.
 * mask_join is task-context, bounded and also waits for hardware ACTIVE=0
 * AFTER the existing SLEH EOI (a callback count alone is insufficient). */
int dre_spi_route_mask_fast(struct dre_spi_route *);
int dre_spi_route_mask_join(struct dre_spi_route *);
int dre_spi_route_unmask(struct dre_spi_route *);
/* Caller first unregisters its handler. Failure retains all memory, route,
 * namespace and external leases. Only success consumes and NULLs *route. */
int dre_spi_route_release(struct dre_spi_route **);
/* Empty, unregistered controller only. A published controller is boot-resident
 * and returns QUARANTINED (no guessed IOService client-join/termination proof).
 * A failed/uncertain reserve can retain
 * a controller/route; neither object may be discarded on nonzero status. */
int dre_spi_controller_close(struct dre_spi_controller **);
#ifdef __cplusplus
}
#endif
#endif
