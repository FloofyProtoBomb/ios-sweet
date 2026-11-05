/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
/* Input FDT must already pass fdt_check_full. Deny every reserved interval,
 * including dynamic alloc-ranges, and all malformed/overflowing ranges. */
int dre_dma_ram_allowed(const void *dt, uint64_t address, uint64_t size);
