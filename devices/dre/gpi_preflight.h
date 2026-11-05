/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
struct dre_gpi_preflight {
    uint32_t idle_mask, states, route;
    const char *dma_state, *map_state, *spi_plan;
};
/* Caller must hold the already-checked touch master/slave/serial clocks.
 * Reads AP-owned GPI channel/event states only; never allocates or starts. */
const char *dre_gpi_preflight(const void *dt, struct dre_gpi_preflight *out);
