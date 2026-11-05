/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include "dma_context.h"

typedef int (*dre_table_read)(void *cookie, uint64_t address, uint64_t *value);
struct dre_dma_walk {
    uint64_t first_pa, last_pa;
    unsigned pages, descriptors;
};

/* Inspect stage-1 AArch64 4K tables for at most 16 pages. The callback must
 * restrict reads to known-readable RAM and reject unstable 64-bit values.
 * Does not read/write device memory, change tables/TLBs, or authorize DMA.
 * IDENTITY_RW describes table contents, not a hardware translation test. */
const char *dre_dma_walk(const struct dre_dma_context *context, uint64_t iova,
                         uint64_t size, dre_table_read read, void *cookie,
                         struct dre_dma_walk *out);
