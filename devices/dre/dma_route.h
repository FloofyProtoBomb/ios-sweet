/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

struct dre_dma_route {
    uint32_t id0, id1, control, slot, smr, s2cr;
    unsigned matches;
    int physical_dma;
    int translated;
};

/* Read-only ARM SMMUv2 route inspection. A translation or uncertain state is
 * never treated as a physical-address DMA mapping. No SMMU register is written. */
const char *dre_dma_route(const void *dt, const char *device_path,
                          uint32_t expected_sid, struct dre_dma_route *out);
