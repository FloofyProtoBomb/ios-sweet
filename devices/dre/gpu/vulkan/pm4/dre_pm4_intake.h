/* SPDX-License-Identifier: MIT
 * Locally checked structural subset. No native execution.
 * Kernel-private, portable C structural intake. Not an IOUserClient UAPI.
 * A successful subset result NEVER authorizes RAW_IB_SUBMIT or GPU execution.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "a619_pm4_policy.h"
#ifdef __cplusplus
extern "C" {
#endif

#define DRE_PM4_MAX_BOS 64u
#define DRE_PM4_MAX_ROOTS 16u
#define DRE_PM4_MAX_PROTECTED 32u
#define DRE_PM4_MAX_SNAPSHOTS 32u
#define DRE_PM4_MAX_DEPTH 8u
#define DRE_PM4_MAX_SINGLE_WORDS 16384u
#define DRE_PM4_MAX_TOTAL_WORDS 65536u
#define DRE_PM4_SNAPSHOT_PAGE_BYTES 16384u
#define DRE_PM4_MAX_ARENA_WORDS 262144u /* 1MiB including per-snapshot padding */
#define DRE_PM4_MAX_SNAPSHOT_PAGES 64u
#define DRE_PM4_MAX_PACKETS 16384u
#define DRE_PM4_MAX_REFERENCES 512u
#define DRE_PM4_MAX_EDGES 64u
#define DRE_PM4_MAX_INLINE_WRITE_WORDS 1024u
#define DRE_PM4_READ 1u
#define DRE_PM4_WRITE 2u
#define DRE_PM4_EXECUTE 4u

enum dre_pm4_status {
    DRE_PM4_STRUCTURAL_SUBSET_ONLY = 0,
    DRE_PM4_BAD_INPUT,
    DRE_PM4_WRONG_CHIP,
    DRE_PM4_OWNERSHIP_UNPROVEN,
    DRE_PM4_WORKSPACE_BUSY,
    DRE_PM4_LIMIT,
    DRE_PM4_SNAPSHOT_FAILED,
    DRE_PM4_MUTABLE_SNAPSHOT,
    DRE_PM4_BAD_HEADER,
    DRE_PM4_TRUNCATED_PACKET,
    DRE_PM4_BAD_PACKET_SHAPE,
    DRE_PM4_PRIVILEGED_PACKET,
    DRE_PM4_PRIVILEGED_REGISTER,
    DRE_PM4_UNSUPPORTED_REGISTER_PACKET,
    DRE_PM4_UNSUPPORTED_PACKET,
    DRE_PM4_BAD_ADDRESS,
    DRE_PM4_PROTECTED_ADDRESS,
    DRE_PM4_RESOURCE_NOT_LISTED,
    DRE_PM4_RESOURCE_ACCESS,
    DRE_PM4_IB_CYCLE,
    DRE_PM4_REPEATED_OR_OVERLAPPING_IB
};
/* All descriptors are copied/resolved kernel objects, never user-provided VA
 * authority. owns_resource must prove the actual task/session lease and pins. */
struct dre_pm4_resource {
    uint64_t handle, gpu_va, bytes;
    uint32_t granted_access;  /* READ plus optional WRITE or EXECUTE, no W+X */
    uint32_t submitted_access; /* READ/WRITE only, subset of granted_access */
};
struct dre_pm4_root { uint64_t handle, offset; uint32_t words, reserved; };
struct dre_pm4_range { uint64_t gpu_va, bytes; };
struct dre_pm4_request {
    uint32_t chip_id, flags; /* flags must be zero; no experimental bypass */
    uint64_t task_generation;
    const struct dre_pm4_resource *resources;
    uint32_t resource_count;
    const struct dre_pm4_root *roots;
    uint32_t root_count;
    const struct dre_pm4_range *protected_ranges;
    uint32_t protected_count;
};
struct dre_pm4_snapshot {
    uint64_t source_handle, source_offset, source_gpu_va, snapshot_lease;
    uint32_t words, arena_word;
    uint32_t state, reserved; /* state:1 reserved/partial,2 sealed,3 walked */
};
enum dre_pm4_reference_kind { DRE_PM4_MEMORY_READ = 1, DRE_PM4_MEMORY_WRITE = 2, DRE_PM4_IB_READ = 3 };
struct dre_pm4_reference {
    uint64_t handle, offset, bytes;
    uint32_t snapshot, packet_word, address_word, kind;
};
/* Materialization MUST rewrite each nested address to separately kernel-owned
 * GPU executable snapshot storage. Never publish the original user BO address
 * merely because this graph parsed successfully. Unchanged packet count and
 * immutable command copies must survive through hardware/cache/TLB retirement. */
struct dre_pm4_edge {
    uint32_t parent_snapshot, packet_word, child_snapshot, address_word;
};
struct dre_pm4_workspace {
    uint32_t *word_arena;
    uint32_t word_capacity;
    struct dre_pm4_snapshot *snapshots;
    uint32_t snapshot_capacity;
    struct dre_pm4_reference *references;
    uint32_t reference_capacity;
    struct dre_pm4_edge *edges;
    uint32_t edge_capacity;
    /* Set before first snapshot attempt. Caller cannot reuse/reset this work
     * until all acquired/partial snapshot leases are safely disposed. */
    uint32_t in_use;
};
struct dre_pm4_result {
    enum dre_pm4_status status;
    uint32_t raw_submission_authorized; /* ALWAYS zero, including success */
    uint64_t task_generation;
    uint32_t snapshots, arena_words, copied_words, snapshot_pages;
    uint32_t references, edges, packets, roots;
    uint32_t root_snapshot[DRE_PM4_MAX_ROOTS];
    uint32_t failure_snapshot, failure_word, failure_header, failure_opcode_or_register;
};
struct dre_pm4_ops {
    void *cookie;
    /* Every proof callback returns exactly1 for a proven claim,0 for refusal.
     * Negative error/status values are not truthy success. owns_resource must
     * prove real pinned ordinary RAM and exclude secure/MMIO/boot aliases. */
    int (*owns_resource)(void *, uint64_t task_generation, const struct dre_pm4_resource *);
    /* Must prove complete actual bootstrap ring/SQE/powerup/page-table/
     * marker/protected mappings for this VM, not accept a caller ready flag. */
    int (*owns_protected_ranges)(void *, uint64_t, const struct dre_pm4_range *, uint32_t);
    int (*owns_workspace)(void *, const void *, size_t);
    /* Copy actual pinned BO content into destination, then seal the private
     * CPU snapshot against task/GPU/other-thread mutation. Return1 only for a
     * complete snapshot and set a unique nonzero lease. On partial failure expose ANY
     * retained lease in *lease; the result keeps that slot and arena reserved.
     * No hidden retained lease may be lost when this callback returns failure.
     * The callback may not publish GPU commands or reuse protected boot pages. */
    int (*snapshot)(void *, uint64_t, const struct dre_pm4_resource *, uint64_t,
        uint32_t *destination, uint32_t words, uint64_t *lease);
    int (*owns_immutable_snapshot)(void *, uint64_t lease, const uint32_t *, size_t);
};

enum dre_pm4_status dre_pm4_intake(const struct dre_pm4_request *,
    const struct dre_pm4_ops *, struct dre_pm4_workspace *, struct dre_pm4_result *);
/* WORKSPACE_BUSY or invalid/aliased result storage leaves the previous result
 * untouched so its lease cleanup metadata is not destroyed by a second call.
 * The returned status must always be checked; no result authorizes raw work. */
const char *dre_pm4_status_name(enum dre_pm4_status);

/* Produce one relocated CPU staging copy from a successful sealed intake.
 * The owner must keep the original workspace, mappings and pins held across
 * this call. Each GPU VA is the base of a distinct private 16KiB-aligned
 * destination allocation. The provider must separately prove those mappings,
 * copy and seal the output for device reads, and hold it through retirement.
 * No GPU mapping, write, publication or RAW authorization occurs here.
 * On refusal the staging output is unchanged. */
enum dre_pm4_status dre_pm4_relocate_snapshot(const struct dre_pm4_workspace *,
    const struct dre_pm4_result *, const struct dre_pm4_ops *,
    const uint64_t *gpu_vas, uint32_t va_count, uint32_t snapshot,
    uint32_t *output, uint32_t output_words);
#ifdef __cplusplus
}
#endif
