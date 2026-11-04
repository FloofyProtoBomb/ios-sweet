/* SPDX-License-Identifier: MIT */
#pragma once
#include "dma_route.h"

struct dre_dma_context {
    uint32_t bank, cbar, cba2r, control, tcr, tcr2, ttbr_lo, ttbr_hi;
    int captured;
};

/* Read context metadata only. Does not walk RAM tables, grant DMA permission,
 * issue an ATOS command, or change any SMMU register/mapping. */
const char *dre_dma_context(const struct dre_dma_route *route, struct dre_dma_context *out);
