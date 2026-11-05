/* SPDX-License-Identifier: MIT
 * UNVALIDATED source-only private cache packet planner. No execution API.
 */
#ifndef DRE_A619_CACHE_H
#define DRE_A619_CACHE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_CACHE_PLAN_VERSION 1u
#define DRE_CACHE_MAX_WORDS 64u
#define DRE_CACHE_MAX_PACKETS 32u
enum dre_cache_status {
    DRE_CACHE_PLAN_AUTHORED=0,DRE_CACHE_INVALID=-1,DRE_CACHE_UNSUPPORTED=-2,
    DRE_CACHE_OWNER_REQUIRED=-3,DRE_CACHE_NO_SPACE=-4
};
enum dre_cache_domain {
    DRE_CACHE_UCHE=1u,DRE_CACHE_CCU_COLOR=2u,DRE_CACHE_CCU_DEPTH=4u,
    /* Named so they can fail explicitly, not alias a broad UCHE flush. */
    DRE_CACHE_LRZ=8u,DRE_CACHE_GMEM_CONTENTS=16u,DRE_CACHE_SHADER_STATE=32u
};
enum dre_cache_phase {DRE_CACHE_PRE_READ=1,DRE_CACHE_POST_WRITE=2};
enum dre_cache_producer {DRE_CACHE_PRIVATE_CP=1,DRE_CACHE_COMPUTE=2,DRE_CACHE_SYSMEM_RENDER=3};
enum dre_cache_ccu {DRE_CACHE_CCU_NEVER_USED=1,DRE_CACHE_CCU_SYSMEM=2,DRE_CACHE_CCU_GMEM=3};
enum dre_cache_descriptor {DRE_CACHE_CS_BINDLESS=1u,DRE_CACHE_GFX_BINDLESS=2u};
enum dre_cache_required_context {
    DRE_CACHE_CONTEXT_OWNER_DOMAIN_QUEUE=1u,
    DRE_CACHE_CONTEXT_COMPLETE_HISTORY=2u,
    DRE_CACHE_CONTEXT_PRIVATE_PINNED_NC_PAGE=4u,
    DRE_CACHE_CONTEXT_CPU_STORES_ORDERED=8u,
    DRE_CACHE_CONTEXT_SYSMEM_BINDINGS=16u,
    DRE_CACHE_CONTEXT_NO_PRIOR_RENDER=32u,
    DRE_CACHE_CONTEXT_DESCRIPTOR_REEMIT=64u
};
enum dre_cache_required_observation {
    DRE_CACHE_OBSERVE_FRESH_VALUE_FENCE=1u,
    DRE_CACHE_OBSERVE_WHERE_AM_I=2u,
    DRE_CACHE_OBSERVE_QUEUE_AND_ROQ_DRAIN=4u,
    DRE_CACHE_OBSERVE_FAULT_FREE=8u,
    DRE_CACHE_OBSERVE_SAME_LIVE_OWNER=16u,
    DRE_CACHE_OBSERVE_NC_CPU_READ_ORDER=32u,
    DRE_CACHE_OBSERVE_FULL_RESOURCE_REFS=64u,
    DRE_CACHE_OBSERVE_SHADER_PIPELINE_REFS=128u,
    DRE_CACHE_OBSERVE_LAZY_LOAD_REFS=256u,
    DRE_CACHE_OBSERVE_CCU_BINDINGS=512u,
    DRE_CACHE_OBSERVE_MAPPING_TLB_RETIREMENT=1024u,
    DRE_CACHE_OBSERVE_CCU_EVENT_RETIREMENT=2048u,
    DRE_CACHE_OBSERVE_IMMUTABLE_COMMAND_COPY=4096u
};
struct dre_cache_request {
    uint32_t version,struct_bytes,chip_id,phase;
    uint64_t owner_generation,domain_generation,queue_generation,state_generation;
    uint32_t producer,ccu_state,conditional_depth,renderpass_closed;
    uint32_t clean_domains,invalidate_domains,descriptor_stages,descriptor_reload_follows;
    uint32_t lrz_disabled_and_clean,gmem_contents_dirty,sequence,dirty_domains;
    uint32_t reserved[2];
};
/* Actual provider-minted private16KiB completion allocation. No task mapping,
 * imported surface, user BO or caller physical address is accepted. Real owner
 * callback must verify pins, NC aliases, WRITE GPU permission, canaries,
 * disjointness from command/bootstrap/table/other live storage and a fresh
 * monotonic sequence/reset baseline. The planner itself cannot prove those. */
struct dre_cache_private_page {
    uint64_t gpu,bytes,allocation_lease,domain_generation;
};
struct dre_cache_plan_ops {
    void *cookie;
    /* EXACT1 only after actual ownership/history/state validation, including
     * the actual dirty-domain ledger, never just request flags. Same real
     * owner+VM+queue lifetime lock is held throughout planning/publication.
     * There are no default/stub callbacks and no callers from user input. */
    int (*verify_context)(void *,const struct dre_cache_request *,uint32_t required_context);
    /* Describe already owned/prepared storage; do not allocate/release a page
     * or submit/change hardware from a planner callback. Page offset0 is an
     * event dummy word, +4 the unique value fence, +8 the
     * WHERE_AM_I ack. Before publication provider must actually establish
     * fence=~sequence, ack=UINT32_MAX and its own full-page canaries under NC
     * ordering. An observed old value can never satisfy a new boundary. */
    int (*private_page)(void *,const struct dre_cache_request *,struct dre_cache_private_page *);
};
struct dre_cache_plan {
    uint32_t version,word_count,packet_count,required_context;
    uint32_t planned_clean,planned_invalidate,descriptor_stages,required_observations;
    uint64_t owner_generation,domain_generation,queue_generation,state_generation;
    uint64_t private_page_gpu,private_page_lease;
    uint32_t fence_sequence,fence_byte_offset,ack_byte_offset,reserved;
    /* Boundaries let the real queue publish without splitting a packet across
     * ring wrap. Last entry is the word count, never a hardware completion. */
    uint16_t packet_offsets[DRE_CACHE_MAX_PACKETS+1];
};
/* Authored private words only: never submits MMIO, waits, observes hardware,
 * modifies a cache ledger, accepts user PM4, grants capabilities or retires a
 * reference. After disjoint argument validation, plan is zeroed on failure;
 * caller words stay unchanged and cannot be used without a nonempty authored
 * plan. Invalid pointers/overlapping inputs are not modified. Caller output is
 * scratch, not a GPU executable object: a real executor must copy/seal the exact
 * words into private immutable pinned storage before publication. A plan stays
 * immutable/pinned under its owner until real execution/observations finish.
 * LRZ/GMEM/unknown shader-state/conditional histories explicitly refuse.
 */
enum dre_cache_status dre_a619_cache_plan(const struct dre_cache_request *,
    const struct dre_cache_plan_ops *,uint32_t *words,size_t capacity_words,
    struct dre_cache_plan *);
#ifdef __cplusplus
}
#endif
#endif
