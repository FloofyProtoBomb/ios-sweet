/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only overnight draft.
 * Fixed private CP IB builders + real A619 ring/cache-event/fence protocol.
 * No shader/draw/CCU/LRZ state, scheduler, untrusted validator or Metal device.
 * Packet/ring facts: Mesa85fda3b36405bcaea8bffc64aa4f2651dc90770d XML,
 * Lineage98a38783f42f5588bf00cd933e299aedc589bc10 adreno_ringbuffer.c,
 * and Linuxv6.12 a6xx_gpu.c. See TRUSTED_QUEUE.md for exact scope. */
#include "trusted_queue.h"
#define REG(n) (DRE_A619_GPU_BASE+(uint64_t)(n)*4)
#define BUFFER_BYTES 16384u
#define BUFFER_PAGES 4u
#define IB_LIMIT 8u
#define RING_WORDS 23u
#define FENCE_INDEX 0u
#define ACK_INDEX 1u
#define DEST_INDEX 2u
#define SOURCE_INDEX 3u
#define CANARY(i) (UINT32_C(0x6195a5a5)^(uint32_t)(i))
struct dre_gpu_trusted_queue {
    struct dre_gpu_native_allocator allocator;
    struct dre_gpu_native_allocation *state_allocation,*ib_allocation,*data_allocation;
    struct dre_gpu_native_memory state_memory,ib_memory,data_memory;
    struct dre_a619_vm *vm;
    const struct dre_a619_vm_ops *vm_ops;
    struct dre_a619_vm_mapping ib_mapping,data_mapping;
    uint64_t ib_gpu,data_gpu,vm_generation,lease,ticket;
    uint64_t expected_submitted,expected_completed;
    struct dre_a619_startup *startup;
    struct dre_a619_queue *ring;
    uint32_t generation,next_sequence,sequence,operation,value,ib_words,first_allowed_page;
    uint32_t sealed_ib[IB_LIMIT],expected_end;
    uint32_t prepared,attached,pending,complete,unsafe;
    struct dre_gpu_trusted_completion observed;
};
_Static_assert(sizeof(struct dre_gpu_trusted_queue)<=BUFFER_BYTES,"owned private queue state bound");
static uint32_t next_generation=1;
/* One native A619 exists in this board profile. A claimed ring is never
 * released by a software flag after an uncertain hardware transition. */
static struct dre_gpu_trusted_queue *ring_owner;
static int equal(const char *a,const char *b)
{ if(!a||!b)return 0;while(*a&&*a==*b){a++;b++;}return *a==*b; }
static void clear(void *v,size_t n)
{ unsigned char *p=v;while(n--)*p++=0; }
static uint32_t generation(void)
{
    uint32_t old=__atomic_load_n(&next_generation,__ATOMIC_RELAXED);
    for(unsigned tries=0;tries<1024;tries++) {
        if(!old||old==UINT32_MAX)return 0;
        if(__atomic_compare_exchange_n(&next_generation,&old,old+1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return old;
    }
    return 0;
}
static const char *fail(struct dre_gpu_trusted_queue *s,const char *reason)
{
    s->unsafe=1;
    s->observed.state=DRE_GPU_TRUSTED_ERROR_RETAINED;
    s->observed.engine_references_drained=0;
    if(s->startup)dre_a619_startup_invalidate(s->startup,reason);
    return reason;
}
static int disjoint(const struct dre_gpu_native_memory *a,const struct dre_gpu_native_memory *b)
{
    if((uintptr_t)a->cpu<(uintptr_t)b->cpu+b->bytes&&
       (uintptr_t)b->cpu<(uintptr_t)a->cpu+a->bytes)return 0;
    /* All three private allocations are fixed16KiB; this is at most16
     * comparisons per pair, never a quadratic large-BO ownership scan. */
    for(uint32_t i=0;i<a->page_count;i++)for(uint32_t j=0;j<b->page_count;j++)
        if(a->pages[i].pa==b->pages[j].pa)return 0;
    return 1;
}
static int allocation(struct dre_gpu_trusted_queue *s,uint32_t access,
    enum dre_gpu_allocation_purpose purpose,
    struct dre_gpu_native_allocation **a,struct dre_gpu_native_memory *m)
{
    /* This private constructor has exactly two later allocation roles. No
     * caller-provided purpose reaches it and no user/unknown role is tagged.
     */
    if(purpose!=DRE_GPU_ALLOC_IMMUTABLE_IB&&purpose!=DRE_GPU_ALLOC_PRIVATE_COMPLETION)return 0;
    int status=s->allocator.allocate(s->allocator.cookie,BUFFER_BYTES,access,a,m);
    if(status!=DRE_GPU_OK||!*a||!m->cpu||!m->pages||((uintptr_t)m->cpu&16383)||
       (uintptr_t)m->cpu>UINTPTR_MAX-BUFFER_BYTES||m->bytes!=BUFFER_BYTES||
       m->page_count!=BUFFER_PAGES||m->cache_contract!=DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE||
       s->allocator.verify_memory(s->allocator.cookie,m)!=1)return 0;
    for(uint32_t i=0;i<BUFFER_PAGES;i++)if(m->pages[i].permissions!=access)return 0;
    return s->allocator.classify(s->allocator.cookie,*a,m,purpose)==DRE_GPU_OK;
}
static uint32_t free_range(const struct dre_gpu_trusted_queue *s)
{
    const struct dre_a619_vm *v=s->vm;
    uint32_t run=0;
    for(uint32_t i=s->first_allowed_page;i<v->domain->page_count;i++) {
        if(v->leases[i]||v->pages[i].pa||v->pages[i].permissions){run=0;continue;}
        if(!run&&(i&3))continue;
        if(++run==BUFFER_PAGES)return i+1-run;
    }
    return UINT32_MAX;
}
static int stage(struct dre_gpu_trusted_queue *s,struct dre_gpu_native_allocation *allocation,
    const struct dre_gpu_native_memory *m,
    struct dre_a619_vm_mapping *mapping,uint64_t *gpu)
{
    uint32_t first=free_range(s);if(first==UINT32_MAX)return 0;
    const char *status=dre_a619_vm_map(s->vm,s->vm_ops,first,m->pages,BUFFER_PAGES,mapping);
    if(!equal(status,"VM_MAPPING_STAGED")||!mapping->lease||mapping->retired||
       mapping->generation!=s->vm_generation||mapping->first_page!=first||mapping->page_count!=BUFFER_PAGES)
        return 0;
    if(!allocation||s->vm->domain->active||s->vm->domain->changed||s->vm->domain->unsafe||
       s->allocator.record_mapping(s->allocator.cookie,allocation,s->vm,s->vm_ops,mapping)!=DRE_GPU_OK)
        return 0; /* Real staged mapping/handle stay held on catalog refusal. */
    *gpu=s->vm->domain->va_base+(uint64_t)first*4096;return 1;
}
const char *dre_gpu_trusted_prepare(const struct dre_gpu_native_allocator *a,
    struct dre_a619_vm *vm,const struct dre_a619_vm_ops *ops,uint64_t first_gpu,
    struct dre_gpu_trusted_retained_state *journal,
    struct dre_gpu_trusted_queue **out)
{
    if(!out)return "TRUSTED_INVALID_ARGUMENT";*out=0;
    if(!a||!a->allocate||!a->owns_cpu||!a->owns_ram||!a->verify_memory||!vm||!ops||
       !vm->initialized||!vm->generation||vm->unsafe||!vm->domain||vm->domain->active||vm->domain->changed||
       vm->domain->unsafe||vm->domain->page_count<1024||vm->domain->page_count>DRE_A619_SMMU_MAX_PAGES)
        return "TRUSTED_REQUIRES_OWNED_PREPARED_INACTIVE_VM";
    if(a->version!=DRE_GPU_ALLOCATOR_VERSION||a->struct_bytes!=sizeof(*a)||
       !a->classify||!a->record_mapping)
        return "TRUSTED_ALLOCATION_PURPOSE_API_REQUIRED";
    uint64_t base=vm->domain->va_base,span=(uint64_t)vm->domain->page_count*4096;
    if((first_gpu&16383)||first_gpu<base||first_gpu-base>span||
       2u*BUFFER_BYTES>span-(first_gpu-base))return "TRUSTED_BOOTSTRAP_VA_FLOOR_REJECTED";
    if(!journal||!ops->owns_cpu||a->owns_cpu(a->cookie,journal,sizeof(*journal))!=1||
       ops->owns_cpu(ops->mmu.cookie,journal,sizeof(*journal))!=1||journal->allocation||
       journal->memory.cpu||journal->memory.pages||journal->memory.bytes||
       journal->memory.page_count||journal->memory.cache_contract)
        return "TRUSTED_FRESH_WIRED_STATE_JOURNAL_REQUIRED";
    int allocated=a->allocate(a->cookie,BUFFER_BYTES,DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_WRITE,
        &journal->allocation,&journal->memory);
    /* Caller now retains even an incomplete/invalid first allocation. Never
     * dereference it as a queue or lose its actual handle in a local variable. */
    struct dre_gpu_native_allocation *owner=journal->allocation;
    struct dre_gpu_native_memory memory=journal->memory;
    if(allocated!=DRE_GPU_OK||!owner||!memory.cpu||!memory.pages||((uintptr_t)memory.cpu&16383)||
       (uintptr_t)memory.cpu>UINTPTR_MAX-BUFFER_BYTES||memory.bytes!=BUFFER_BYTES||memory.page_count!=BUFFER_PAGES||
       memory.cache_contract!=DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE||a->verify_memory(a->cookie,&memory)!=1)
        return "TRUSTED_STATE_ALLOCATION_UNAVAILABLE";
    for(uint32_t i=0;i<BUFFER_PAGES;i++)
        if(memory.pages[i].permissions!=(DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_WRITE))
            return "TRUSTED_STATE_ALLOCATION_PERMISSIONS_REJECTED_RETAINED";
    /* The journal already owns this exact result. Classification failure
     * cannot construct an opaque queue, lose the journal, retag or free it.
     */
    if(a->classify(a->cookie,owner,&memory,DRE_GPU_ALLOC_TRUSTED_STATE)!=DRE_GPU_OK)
        return "TRUSTED_STATE_ALLOCATION_PURPOSE_REJECTED_RETAINED";
    struct dre_gpu_trusted_queue *s=memory.cpu;
    clear(s,sizeof(*s));s->allocator=*a;s->state_allocation=owner;s->state_memory=memory;
    s->vm=vm;s->vm_ops=ops;s->vm_generation=vm->generation;
    s->first_allowed_page=(uint32_t)((first_gpu-base)/4096);*out=s;
    s->generation=generation();
    if(!s->generation||a->owns_cpu(a->cookie,s,sizeof(*s))!=1||
       !ops->owns_cpu||ops->owns_cpu(ops->mmu.cookie,s,sizeof(*s))!=1)
        return fail(s,"TRUSTED_STATE_OWNER_UNQUALIFIED");
    /* Ordinary immutable command/IB storage is GPU-readonly +XN. UCODE is
     * the distinct executable SMMU role; CP data-command fetch is not UCODE. */
    if(!allocation(s,DRE_GPU_ACCESS_READ,DRE_GPU_ALLOC_IMMUTABLE_IB,&s->ib_allocation,&s->ib_memory)||
       !allocation(s,DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_WRITE,DRE_GPU_ALLOC_PRIVATE_COMPLETION,
            &s->data_allocation,&s->data_memory)||
       !disjoint(&s->state_memory,&s->ib_memory)||!disjoint(&s->state_memory,&s->data_memory)||
       !disjoint(&s->ib_memory,&s->data_memory))return fail(s,"TRUSTED_PRIVATE_BUFFER_OWNER_REJECTED");
    if(!stage(s,s->ib_allocation,&s->ib_memory,&s->ib_mapping,&s->ib_gpu)||
       !stage(s,s->data_allocation,&s->data_memory,&s->data_mapping,&s->data_gpu))
        return fail(s,"TRUSTED_VM_STAGE_UNCERTAIN_RETAINED");
    s->prepared=1;return "TRUSTED_PRIVATE_IB_BUFFERS_STAGED";
}
static uint64_t page_descriptor(struct dre_a619_smmu_page p)
{
    uint64_t d=p.pa|UINT64_C(0xe63);
    if(!(p.permissions&DRE_GPU_ACCESS_WRITE))d|=0x80;
    if(!(p.permissions&DRE_GPU_ACCESS_EXECUTE))d|=UINT64_C(3)<<53;
    return d;
}
static int mapping(struct dre_gpu_trusted_queue *s,const struct dre_gpu_native_memory *m,
    const struct dre_a619_vm_mapping *map,uint64_t gpu)
{
    if(map->generation!=s->vm_generation||!map->lease||map->retired||map->page_count!=4||
       map->first_page>s->vm->domain->page_count-4||
       gpu!=s->vm->domain->va_base+(uint64_t)map->first_page*4096||
       s->allocator.verify_memory(s->allocator.cookie,m)!=1)return 0;
    for(uint32_t i=0;i<4;i++) {
        uint32_t n=map->first_page+i;
        if(s->vm->leases[n]!=map->lease||s->vm->pages[n].pa!=m->pages[i].pa||
           s->vm->pages[n].permissions!=m->pages[i].permissions||
           s->vm->domain->tables[1024+n]!=page_descriptor(m->pages[i]))return 0;
    }
    return 1;
}
static int live(struct dre_gpu_trusted_queue *s)
{
    if(!s||!s->prepared||!s->attached||s->unsafe||!s->startup||!s->ring||!s->startup->plan.cp||
       s->startup->lease!=s->lease||s->startup->plan.vm!=s->vm||s->startup->plan.vm_ops!=s->vm_ops||
       s->startup->plan.cp->queue!=s->ring||s->vm->unsafe||s->vm->generation!=s->vm_generation||
       !s->vm->domain||s->vm->domain!=s->vm->owned_domain||
       s->vm->pages!=s->vm->owned_pages||s->vm->leases!=s->vm->owned_leases||
       s->vm->domain->page_count!=s->vm->owned_pages_count||
       !equal(dre_a619_startup_check_live(s->startup),"NATIVE_START_HELD")||
       s->allocator.verify_memory(s->allocator.cookie,&s->state_memory)!=1||
       !mapping(s,&s->ib_memory,&s->ib_mapping,s->ib_gpu)||
       !mapping(s,&s->data_memory,&s->data_mapping,s->data_gpu))return 0;
    return s->ring->submitted==s->expected_submitted&&s->ring->completed==s->expected_completed;
}
static int read_reg(struct dre_gpu_trusted_queue *s,uint64_t reg,uint32_t *value)
{
    struct dre_a619_ops *io=&s->startup->plan.io;
    return io->guard(io->cookie)&&io->read32(io->cookie,reg,value)&&io->guard(io->cookie);
}
static int snapshot(struct dre_gpu_trusted_queue *s)
{
    struct dre_gpu_trusted_completion *o=&s->observed;
    uint32_t low,high,control,mask,sqe,protection,trust,ahb,halt;
    if(!read_reg(s,DRE_A619_RB_BASE,&low)||!read_reg(s,DRE_A619_RB_BASE_HI,&high)||
       !read_reg(s,DRE_A619_RB_CNTL,&control)||!read_reg(s,DRE_A619_RB_RPTR,&o->rptr)||
       !read_reg(s,DRE_A619_RB_WPTR,&o->wptr)||!read_reg(s,REG(0x210),&o->status)||
       !read_reg(s,REG(0x201),&o->irq)||!read_reg(s,REG(0x821),&o->fault)||
       !read_reg(s,REG(0x38),&mask)||!read_reg(s,REG(0x808),&sqe)||
       !read_reg(s,REG(0x84f),&protection)||!read_reg(s,REG(0xf400),&trust)||
       !read_reg(s,REG(0x98d),&ahb)||!read_reg(s,REG(0x3c45),&halt)||
       !read_reg(s,REG(0x92a),&o->ib1_remaining)||!read_reg(s,REG(0x92d),&o->ib2_remaining)||
       !read_reg(s,REG(0x930),&o->sds_remaining)||!read_reg(s,REG(0x933),&o->mrb_remaining)||
       !read_reg(s,REG(0x949),&o->roq_ib1)||!read_reg(s,REG(0x94a),&o->roq_ib2)||
       !read_reg(s,REG(0x94b),&o->roq_sds)||!read_reg(s,REG(0x94c),&o->roq_mrb))return 0;
    unsigned size=0;while((2u<<size)<s->ring->ring_dwords&&size<14)size++;
    return ((uint64_t)high<<32|low)==s->ring->ring_region.gpu&&
        control==((1u<<27)|(2u<<8)|size)&&!mask&&sqe==1&&protection==11&&
        !trust&&ahb==1&&!halt&&o->rptr<s->ring->ring_dwords&&o->wptr<s->ring->ring_dwords;
}
static int idle(struct dre_gpu_trusted_queue *s)
{
    const struct dre_gpu_trusted_completion *o=&s->observed;
    return !(o->status&0xfffffffeu)&&!o->irq&&!o->fault&&o->rptr==o->wptr&&
        !o->ib1_remaining&&!o->ib2_remaining&&!o->sds_remaining&&!o->mrb_remaining&&
        !(o->roq_ib1>>16)&&!(o->roq_ib2>>16)&&!(o->roq_sds>>16)&&!(o->roq_mrb>>16);
}
const char *dre_gpu_trusted_attach(struct dre_gpu_trusted_queue *s,struct dre_a619_startup *startup)
{
    if(!s||!startup||!s->prepared||s->attached||s->unsafe)return "TRUSTED_NOT_FRESH";
    if(startup->phase!=DRE_A619_START_MARKER_CONFIRMED||!startup->plan.cp||
       !startup->plan.cp->queue||startup->plan.vm!=s->vm||startup->plan.vm_ops!=s->vm_ops||
       startup->plan.cp->stage!=6)return "TRUSTED_STARTUP_MARKER_MISSING";
    struct dre_a619_queue *ring=startup->plan.cp->queue;
    if(ring->submitted!=1||ring->completed!=1||ring->ready_mask!=DRE_A619_READY_MASK||ring->poisoned||
       !ring->ring||ring->ring_dwords<1024||ring->ring_dwords>16384||
       (ring->ring_dwords&(ring->ring_dwords-1)))return "TRUSTED_UNKNOWN_PRIOR_QUEUE_HISTORY";
    struct dre_gpu_trusted_queue *unclaimed=0;
    if(!__atomic_compare_exchange_n(&ring_owner,&unclaimed,s,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return "TRUSTED_NATIVE_RING_ALREADY_CLAIMED";
    s->startup=startup;s->ring=ring;s->lease=startup->lease;
    s->expected_submitted=ring->submitted;s->expected_completed=ring->completed;s->attached=1;
    if(!live(s)||!snapshot(s)||!idle(s)||s->observed.wptr!=ring->write_pointer)
        return fail(s,"TRUSTED_ATTACH_OWNER_OR_IDLE_UNQUALIFIED");
    return "TRUSTED_PRIVATE_QUEUE_ATTACHED_ONLY";
}
static int sealed(struct dre_gpu_trusted_queue *s,int full)
{
    volatile const uint32_t *ib=s->ib_memory.cpu;
    for(uint32_t i=0;i<(full?BUFFER_BYTES/4:s->ib_words);i++)
        if(ib[i]!=(i<s->ib_words?s->sealed_ib[i]:0))return 0;
    return 1;
}
static int data_intact(struct dre_gpu_trusted_queue *s)
{
    volatile const uint32_t *data=s->data_memory.cpu;
    for(uint32_t i=4;i<BUFFER_BYTES/4;i++)if(data[i]!=CANARY(i))return 0;
    return data[SOURCE_INDEX]==s->value;
}
static void build(struct dre_gpu_trusted_queue *s,uint32_t operation,uint32_t value)
{
    volatile uint32_t *ib=s->ib_memory.cpu,*data=s->data_memory.cpu;
    for(uint32_t i=0;i<BUFFER_BYTES/4;i++){ib[i]=0;data[i]=CANARY(i);}
    clear(s->sealed_ib,sizeof(s->sealed_ib));
    uint64_t destination=s->data_gpu+DEST_INDEX*4,source=s->data_gpu+SOURCE_INDEX*4;
    if(operation==DRE_GPU_TRUSTED_WRITE32) {
        s->ib_words=4;s->sealed_ib[0]=dre_a619_type7(0x3d,3);
        s->sealed_ib[1]=(uint32_t)destination;s->sealed_ib[2]=(uint32_t)(destination>>32);
        s->sealed_ib[3]=value;
    } else if(operation==DRE_GPU_TRUSTED_COPY32) {
        s->ib_words=6;s->sealed_ib[0]=dre_a619_type7(0x73,5);s->sealed_ib[1]=0;
        s->sealed_ib[2]=(uint32_t)destination;s->sealed_ib[3]=(uint32_t)(destination>>32);
        s->sealed_ib[4]=(uint32_t)source;s->sealed_ib[5]=(uint32_t)(source>>32);
    } else {
        s->ib_words=2;s->sealed_ib[0]=dre_a619_type7(0x10,1);s->sealed_ib[1]=0;
    }
    for(uint32_t i=0;i<s->ib_words;i++)ib[i]=s->sealed_ib[i];
    data[FENCE_INDEX]=~s->sequence;data[ACK_INDEX]=UINT32_MAX;
    data[SOURCE_INDEX]=value;data[DEST_INDEX]=operation==DRE_GPU_TRUSTED_UCHE_CACHE_ONLY?value:~value;
    s->value=value;s->operation=operation;
}
static unsigned ring_packet(struct dre_gpu_trusted_queue *s,uint32_t *packet)
{
    unsigned n=0;uint64_t fence=s->data_gpu+FENCE_INDEX*4,ack=s->data_gpu+ACK_INDEX*4;
    packet[n++]=dre_a619_type7(0x26,0); /* CP_WAIT_FOR_IDLE */
    packet[n++]=dre_a619_type7(0x46,1);packet[n++]=0x31; /* A6XX UCHE invalidate */
    packet[n++]=dre_a619_type7(0x26,0);
    packet[n++]=dre_a619_type7(0x3f,3); /* no IB_PRIV bit; count <=6 */
    packet[n++]=(uint32_t)s->ib_gpu;packet[n++]=(uint32_t)(s->ib_gpu>>32);packet[n++]=s->ib_words;
    packet[n++]=dre_a619_type7(0x12,0); /* WAIT_MEM_WRITES */
    packet[n++]=dre_a619_type7(0x13,0); /* WAIT_FOR_ME */
    packet[n++]=dre_a619_type7(0x26,0);
    packet[n++]=dre_a619_type7(0x46,4);packet[n++]=0x04; /* CACHE_FLUSH_TS, no IRQ/TS override */
    packet[n++]=(uint32_t)fence;packet[n++]=(uint32_t)(fence>>32);packet[n++]=s->sequence;
    packet[n++]=dre_a619_type7(0x26,0);
    packet[n++]=dre_a619_type7(0x46,1);packet[n++]=0x31;
    packet[n++]=dre_a619_type7(0x26,0);
    packet[n++]=dre_a619_type7(0x62,2); /* actual firmware WHERE_AM_I acknowledgement */
    packet[n++]=(uint32_t)ack;packet[n++]=(uint32_t)(ack>>32);
    return n;
}
const char *dre_gpu_trusted_submit(struct dre_gpu_trusted_queue *s,
    enum dre_gpu_trusted_operation op,uint32_t value,uint64_t *ticket,uint32_t *published)
{
    if(!s||!ticket||!published)return "TRUSTED_INVALID_ARGUMENT";
    *ticket=0;*published=0;
    if(op<DRE_GPU_TRUSTED_WRITE32||op>DRE_GPU_TRUSTED_UCHE_CACHE_ONLY)
        return "TRUSTED_UNSUPPORTED_CACHE_OR_PROGRAM_SCOPE";
    if(s->pending)return "TRUSTED_TICKET_STILL_HELD";
    if(!live(s)||!snapshot(s)||!idle(s)||s->observed.wptr!=s->ring->write_pointer)
        return fail(s,"TRUSTED_SUBMIT_OWNER_OR_IDLE_LOST");
    if(s->next_sequence==UINT32_MAX||s->expected_submitted==UINT64_MAX)
        return "TRUSTED_FENCE_SEQUENCE_EXHAUSTED";
    s->sequence=++s->next_sequence;build(s,op,value);
    if(!sealed(s,1)||!data_intact(s))return fail(s,"TRUSTED_BUILDER_INTEGRITY_FAILED");
    uint32_t packet[RING_WORDS];
    if(ring_packet(s,packet)!=RING_WORDS)return fail(s,"TRUSTED_PACKET_BOUNDS_FAILED");
    uint32_t start=s->ring->write_pointer,n=s->ring->ring_dwords;
    int wrap=start+RING_WORDS>n-2;
    if(wrap&&RING_WORDS>=start)return "TRUSTED_RING_SPACE_UNAVAILABLE";
    uint32_t end=(wrap?0:start)+RING_WORDS;
    struct dre_a619_ops *io=&s->startup->plan.io;
    io->barrier(io->cookie);
    if(!live(s))return fail(s,"TRUSTED_OWNER_LOST_BEFORE_RING_STORE");
    if(wrap) {
        /* Pinned KGSL allocspace: one NOP consumes the entire old tail,
         * payload is inert zeros, new contiguous packet starts at0. */
        s->ring->ring[start]=dre_a619_type7(0x10,n-start-1);
        for(uint32_t i=start+1;i<n;i++)s->ring->ring[i]=0;
        start=0;
    }
    for(uint32_t i=0;i<RING_WORDS;i++)s->ring->ring[start+i]=packet[i];
    io->barrier(io->cookie);
    s->ticket=(uint64_t)s->generation<<32|s->sequence;s->pending=1;s->complete=0;s->expected_end=end;
    clear(&s->observed,sizeof(s->observed));
    s->observed.ticket=s->ticket;s->observed.operation=op;s->observed.hardware_sequence=s->sequence;
    s->observed.state=DRE_GPU_TRUSTED_PENDING;s->observed.expected_value=value;*ticket=s->ticket;
    if(!live(s))return fail(s,"TRUSTED_OWNER_LOST_AFTER_RING_STORE_RETAINED");
    /* A failed callback may still have reached hardware. Journal first and
     * retain every original allocation/mapping on ANY publication uncertainty. */
    *published=1;s->expected_submitted++;s->ring->submitted++;
    s->ring->write_pointer=end;
    if(!io->write32(io->cookie,DRE_A619_RB_WPTR,end))
        return fail(s,"TRUSTED_WPTR_PUBLICATION_UNCERTAIN_RETAINED");
    io->barrier(io->cookie);
    if(!live(s))return fail(s,"TRUSTED_OWNER_LOST_AFTER_PUBLICATION_RETAINED");
    return "TRUSTED_IB_PUBLISHED_NOT_COMPLETE";
}
const char *dre_gpu_trusted_query(struct dre_gpu_trusted_queue *s,uint64_t ticket,
    struct dre_gpu_trusted_completion *out)
{
    if(!s||!out||!s->pending||!ticket||ticket!=s->ticket)return "TRUSTED_STALE_TICKET";
    if(s->unsafe){*out=s->observed;return "TRUSTED_ERROR_RESOURCES_RETAINED";}
    if(!live(s)||!snapshot(s)||!sealed(s,0)) {
        fail(s,"TRUSTED_QUERY_OWNER_MAPPING_OR_COMMAND_CHANGED");*out=s->observed;
        return "TRUSTED_ERROR_RESOURCES_RETAINED";
    }
    if(s->observed.irq||s->observed.fault) {
        fail(s,"TRUSTED_HARDWARE_FAULT_RETAINED");*out=s->observed;return "TRUSTED_ERROR_RESOURCES_RETAINED";
    }
    struct dre_a619_ops *io=&s->startup->plan.io;
    volatile const uint32_t *data=s->data_memory.cpu;io->barrier(io->cookie);
    s->observed.where_am_i=data[ACK_INDEX];s->observed.observed_value=data[DEST_INDEX];
    if(s->observed.rptr==s->expected_end&&s->observed.wptr==s->expected_end&&
       data[FENCE_INDEX]==s->sequence&&data[ACK_INDEX]==s->expected_end&&idle(s)) {
        if(!sealed(s,1)||!data_intact(s)||data[DEST_INDEX]!=s->value||!live(s)) {
            fail(s,"TRUSTED_FENCE_DATA_OR_CANARY_MISMATCH");*out=s->observed;
            return "TRUSTED_ERROR_RESOURCES_RETAINED";
        }
        if(!s->complete) {
            s->expected_completed++;s->ring->completed++;s->complete=1;
        }
        s->observed.state=DRE_GPU_TRUSTED_COMPLETE;
        s->observed.cache_scope=DRE_GPU_TRUSTED_UCHE_FLUSH_INVALIDATE;
        s->observed.engine_references_drained=1;
        *out=s->observed;return "TRUSTED_IB_HARDWARE_AND_UCHE_EVENTS_COMPLETE";
    }
    *out=s->observed;return "TRUSTED_PENDING";
}
const char *dre_gpu_trusted_wait(struct dre_gpu_trusted_queue *s,uint64_t ticket,
    uint64_t budget,struct dre_gpu_trusted_completion *out)
{
    if(!s||!out||!s->attached||!budget||budget>UINT64_C(19200000))return "TRUSTED_INVALID_BUDGET";
    struct dre_a619_ops *io=&s->startup->plan.io;
    uint64_t start=io->now(io->cookie);
    for(unsigned polls=0;polls<1000000;polls++) {
        const char *status=dre_gpu_trusted_query(s,ticket,out);
        if(!equal(status,"TRUSTED_PENDING"))return status;
        if(io->now(io->cookie)-start>=budget) {
            fail(s,"TRUSTED_COMPLETION_TIMEOUT_RETAINED");*out=s->observed;return "TRUSTED_COMPLETION_TIMEOUT_RETAINED";
        }
    }
    fail(s,"TRUSTED_CLOCK_STALLED_RETAINED");*out=s->observed;return "TRUSTED_CLOCK_STALLED_RETAINED";
}
const char *dre_gpu_trusted_close(struct dre_gpu_trusted_queue *s,uint64_t ticket)
{
    struct dre_gpu_trusted_completion completion;
    const char *status=dre_gpu_trusted_query(s,ticket,&completion);
    if(!equal(status,"TRUSTED_IB_HARDWARE_AND_UCHE_EVENTS_COMPLETE"))return status;
    if(!completion.engine_references_drained)return "TRUSTED_RETIREMENT_INCOMPLETE";
    s->pending=s->complete=0;s->ticket=0;
    return "TRUSTED_TICKET_CLOSED_PRIVATE_MAPPINGS_RETAINED";
}
