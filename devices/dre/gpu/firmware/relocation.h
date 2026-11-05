/* SPDX-License-Identifier: MIT -- source-qualified plan, not PAS authority. */
#ifndef DRE_GPU_FW_RELOCATION_H
#define DRE_GPU_FW_RELOCATION_H
#include "package.h"
#include "../a619_zap.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_GPU_FW_RELOCATION_HANDOFF_BYTES 96u
#define DRE_GPU_FW_RELOCATION_ALIGNMENT (1024u*1024u)
#define DRE_GPU_FW_RELOCATION_BYTES 16384u
#define DRE_GPU_FW_RELOCATION_STORAGE_BYTES (DRE_GPU_FW_RELOCATION_ALIGNMENT+DRE_GPU_FW_RELOCATION_BYTES)
struct dre_gpu_fw_relocation {
    struct dre_a619_zap_plan zap;
    uint64_t load_pa,load_bytes,metadata_pa,metadata_bytes;
    uint32_t entry_offset,reserved;
};
/* Bounds/format/entry validation only. It supplies no physical ownership,
 * signed-firmware authenticity, PAS/XPU permission, cache or retirement proof.
 * The genuine chosen destination must be supplied; no guessed DT carveout. */
const char *dre_gpu_fw_relocation_plan(const void *file,size_t file_bytes,
    uint64_t load_pa,size_t load_bytes,uint64_t metadata_pa,size_t metadata_bytes,
    struct dre_gpu_fw_relocation *);
/* Explicit v2 handoff: magic,version/bytes,packagePA/bytes,metadataPA/bytes,
 * chip,loadPA/bytes,loadAlignment,reserved0,CRC32(first88bytes). Every byte of
 * the separate whole-granule destination remains inside the boot-held image.
 * The legacy64-byte handoff/validator remains unchanged. */
int dre_gpu_fw_relocation_handoff_valid(const uint64_t firmware[12],
    const uint64_t storage[8]);
/* Runtime-address slice arithmetic only. Caller must separately prove the
 * actual dedicated initialized object/image/DDR ownership and exclusions. */
int dre_gpu_fw_relocation_slice(uint64_t storage_pa,uint64_t storage_bytes,
    uint64_t image_pa,uint64_t image_bytes,uint64_t *load_pa);
#ifdef __cplusplus
}
#endif
#endif
