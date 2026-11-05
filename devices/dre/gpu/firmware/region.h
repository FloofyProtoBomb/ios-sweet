/* SPDX-License-Identifier: MIT -- UNVALIDATED live-FDT geometry resolver. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_GPU_FW_REGION_SUBPAGE 1u
#define DRE_GPU_FW_REGION_ADJACENT_RESERVED 2u
struct dre_gpu_fw_region {
    uint64_t pa,bytes,cpu_granule_pa,cpu_granule_bytes;
    uint32_t phandle,limitations;
};
/* FDT must already pass fdt_check_full. Finds real PAS13->memory-region->
 * fixed no-map node. It describes ownership requirements, never acquires
 * PAS/GPU/IPA ownership or authorizes writes to reserved memory. */
const char *dre_gpu_fw_reserved_region(const void *,struct dre_gpu_fw_region *);
#ifdef __cplusplus
}
#endif
