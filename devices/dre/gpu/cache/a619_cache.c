/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Source-derived bounded A6XX cache-boundary builder only.
 * Primary source order/scope recorded in sources.json and README.md.
 */
#include "a619_cache.h"
#include "../a619_backend.h"

struct writer {
    uint32_t words[DRE_CACHE_MAX_WORDS],at,packets,failed;
    uint16_t offsets[DRE_CACHE_MAX_PACKETS+1];
};
static void clear(void *value,size_t bytes)
{unsigned char *p=value;while(bytes--)*p++=0;}
static unsigned parity(unsigned n)
{unsigned odd=1;while(n){odd^=n&1u;n>>=1;}return odd;}
static int begin(struct writer *w,uint32_t header,uint32_t words)
{
    if(w->failed||w->packets==DRE_CACHE_MAX_PACKETS||words>DRE_CACHE_MAX_WORDS-w->at) {
        w->failed=1;return 0;
    }
    w->offsets[w->packets++]=(uint16_t)w->at;w->words[w->at++]=header;return 1;
}
static void wait0(struct writer *w,uint32_t op)
{(void)begin(w,dre_a619_type7(op,0),1);}
static void event(struct writer *w,uint32_t value)
{if(begin(w,dre_a619_type7(0x46,1),2))w->words[w->at++]=value;}
static void event32(struct writer *w,uint32_t value,uint64_t address,uint32_t sequence)
{
    if(!begin(w,dre_a619_type7(0x46,4),5))return;
    w->words[w->at++]=value;
    w->words[w->at++]=(uint32_t)address;w->words[w->at++]=(uint32_t)(address>>32);
    w->words[w->at++]=sequence;
}
static void descriptors(struct writer *w,uint32_t stages)
{
    uint32_t value=0;
    if(stages&DRE_CACHE_CS_BINDLESS)value|=0x1fu<<9;
    if(stages&DRE_CACHE_GFX_BINDLESS)value|=0x1fu<<14;
    /* Fixed A6XX SP_UPDATE_CNTL only. These bits discard pending bindless
     * loads; they do NOT flush SP shader writes or invalidate every cache. */
    const uint32_t reg=0xbb08,count=1;
    if(!begin(w,0x40000000u|count|(parity(count)<<7)|(reg<<8)|(parity(reg)<<27),2))return;
    w->words[w->at++]=value;
}
static void ack(struct writer *w,uint64_t address)
{
    if(!begin(w,dre_a619_type7(0x62,2),3))return;
    w->words[w->at++]=(uint32_t)address;w->words[w->at++]=(uint32_t)(address>>32);
}
static int overlaps(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    if(an>UINTPTR_MAX-x||bn>UINTPTR_MAX-y)return 1;
    return x<y+bn&&y<x+an;
}
enum dre_cache_status dre_a619_cache_plan(const struct dre_cache_request *r,
    const struct dre_cache_plan_ops *ops,uint32_t *out,size_t capacity,struct dre_cache_plan *plan)
{
    if(!r||!plan||!out||capacity>DRE_CACHE_MAX_WORDS||!capacity||
       overlaps(out,capacity*sizeof(*out),plan,sizeof(*plan))||
       overlaps(r,sizeof(*r),plan,sizeof(*plan))||overlaps(r,sizeof(*r),out,capacity*sizeof(*out))||
       (ops&&(overlaps(ops,sizeof(*ops),plan,sizeof(*plan))||overlaps(ops,sizeof(*ops),out,capacity*sizeof(*out)))))
        return DRE_CACHE_INVALID;
    struct dre_cache_request request=*r;r=&request;
    clear(plan,sizeof(*plan));
    if(r->version!=DRE_CACHE_PLAN_VERSION||r->struct_bytes!=sizeof(*r)||r->chip_id!=DRE_A619_CHIP_ID||
       !r->owner_generation||!r->domain_generation||!r->queue_generation||!r->state_generation||
       (r->phase!=DRE_CACHE_PRE_READ&&r->phase!=DRE_CACHE_POST_WRITE)||
       r->producer<DRE_CACHE_PRIVATE_CP||r->producer>DRE_CACHE_SYSMEM_RENDER||
       r->renderpass_closed>1||r->descriptor_reload_follows>1||r->lrz_disabled_and_clean>1||
       r->gmem_contents_dirty>1||!r->sequence||r->reserved[0]||r->reserved[1]||(r->descriptor_stages&~3u))
        return DRE_CACHE_INVALID;
    const uint32_t requested=r->clean_domains|r->invalidate_domains;
    const uint32_t supported=DRE_CACHE_UCHE|DRE_CACHE_CCU_COLOR|DRE_CACHE_CCU_DEPTH;
    if((requested&~supported)||(r->dirty_domains&~supported)||(!requested&&!r->descriptor_stages)||r->conditional_depth||
       r->gmem_contents_dirty||!r->lrz_disabled_and_clean||
       (r->ccu_state!=DRE_CACHE_CCU_NEVER_USED&&r->ccu_state!=DRE_CACHE_CCU_SYSMEM))
        return DRE_CACHE_UNSUPPORTED;
    if(r->descriptor_stages&&(r->phase!=DRE_CACHE_PRE_READ||!r->descriptor_reload_follows))
        return DRE_CACHE_UNSUPPORTED;
    if(!r->descriptor_stages&&r->descriptor_reload_follows)return DRE_CACHE_INVALID;
    uint32_t ccu=requested&(DRE_CACHE_CCU_COLOR|DRE_CACHE_CCU_DEPTH);
    if((ccu||r->producer==DRE_CACHE_SYSMEM_RENDER)&&
       (r->ccu_state!=DRE_CACHE_CCU_SYSMEM||!r->renderpass_closed))return DRE_CACHE_UNSUPPORTED;
    /* Cleaning before invalidation is mandatory for CCU in Mesa. This narrow
     * conservative boundary also requires it for requested UCHE invalidation,
     * instead of discarding unknown dirty data from a guessed history. */
    uint32_t clean=r->clean_domains|r->invalidate_domains;
    /* The provider must prove its real ledger agrees. An unrequested dirty
     * CCU/LRZ/GMEM domain cannot disappear behind a UCHE-only fence. */
    if(r->dirty_domains&~(clean|DRE_CACHE_UCHE))return DRE_CACHE_UNSUPPORTED;
    uint32_t needs=DRE_CACHE_CONTEXT_OWNER_DOMAIN_QUEUE|DRE_CACHE_CONTEXT_COMPLETE_HISTORY|
        DRE_CACHE_CONTEXT_PRIVATE_PINNED_NC_PAGE|DRE_CACHE_CONTEXT_CPU_STORES_ORDERED;
    if(ccu||r->producer==DRE_CACHE_SYSMEM_RENDER)needs|=DRE_CACHE_CONTEXT_SYSMEM_BINDINGS;
    else needs|=DRE_CACHE_CONTEXT_NO_PRIOR_RENDER;
    if(r->descriptor_stages)needs|=DRE_CACHE_CONTEXT_DESCRIPTOR_REEMIT;
    if(!ops||!ops->verify_context||!ops->private_page||ops->verify_context(ops->cookie,r,needs)!=1)
        return DRE_CACHE_OWNER_REQUIRED;
    struct dre_cache_private_page page={0};
    if(ops->private_page(ops->cookie,r,&page)!=1||!page.gpu||(page.gpu&16383)||page.bytes!=16384||
       page.gpu>=(1ull<<40)||page.bytes>(1ull<<40)-page.gpu||!page.allocation_lease||
       page.domain_generation!=r->domain_generation)return DRE_CACHE_OWNER_REQUIRED;
    struct writer w={0};
    wait0(&w,0x12); /* CP memory writes */
    wait0(&w,0x26); /* pipeline WFI, not a software idle boolean */
    wait0(&w,0x13); /* order CP/ME before changing boundary state */
    if(clean&DRE_CACHE_CCU_COLOR)event32(&w,0x1d,page.gpu,0);
    if(clean&DRE_CACHE_CCU_DEPTH)event32(&w,0x1c,page.gpu,0);
    if(r->invalidate_domains&DRE_CACHE_CCU_COLOR)event(&w,0x19);
    if(r->invalidate_domains&DRE_CACHE_CCU_DEPTH)event(&w,0x18);
    if(clean&DRE_CACHE_UCHE)event32(&w,0x04,page.gpu,0);
    if(r->invalidate_domains&DRE_CACHE_UCHE)event(&w,0x31);
    if(r->descriptor_stages)descriptors(&w,r->descriptor_stages);
    /* Mirrors the A6XX flush order's final CP-write/WFI/WFM boundary. */
    wait0(&w,0x12);wait0(&w,0x26);wait0(&w,0x13);
    /* Known supplied-value CACHE_FLUSH_TS fence, IRQ and timestamp-override
     * clear. It covers this boundary only, never future inserted shader work. */
    event32(&w,0x04,page.gpu+4,r->sequence);
    wait0(&w,0x26);wait0(&w,0x13);ack(&w,page.gpu+8);
    w.offsets[w.packets]=(uint16_t)w.at;
    /* The fixed maximum is39words/17packets; checks remain explicit for future
     * source changes. Nothing has been written to a ring or caller storage. */
    if(w.failed||w.at>DRE_CACHE_MAX_WORDS||w.packets>DRE_CACHE_MAX_PACKETS)return DRE_CACHE_INVALID;
    if(w.at>capacity)return DRE_CACHE_NO_SPACE;
    if(ops->verify_context(ops->cookie,r,needs)!=1)return DRE_CACHE_OWNER_REQUIRED;
    struct dre_cache_plan p={0};p.version=DRE_CACHE_PLAN_VERSION;
    p.word_count=w.at;p.packet_count=w.packets;p.required_context=needs;
    p.planned_clean=clean|DRE_CACHE_UCHE; /* final supplied-value fence also cleans UCHE */
    p.planned_invalidate=r->invalidate_domains;p.descriptor_stages=r->descriptor_stages;
    p.required_observations=DRE_CACHE_OBSERVE_FRESH_VALUE_FENCE|DRE_CACHE_OBSERVE_WHERE_AM_I|
        DRE_CACHE_OBSERVE_QUEUE_AND_ROQ_DRAIN|DRE_CACHE_OBSERVE_FAULT_FREE|DRE_CACHE_OBSERVE_SAME_LIVE_OWNER|
        DRE_CACHE_OBSERVE_NC_CPU_READ_ORDER|DRE_CACHE_OBSERVE_FULL_RESOURCE_REFS|DRE_CACHE_OBSERVE_MAPPING_TLB_RETIREMENT|
        DRE_CACHE_OBSERVE_IMMUTABLE_COMMAND_COPY;
    if(r->producer!=DRE_CACHE_PRIVATE_CP)p.required_observations|=DRE_CACHE_OBSERVE_SHADER_PIPELINE_REFS;
    if(r->descriptor_stages||r->producer!=DRE_CACHE_PRIVATE_CP)p.required_observations|=DRE_CACHE_OBSERVE_LAZY_LOAD_REFS;
    if(ccu||r->producer==DRE_CACHE_SYSMEM_RENDER)p.required_observations|=
        DRE_CACHE_OBSERVE_CCU_BINDINGS|DRE_CACHE_OBSERVE_CCU_EVENT_RETIREMENT;
    p.owner_generation=r->owner_generation;p.domain_generation=r->domain_generation;
    p.queue_generation=r->queue_generation;p.state_generation=r->state_generation;
    p.private_page_gpu=page.gpu;p.private_page_lease=page.allocation_lease;
    p.fence_sequence=r->sequence;p.fence_byte_offset=4;p.ack_byte_offset=8;
    for(uint32_t i=0;i<=w.packets;i++)p.packet_offsets[i]=w.offsets[i];
    for(uint32_t i=0;i<w.at;i++)out[i]=w.words[i];
    *plan=p;return DRE_CACHE_PLAN_AUTHORED;
}
