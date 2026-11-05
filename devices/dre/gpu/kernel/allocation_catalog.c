/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only registry helpers. */
#include "allocation_catalog.h"
int dre_gpu_catalog_kernel_purpose(uint32_t purpose,uint32_t access)
{
    switch(purpose) {
    case DRE_GPU_ALLOC_RING:case DRE_GPU_ALLOC_IMMUTABLE_IB:return access==DRE_GPU_ACCESS_READ;
    case DRE_GPU_ALLOC_SQE_UCODE:return access==(DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_EXECUTE);
    case DRE_GPU_ALLOC_BOOT_TABLES:case DRE_GPU_ALLOC_CPU_METADATA:
    case DRE_GPU_ALLOC_PRIVATE_CONTROL:case DRE_GPU_ALLOC_CP_SCRATCH:
    case DRE_GPU_ALLOC_CP_POWERUP:case DRE_GPU_ALLOC_PRIVATE_COMPLETION:
    case DRE_GPU_ALLOC_TRUSTED_STATE:case DRE_GPU_ALLOC_CATALOG_SCRATCH:
        return access==(DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_WRITE);
    default:return 0; /* UNKNOWN and TASK_BO cannot be assigned by constructors */
    }
}
int dre_gpu_catalog_memory_same(const struct dre_gpu_native_memory *a,const struct dre_gpu_native_memory *b)
{
    return a&&b&&a->cpu==b->cpu&&a->bytes==b->bytes&&a->pages==b->pages&&
        a->page_count==b->page_count&&a->cache_contract==b->cache_contract;
}
