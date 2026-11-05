/* SPDX-License-Identifier: MIT */
#pragma once
#include "dma_route.h"

struct dre_ufs_read_state {
    uint32_t capabilities, block_size, ocs, response, status;
    uint64_t last_lba, userdata_first, userdata_blocks;
    unsigned commands;
    int safe_to_handoff;
};

/* Polled, LUN-0-only read test with a private, below-4G-capable DMA arena.
 * Requires a positively identified physical DMA route and the caller's
 * existing Holi UFS power/clock/resource checks. Restores the original UTRL.
 * A false safe_to_handoff MUST stop booting XNU until a hardware reset. */
const char *dre_ufs_read_test(const struct dre_dma_route *route,
                            struct dre_ufs_read_state *out);
uintptr_t dre_ufs_probe_arena(void);
