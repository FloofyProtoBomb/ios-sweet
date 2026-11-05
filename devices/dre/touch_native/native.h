/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight draft. No default native resource/DMA provider exists.
 */
#ifndef DRE_TOUCH_NATIVE_H
#define DRE_TOUCH_NATIVE_H
#include <stddef.h>
#include <stdint.h>

enum dre_tn_result {
    DRE_TN_OK, DRE_TN_ARGUMENT, DRE_TN_PROFILE, DRE_TN_PROVIDER,
    DRE_TN_OWNERSHIP, DRE_TN_DMA, DRE_TN_MMIO, DRE_TN_TIMEOUT,
    DRE_TN_PROTOCOL, DRE_TN_POISONED, DRE_TN_STATE, DRE_TN_FIRMWARE,
    DRE_TN_CHECKSUM, DRE_TN_RECOVERY, DRE_TN_INPUT
};
enum { DRE_TN_TX_RING, DRE_TN_RX_RING, DRE_TN_EVENT_RING,
       DRE_TN_TX_BYTES, DRE_TN_RX_BYTES, DRE_TN_BLOCKS };
enum dre_tn_barrier { DRE_TN_PUBLISH, DRE_TN_CONSUME, DRE_TN_MMIO_FINISH };

struct dre_tn_profile {
    uint64_t spi_base, gpi_base, gpi_ee_base, iova_base, iova_size;
    uint32_t gpi_irq[5], spi_irq, gpi_mask, event_factor, stream_id;
    uint32_t irq_gpio, reset_gpio, cs_gpio, width, height, max_hz;
    uint32_t firmware_size;
    uint8_t firmware_sha256[32];
};

/* No firmware bytes, FDT bytes or per-phone identifiers leave this API. */
#ifdef __cplusplus
extern "C" {
#endif
enum dre_tn_result dre_tn_profile_from_fdt(const void *fdt, size_t bytes,
                                          struct dre_tn_profile *out);
#ifdef __cplusplus
}
#endif

struct dre_tn_memory {
    volatile uint8_t *cpu;
    uint64_t iova;
    size_t bytes;
    void *allocation; /* Provider-owned wired allocation + domain mapping. */
};
struct dre_tn_clock {
    uint32_t source, divider;
    uint64_t parent_hz;
};

/* These are native kernel operations, not readiness flags. Every zero return
 * must follow the actual operation/proof described here. Missing ops refuse.
 * All calls except IRQ entry run on ONE serialized, sleepable work loop.
 *
 * reserve: atomically lease a free AP GPII pair/event ring, SE0, pins0..3/21/22,
 * GPI IRQ + GENI IRQ + TLMM IRQ, wrapper clocks/rails and panel/reset lifecycle.
 * It must exclude the old Sweet touch driver and other QUP clients. The GPII
 * is allocated dynamically from0..4; DT's second DMA cell is SEID0, not GPII0.
 * get_clock: inspect and hold a qualified existing parent/source/rate; no PLL
 * programming or frequency-derived shared-rail policy is authorized here.
 * alloc_nc/verify_nc: actual wired Normal-NC CPU pages with no WB alias, and
 * SID0x16 IOVA translations/permissions over EVERY byte. A physical/CPU address
 * cast or GPU SID0 mapping is not a touch DMA mapping.
 * irq_mask_sync: mask the leased GIC/GENI/GPI sources and join in-flight IRQ
 * handlers before worker-only MMIO. Command acknowledgements are then polled
 * from the OWNED GPI source register; a state bit alone is insufficient.
 * release: after all GPI contexts deallocated and IRQs joined, drop only this
 * owner's refs. It must preserve other owners' rail/clock/pin requirements.
 * Any ambiguous command/doorbell write retains all resources until a separate
 * proven native recovery retires DMA. No automatic reset/SMMU reprogramming.
 */
struct dre_tn_ops {
    void *cookie;
    int (*reserve)(void *, const struct dre_tn_profile *, void **lease, unsigned *gpii);
    int (*check_lease)(void *, void *lease);
    int (*get_clock)(void *, void *lease, struct dre_tn_clock *);
    int (*alloc_nc)(void *, void *lease, size_t bytes, size_t align,
                    struct dre_tn_memory *);
    int (*verify_nc)(void *, void *lease, const struct dre_tn_memory *,
                     unsigned stream_id);
    int (*free_nc)(void *, void *lease, struct dre_tn_memory *);
    int (*read32)(void *, void *lease, uint64_t address, uint32_t *);
    int (*write32)(void *, void *lease, uint64_t address, uint32_t);
    void (*barrier)(void *, enum dre_tn_barrier);
    uint64_t (*ticks)(void *); /* Monotonic CNTVCT, 19.2MHz, wraps modulo2^64. */
    uint64_t (*ticks_hz)(void *);
    void (*delay_us)(void *, unsigned); /* Worker sleep or bounded hardware wait. */
    int (*irq_mask_sync)(void *, void *lease);
    int (*release)(void *, void *lease);
};
#endif
