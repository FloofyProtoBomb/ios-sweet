/* SPDX-License-Identifier: MIT */
#pragma once
#include "a619_backend.h"
#ifdef __cplusplus
extern "C" {
#endif

/* GPU mappings must have exactly these access permissions. All CPU mappings
 * are exclusively owned Normal-NC. No caller-owned capability bits are used. */
#define DRE_A619_CP_READ 1u
#define DRE_A619_CP_WRITE 2u
#define DRE_A619_CP_EXECUTE 4u
struct dre_a619_cp_buffer {
    struct dre_a619_region region;
    volatile uint32_t *cpu;
};
struct dre_a619_cp_ops {
    struct dre_a619_ops io;
    /* Prove acquired rails/rates/bus/SPTPRAC, static RBBM/protection/UCHE
     * programming, the owned active SID0 domain, and disabled IFPC/preemption.
     * This must consult real provider state, not DT presence/readiness bits. */
    int (*platform_ready)(void *cookie);
    /* Prove exact GPU->owned RAM->CPU mapping, size, NC CPU memory type and
     * GPU permissions; reject aliases, secure pages, unrelated CPU buffers. */
    int (*mapping)(void *cookie,const struct dre_a619_cp_buffer *buffer,
        uint32_t permissions);
    /* Check the entire private firmware against the trusted manifest digest.
     * No firmware bytes or private per-phone material belong in the repo. */
    int (*firmware_verified)(void *cookie,const void *file,size_t bytes);
    /* Invoke the real PAS-13 authenticated loader after ME_INIT. Return true
     * only for successful, still-owned authenticated ZAP state. No bypass. */
    int (*authenticate_zap)(void *cookie);
};
struct dre_a619_cp {
    uint32_t chip_id;
    struct dre_a619_queue *queue; /* fresh, empty, exclusive single ring */
    const void *firmware;
    size_t firmware_bytes;
    struct dre_a619_cp_buffer sqe, scratch, powerup;
    uint32_t stage, status, irq_status, hw_fault, observed_rptr, observed_wptr;
    int changed, active, unsafe;
};
/* Exact pinned Holi RB/SQE/12-dword ME_INIT, real register-list snapshot,
 * WHERE_AM_I, authenticated ZAP, CP_SET_SECURE_MODE and checked marker.
 * This is a cold, quiescent single-ring diagnostic initialization slice,
 * not clocks/RPM/RBBM cold-start, scheduling, drawing, a Metal API provider,
 * suspend/resume or preemption. The supplied providers must already be real.
 * Any uncertain MMIO/ownership/completion after the first store requires a
 * hardware reset. Retain ALL mappings, firmware and ZAP resources until reset;
 * never roll back/free a potentially active DMA ring. No release API is given.
 * Success sets queue readiness only after actual CP/secure-state checks and
 * requires the existing queue backend's hardware-confirmed marker completion. */
const char *dre_a619_cp_start(struct dre_a619_cp *state,
    const struct dre_a619_cp_ops *ops,uint64_t budget);
#ifdef __cplusplus
}
#endif
