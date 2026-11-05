/* SPDX-License-Identifier: MIT -- UNVALIDATED optional XNU firmware source. */
#pragma once
#include "package.h"
#include "region.h"
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
};
/* The actual privately compiled kernel anchor is required. No early VFS,
 * identifier access, GPU register access or SCM call occurs during load. */
const char *dre_gpu_fw_native_load(struct dre_gpu_fw_native **);
int dre_gpu_fw_native_source(struct dre_gpu_fw_native *,enum dre_gpu_fw_kind,
    struct dre_gpu_fw_view *);
/* Exact SQE full-file identity callback suitable for real CP binding. */
int dre_gpu_fw_native_sqe_verified(struct dre_gpu_fw_native *,const void *,size_t);
const struct dre_gpu_fw_region *dre_gpu_fw_native_region(struct dre_gpu_fw_native *);
/* Prepare all actual metadata/reserved mappings BEFORE cold CP starts. The
 * known8KiB GPU/IPA shared16KiB granule explicitly refuses this mapper. */
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
