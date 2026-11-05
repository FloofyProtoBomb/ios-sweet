/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight source. */
#pragma once
#include "../kernel/dre_gpu_provider.h"
#ifdef __cplusplus
extern "C" {
#endif
enum dre_gpu_boot_slot {
    DRE_GPU_BOOT_TABLES=0,DRE_GPU_BOOT_PAGES,DRE_GPU_BOOT_LEASES,DRE_GPU_BOOT_REVERSE,
    DRE_GPU_BOOT_RING,DRE_GPU_BOOT_SQE,DRE_GPU_BOOT_SCRATCH,DRE_GPU_BOOT_POWERUP,
    DRE_GPU_BOOT_RESULT,DRE_GPU_BOOT_SLOTS
};
struct dre_gpu_boot_buffer {
    struct dre_gpu_native_allocation *allocation;
    struct dre_gpu_native_memory memory;
};
struct dre_gpu_boot_memory {
    struct dre_gpu_native_allocator allocator;
    struct dre_a619_vm_ops ops;
    struct dre_a619_vm vm;
    struct dre_a619_smmu domain;
    struct dre_a619_queue queue;
    struct dre_a619_cp cp;
    struct dre_gpu_boot_buffer buffer[DRE_GPU_BOOT_SLOTS];
    struct dre_a619_vm_mapping mapping[5];
    struct dre_a619_cp_buffer expected[5];
    uint32_t expected_permissions[5],held,attempted,initialized,unsafe;
    uint64_t first_unused_gpu_va;
    const char *error;
};
/* Prepare real owned RAM, table storage and five CP boot mappings before
 * startup activation. No hardware is initialized here. The actual native
 * owner must recognize this wired private control object in ops->owns_cpu;
 * its allocator registry proves the remaining arrays, tables and buffers.
 * reservation supplies an actual exclusively reserved CB/SMR/ASID/global
 * owner with exactly CB_OWNED|ASID_OWNED prerequisites. Those reservation
 * facts may not come from DT presence. Fresh TABLES_NC is derived only after
 * the real new allocation proof. No powered/global-init/quiescent assertion
 * is accepted before actual startup's post-power qualify_domain callback.
 * The immutable SQE full file must already pass the private firmware anchor.
 * This helper still parses its actual prefix/code length independently.
 * All allocations remain held on failure; caller may discard them only while
 * actual native ownership proves they were never published to any device. */
const char *dre_gpu_boot_memory_prepare(struct dre_gpu_boot_memory *,
    const struct dre_gpu_native_allocator *,const struct dre_a619_vm_ops *,
    const struct dre_a619_smmu *reservation,uint32_t window_pages,
    const void *trusted_sqe_file,size_t trusted_sqe_bytes);
/* Exact contiguous table prefix / real CP mapping proof adapters. A native
 * context can forward its existing VM/CP callbacks here. No generic CPU/PA
 * range, requested cache option or lookup in DT grants ownership. */
int dre_gpu_boot_table_mapping(struct dre_gpu_boot_memory *,uint64_t pa,
    const void *cpu,size_t bytes);
int dre_gpu_boot_cp_mapping(struct dre_gpu_boot_memory *,
    const struct dre_a619_cp_buffer *,uint32_t permissions);
/* Install only the real prepared VM/domain/CP pointers and matching MMU ops.
 * Platform, RBBM, PAS/firmware, cold/live guards and policies stay supplied by
 * their actual native owner. Does not set a ready mask or call startup. */
const char *dre_gpu_boot_memory_bind(struct dre_gpu_boot_memory *,
    struct dre_a619_start_plan *);
#ifdef __cplusplus
}
#endif
