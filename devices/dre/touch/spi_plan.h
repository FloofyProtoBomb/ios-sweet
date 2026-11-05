/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
struct dre_spi_plan { uint32_t tx[3][4], rx[1][4]; };
/* One CS0, 8-bit, full-duplex transfer. Build descriptors only; caller must
 * own GPI rings, map buffers, configure pins/clocks and manage completion.
 * No channel command, DMA, touch reset or firmware upload is performed. */
const char *dre_spi_plan(struct dre_spi_plan *out, uint64_t tx, uint64_t rx,
                         unsigned bytes, unsigned mode, unsigned clock_source,
                         unsigned clock_divider);
