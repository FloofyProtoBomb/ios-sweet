/* SPDX-License-Identifier: MIT */
#pragma once
#include "dma_context.h"
struct dre_ufs_handoff_dma {
    const void *dt;
    struct dre_dma_route route;
    struct dre_dma_context context;
};
/* Early single-CPU handoff only: recognize the observed stage-1 disabled /
 * stage-2 bypass mode without configuring SMMU registers. Only the UFS
 * module's private below-4G arena may be used. Active translation is denied. */
const char *dre_ufs_handoff_begin(const void *dt, const struct dre_dma_route *route,
                                 struct dre_ufs_handoff_dma *out);
int dre_ufs_handoff_valid(void *cookie);
