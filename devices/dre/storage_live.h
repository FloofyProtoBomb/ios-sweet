/* SPDX-License-Identifier: MIT */
#pragma once
#include "libfdt/libfdt.h"
#include "storage_abi.h"
#include "dma_memory.h"
static inline int dre_storage_dt_address(const void *dt, const char *key, uint64_t *out)
{
    int length;
    const fdt32_t *p = fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), key, &length);
    if (!p || (length != 4 && length != 8)) return 0;
    *out = fdt32_to_cpu(p[0]);
    if (length == 8) *out = (*out << 32) | fdt32_to_cpu(p[1]);
    return 1;
}
/* The bootloader's ramdisk is not necessarily listed in reserved-memory. */
static inline int dre_storage_live_valid(const void *dt, const uint64_t w[8])
{
    uint64_t start, end;
    if (!dre_storage_abi_valid(w) ||
        !dre_storage_dt_address(dt, "linux,initrd-start", &start) ||
        !dre_storage_dt_address(dt, "linux,initrd-end", &end) || end < start ||
        (start != end && w[5] < end && w[5] + w[6] > start)) return 0;
    return dre_dma_ram_allowed(dt, w[1], w[2]) && dre_dma_ram_allowed(dt, w[3], w[4]);
}
