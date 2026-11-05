/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#define DRE_STORAGE_MAGIC UINT64_C(0x0031534655455244) /* DREUFS1\0 */
#define DRE_STORAGE_ARENA 0x4000u
/* LE u64: magic, arena PA/size, live FDT PA/size, owned image PA/size, CRC32.
 * This transfers memory ownership, never an assertion that DMA is safe. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t dre_storage_crc(const void *bytes, unsigned size);
int dre_storage_abi_valid(const uint64_t words[8]);
#ifdef __cplusplus
}
#endif
