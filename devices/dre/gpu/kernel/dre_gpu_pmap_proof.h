/* SPDX-License-Identifier: MIT; UNVALIDATED optional native proof ABI. */
#ifndef DRE_GPU_PMAP_PROOF_H
#define DRE_GPU_PMAP_PROOF_H
#include <stdint.h>
#include <kern/task.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Definitions live in dre_gpu_nc_pmap.inc, included at the END of the actual
 * osfmk/arm/pmap/pmap.c translation unit by an explicit optional profile.
 * page_metadata is private owned kernel storage with a u64 PA at each stride.
 * No caller-provided cache flag or virtual->PA claim substitutes for PTEs. */
int dre_gpu_pmap_verify_kernel_nc(uint64_t kernel_address,
    const void *page_metadata,uint32_t count,uint32_t stride);
int dre_gpu_pmap_verify_task_nc(task_t,uint64_t task_address,
    const void *page_metadata,uint32_t count,uint32_t stride,uint32_t cpu_access);
#ifdef __cplusplus
}
#endif
#endif
