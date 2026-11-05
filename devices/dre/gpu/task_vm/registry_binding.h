/* SPDX-License-Identifier: MIT -- UNVALIDATED private inventory observation. */
#ifndef DRE_TASKVM_REGISTRY_BINDING_H
#define DRE_TASKVM_REGISTRY_BINDING_H
#include <stddef.h>
#include <stdint.h>
#include "../kernel/allocation_catalog.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_TASKVM_REGISTRY_OBSERVATION_VERSION 1u
enum dre_taskvm_registry_owner_relation {
    DRE_TASKVM_REGISTRY_KERNEL_ALLOCATION=0,
    DRE_TASKVM_REGISTRY_TARGET_CONNECTION=1,
    DRE_TASKVM_REGISTRY_FOREIGN_CONNECTION=2
};
struct dre_taskvm_registry_binding {
    /* Actual provider alone populates/expires this borrowed scope. Caller
     * retains the entire private wired allocation throughout the call. */
    struct dre_gpu_allocation_catalog_snapshot borrowed;
};
struct dre_taskvm_registry_workspace {
    uint64_t *sorted_pa; /* private wired CPU scratch, no task/GPU aliases */
    uint32_t page_capacity,reserved;
};
/* Historical observations only. No allocation/descriptor/CPU/page-list pointer,
 * physical address, proposed leaf or callable provider reference is exported.
 * Mapping_state0 means NO AUTHORITATIVE RECORD, not proof of being unmapped.
 * The visit pin expires at return. Mapping lease values can still describe a
 * real live mapping, but this historical copy retains no mapping authority. */
struct dre_taskvm_registry_record_observation {
    uint64_t allocation_generation,visit_pin_lease;
    uint64_t connection_generation,gpu_va,mapping_lease,mapping_generation;
    uint32_t purpose,granted_access,page_count,mapped_prefix_pages;
    uint32_t mapping_state,published,cpu_mapped,owner_relation;
};
struct dre_taskvm_registry_observation {
    uint32_t version,struct_bytes;
    uint64_t catalog_generation,common_lease,reset_epoch,connection_generation;
    uint64_t va_base;
    uint32_t coverage_kind,window_kind,window_pages,record_count,total_pages;
    uint32_t unknown_purpose_count,foreign_connection_count,no_mapping_record_count;
    struct dre_taskvm_registry_record_observation records[DRE_GPU_CATALOG_MAX_ALLOCATIONS];
};
/* Genuine synchronous descriptor-data inventory inspection, NOT a proposal
 * entrypoint. Caller holds the real service mutex. registered_client must be
 * the actual registered IOUserClient object; native service resolves its real
 * retained task and connection generation, never a supplied task integer.
 *
 * Binding/workspace/sorting/output must resolve actual immutable no-GPU
 * CATALOG_SCRATCH allocations, not live private control/table metadata.
 * Every page-list range is verified as actual borrowed
 * kernel-only metadata, without claiming its whole shared heap granule private.
 * EXACT TASKVM_REGISTRY_DATA_OBSERVED_PLANNER_REFUSED means these observations
 * passed the provider's complete synchronous descriptor-DATA visit, and the
 * global protected-inventory planner remains REFUSED. Other results require
 * ignoring all old caller output. Failure before genuine private-storage proof
 * can leave caller scratch untouched. No planner is invoked by this source.
 */
const char *dre_taskvm_inspect_registered_catalog(const struct dre_gpu_native_allocator *,
    const void *registered_client,uint64_t connection_generation,
    struct dre_taskvm_registry_binding *,struct dre_taskvm_registry_workspace *,
    struct dre_taskvm_registry_observation *);
#ifdef __cplusplus
}
#endif
#endif
