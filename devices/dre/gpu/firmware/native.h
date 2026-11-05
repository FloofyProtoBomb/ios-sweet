/* SPDX-License-Identifier: MIT -- UNVALIDATED optional XNU firmware source. */
#pragma once
#include "package.h"
#include "region.h"
#include "relocation.h"
#include "../a619_pas13.h"
#ifdef __cplusplus
extern "C" {
#endif
struct dre_gpu_fw_native;
struct dre_gpu_fw_native_ops {
    void *cookie;
    uint64_t lease;
    /* EXACT1 only for real held providers, never presence flags. */
    int (*owns_pas13)(void *,uint64_t,const struct dre_gpu_fw_region *);
    int (*scm64_discovered)(void *);
    int (*scm_clocks_held)(void *,uint64_t);
    int (*normal_nc)(void *,const void *,uint64_t,size_t);
    int (*consumers_retired)(void *,uint64_t);
    struct dre_a619_pas13_ops scm; /* actual serialized SCM transport */
    /* V2 only, EXACT1: genuinely held PAS13/XPU/security/clock/reset/source
     * authority for the signed relocation to this independently owned boot
     * image slice. The original DT GPU/IPA region is identity only; no
     * ownership, alias or permission over its neighbors is implied. Must
     * retain actual security references and this boot image through PAS
     * shutdown +all consumers/cache/DMA retirement. Absent callback refuses.
     * Format/alignment, a DT name or SCM-supported query cannot prove it. */
    int (*owns_relocated_pas13)(void *,uint64_t,const struct dre_gpu_fw_region *,
        const struct dre_gpu_fw_relocation *,uint64_t image_pa,uint64_t image_bytes);
};
/* The actual privately compiled kernel anchor is required. No early VFS,
 * identifier access, GPU register access or SCM call occurs during load. */
const char *dre_gpu_fw_native_load(struct dre_gpu_fw_native **);
int dre_gpu_fw_native_source(struct dre_gpu_fw_native *,enum dre_gpu_fw_kind,
    struct dre_gpu_fw_view *);
/* Exact SQE full-file identity callback suitable for real CP binding. */
int dre_gpu_fw_native_sqe_verified(struct dre_gpu_fw_native *,const void *,size_t);
const struct dre_gpu_fw_region *dre_gpu_fw_native_region(struct dre_gpu_fw_native *);
/* Prepare all actual metadata/destination mappings BEFORE cold CP starts.
 * V1 shared GPU/IPA granule still refuses. V2 uses only the separate checked
 * boot-held slice and mandatory real relocated-PAS authority above. */
const char *dre_gpu_fw_native_prepare_pas13(struct dre_gpu_fw_native *,
    const struct dre_gpu_fw_native_ops *);
/* Called by real CP authenticate_zap AFTER ME_INIT, at the existing phase. */
const char *dre_gpu_fw_native_authenticate(struct dre_gpu_fw_native *);
const char *dre_gpu_fw_native_shutdown(struct dre_gpu_fw_native *);
/* Requires real source/CP/firmware-DMA consumers retired, successful PAS
 * shutdown and no uncertain SMC. Original low-DDR image remains boot-held. */
const char *dre_gpu_fw_native_release(struct dre_gpu_fw_native *);
#ifdef __cplusplus
}
#endif
