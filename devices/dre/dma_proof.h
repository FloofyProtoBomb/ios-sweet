/* SPDX-License-Identifier: MIT */
#pragma once
#include "dma_route.h"
#include "dma_walk.h"
const char *dre_dma_proof(const void *dt, const struct dre_dma_route *route,
                          const char *path, uint32_t sid, uintptr_t buffer,
                          uint64_t bytes, struct dre_dma_walk *out);
uintptr_t dre_usb_probe_arena(void);
