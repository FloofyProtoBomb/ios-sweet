/* SPDX-License-Identifier: MIT -- UNVALIDATED, synchronous kernel-only views. */
#ifndef DRE_GPU_ALLOCATION_CATALOG_H
#define DRE_GPU_ALLOCATION_CATALOG_H
#include "dre_gpu_provider.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_GPU_CATALOG_VERSION 1u
#define DRE_GPU_CATALOG_MAX_ALLOCATIONS 128u
#define DRE_GPU_CATALOG_MAX_PAGES 65536u
enum dre_gpu_catalog_window {
    DRE_GPU_CATALOG_WINDOW_ABSENT=0,
    DRE_GPU_CATALOG_BOOTSTRAP_SHARED_WINDOW=1 /* NOT task isolation */
};
enum dre_gpu_catalog_coverage { DRE_GPU_CATALOG_ALLOCATOR_DATA_ONLY=1 };
enum dre_gpu_catalog_mapping_state {
    DRE_GPU_CATALOG_NO_MAPPING_RECORD=0, /* unknown, NOT proof of no mapping */
    DRE_GPU_CATALOG_MAPPING_STAGED,
    DRE_GPU_CATALOG_MAPPING_ACTIVE,DRE_GPU_CATALOG_MAPPING_RETIRED
};
struct dre_gpu_allocation_catalog_record {
    const struct dre_gpu_native_allocation *allocation;
    struct dre_gpu_native_memory memory; /* full descriptor pages +padding */
    uint64_t allocation_generation,pin_lease;
    uint64_t task_identity,connection_generation;
    uint64_t gpu_va,mapping_lease,mapping_generation;
    uint32_t purpose,granted_access,mapped_prefix_pages,published;
    uint32_t cpu_mapped,reserved;
    uint32_t mapping_state,reserved2;
};
struct dre_gpu_allocation_catalog_snapshot {
    uint32_t version,struct_bytes;
    uint64_t scope_token,catalog_generation,common_lease,reset_epoch;
    uint64_t task_identity,connection_generation;
    uint64_t va_base;
    uint32_t window_pages,window_kind,record_count,total_pages;
    /* Exact descriptor-backed allocator DATA inventory only. Shared IOMalloc
     * registry/page metadata granules and private reserved firmware are NOT
     * certified as a complete protected global physical inventory. */
    uint32_t coverage_kind,reserved;
    struct dre_gpu_allocation_catalog_record records[DRE_GPU_CATALOG_MAX_ALLOCATIONS];
};
/* Internal arithmetic/shape helpers. Actual descriptor/class/task/lock/ref
 * authority resides exclusively in DREAdrenoGPU.cpp, never these helpers. */
int dre_gpu_catalog_kernel_purpose(uint32_t purpose,uint32_t granted_access);
int dre_gpu_catalog_memory_same(const struct dre_gpu_native_memory *,const struct dre_gpu_native_memory *);
#ifdef __cplusplus
}
#endif
#endif
