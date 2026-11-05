/* SPDX-License-Identifier: MIT */
#pragma once
#include "dma_context.h"
struct dre_ufs_handoff_dma {
    const void *dt;
    struct dre_dma_route route;
    struct dre_dma_context context;
};
/* Recognize the observed stage-1 disabled / stage-2 bypass mode without
 * configuring SMMU registers. Caller serializes transactions and owns the
 * module's private uncached below-4G arena. Active translation is denied. */
const char *dre_ufs_handoff_begin(const void *dt, const struct dre_dma_route *route,
                                 struct dre_ufs_handoff_dma *out);
int dre_ufs_handoff_valid(void *cookie);
