/* SPDX-License-Identifier: MIT */
#pragma once
#include "a619_backend.h"
#ifdef __cplusplus
extern "C" {
#endif
struct dre_a619_rbbm_ops {
    struct dre_a619_ops io;
    /* Real acquired rail/source/bus/branch/SPTPRAC providers, exclusive
     * reset GPU with CP halted, secure buffers/IFPC/preemption unused,
     * SMMUv2 platform selected and no LLCC slices allocated. No DT-only bits. */
    int (*providers_ready)(void *cookie);
};
struct dre_a619_rbbm {
    uint32_t chip_id, highest_bank_bit, gmem_bytes, min_access_length, ubwc_mode;
    uint64_t last_register;
    uint32_t last_expected, last_observed, writes;
    int changed, active, unsafe;
};
/* Static nonsecure A619-Holi programming for the pinned 512KiB/HBB14,
 * 32-byte/UBWC2 board. Programs address/cache/ROQ/GMEM/protection/clock-gating
 * state, wrapper busy counters and non-LLCC SMMUv2 cache override; clears
 * only the documented Holi bus halts. Does not start CP, create translations,
 * change rails/PLL rates, write secure-buffer/trust state, enable GPU IRQs,
 * configure preemption/IFPC or provide Metal. Every store is read back.
 * Once any store is attempted, an uncertainty requires actual hardware reset;
 * retain allocations and power providers. No guessed register rollback. */
const char *dre_a619_rbbm_initialize(struct dre_a619_rbbm *state,
    const struct dre_a619_rbbm_ops *ops);
#ifdef __cplusplus
}
#endif
