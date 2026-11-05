/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Input FDT must already pass fdt_check_full. Deny every reserved interval,
 * including dynamic alloc-ranges, and all malformed/overflowing ranges. */
int dre_dma_ram_allowed(const void *dt, uint64_t address, uint64_t size);
/* Only for the bootloader-loaded image / original input FDT whose ownership
 * is explicitly transferred. Dynamic shared-dma-pool search windows are
 * not occupied reservations before Linux. All fixed reservations still deny.
 * Never use this for page-table inspection or arbitrary unowned DMA memory. */
int dre_dma_boot_owned_ram_allowed(const void *dt, uint64_t address, uint64_t size);
#ifdef __cplusplus
}
#endif
