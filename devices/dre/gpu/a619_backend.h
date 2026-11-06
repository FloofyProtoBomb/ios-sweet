/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DRE_A619_CHIP_ID UINT32_C(0x06010900)
#define DRE_A619_GPU_BASE UINT64_C(0x05900000)
#define DRE_A619_RB_BASE (DRE_A619_GPU_BASE + 0x800 * 4)
#define DRE_A619_RB_BASE_HI (DRE_A619_GPU_BASE + 0x801 * 4)
#define DRE_A619_RB_CNTL (DRE_A619_GPU_BASE + 0x802 * 4)
#define DRE_A619_RB_RPTR (DRE_A619_GPU_BASE + 0x806 * 4)
#define DRE_A619_RB_WPTR (DRE_A619_GPU_BASE + 0x807 * 4)

/* These are kernel-owned assertions, never userspace supplied capabilities.
 * Power/clock, SQE/CP initialization, authenticated ZAP/nonsecure transition,
 * a private SMMU domain and uncached CPU mappings must already be verified.
 * The backend does not implement or bypass any of those initialization steps. */
#define DRE_A619_POWER_CLOCK (1u << 0)
#define DRE_A619_SQE_CP (1u << 1)
#define DRE_A619_ZAP (1u << 2)
#define DRE_A619_SMMU (1u << 3)
#define DRE_A619_UNCACHED (1u << 4)
#define DRE_A619_READY_MASK 0x1fu

struct dre_a619_region { uint64_t gpu, bytes; };
struct dre_a619_queue {
    uint32_t chip_id, ready_mask;
    volatile uint32_t *ring;
    struct dre_a619_region ring_region, result_region;
    volatile uint32_t *result; /* CPU mapping of result_region.gpu */
    uint32_t ring_dwords, write_pointer;
    uint64_t submitted, completed;
    int poisoned;
};
struct dre_a619_ops {
    /* Synchronous caller serializes queue access and keeps all mappings alive.
     * guard validates unchanged device/domain ownership and no active fault.
     * barrier flushes CPU stores to the device, then orders CPU reads after DMA.
     * now is monotonic in caller-defined units; budget is in the same units. */
    int (*guard)(void *cookie);
    int (*read32)(void *cookie, uint64_t address, uint32_t *value);
    int (*write32)(void *cookie, uint64_t address, uint32_t value);
    void (*barrier)(void *cookie);
    uint64_t (*now)(void *cookie);
    void *cookie;
};

static inline int dre_a619_deadline_expired(uint64_t start, uint64_t now, uint64_t budget)
{
    /* An older counter sample must not look like a huge elapsed duration.
     * Small forward deltas still work across wrap. Callers bound poll counts. */
    uint64_t elapsed = now - start;
    return elapsed < (UINT64_C(1) << 63) && elapsed >= budget;
}

uint32_t dre_a619_type7(unsigned opcode, unsigned count);
/* One restricted native bring-up job, not an application command parser.
 * Emits only CP_MEM_WRITE, WAIT_MEM_WRITES and WAIT_FOR_IDLE. Hardware is
 * considered completed only after RPTR advances AND uncached memory matches.
 * After publication, any failure poisons the queue; its pages must remain
 * reserved until hardware reset. Never restore/free an active GPU ring. */
const char *dre_a619_submit_marker(struct dre_a619_queue *queue,
    const struct dre_a619_ops *ops, uint64_t result_gpu, uint32_t value,
    uint64_t budget);

struct dre_a619_sqe {
    const uint8_t *code;
    size_t bytes;
    uint32_t version;
};
/* Same 4-byte firmware-prefix exclusion as the matching KGSL driver. The
 * caller must verify the expected firmware digest before mapping code read-only
 * into GPU VA space. This view alone does not authenticate/load firmware. */
const char *dre_a619_sqe_view(uint32_t chip_id, const void *file, size_t bytes,
    struct dre_a619_sqe *view);
#ifdef __cplusplus
}
#endif
