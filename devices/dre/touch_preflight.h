/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

struct dre_touch_preflight {
    uint32_t votes_before, votes_during, votes_after;
    uint32_t revision, status, fifo_disabled, tx_geometry, rx_geometry;
    uint32_t pin_match, irq_level;
    uint32_t firmware_index, firmware_bytes, firmware_version, partitions;
    const char *firmware_state;
    int safe_to_handoff;
};

/* Temporarily vote only the three known GCC clocks, inspect GENI without
 * sending commands, and restore our votes. Select firmware from the live DT
 * using the selected panel token. No pin writes, resets, firmware upload or DMA.
 * If clock restoration fails, the caller must stop the XNU handoff. */
const char *dre_touch_preflight(const void *dt, struct dre_touch_preflight *out);
