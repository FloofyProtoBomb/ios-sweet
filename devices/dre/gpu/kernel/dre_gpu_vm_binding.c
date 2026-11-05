/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight draft. No tests/compiles/hardware execution.
 * Calls the actual owned VM mapper/TLB synchronizer; provides no cache flush,
 * packet parser, scheduler, protected task VM or persistent CPU map support. */
#include "dre_gpu_vm_binding.h"
static int equal(const char *a,const char *b)
{ if(!a||!b)return 0;while(*a&&*a==*b){a++;b++;}return *a==*b; }
static void clear(void *v,size_t n)
{ unsigned char *p=v;while(n--)*p++=0; }
static int fail(struct dre_gpu_vm_binding *b)
{
    b->unsafe=1;
    if(b->startup)dre_a619_startup_invalidate(b->startup,"EXCLUSIVE_VM_BINDING_QUARANTINED");
    return DRE_GPU_FAULT;
}
static uint64_t pte(struct dre_a619_smmu_page p)
{
    uint64_t d=p.pa|UINT64_C(0xe63);
    if(!(p.permissions&DRE_A619_SMMU_WRITE))d|=0x80;
    if(!(p.permissions&DRE_A619_SMMU_EXECUTE))d|=UINT64_C(3)<<53;
    return d;
}
static int protected_page(const struct dre_gpu_vm_binding *b,uint32_t page)
{
    uint32_t block=page/512;
    return !!(b->protected_blocks[block/64]&(UINT64_C(1)<<(block%64)));
}
static void protect(struct dre_gpu_vm_binding *b,uint32_t page)
{
    uint32_t block=page/512;
    b->protected_blocks[block/64]|=UINT64_C(1)<<(block%64);
}
int dre_gpu_vm_binding_live(struct dre_gpu_vm_binding *b)
{
    if(!b||!b->initialized||b->unsafe||!b->startup||!b->vm||!b->ops)
        return DRE_GPU_NOT_READY;
    struct dre_a619_vm *v=b->vm;
    struct dre_a619_smmu *s=v->domain;
    if(b->startup->plan.vm!=v||b->startup->plan.vm_ops!=b->ops||
       !v->initialized||v->unsafe||!s||!s->active||s->unsafe||
       v->domain!=v->owned_domain||v->pages!=v->owned_pages||v->leases!=v->owned_leases||
       v->reverse_pa!=v->owned_reverse||v->generation!=b->generation||
       s->va_base!=b->base||s->table_pa!=b->table_pa||s->page_count!=b->page_count||
       b->page_count!=v->owned_pages_count||b->base!=v->owned_va||
       b->table_pa!=v->owned_table_pa||s->page_capacity!=b->page_count||
       s->table_capacity_bytes!=v->owned_table_bytes||
       s->pages!=v->pages||s->tables!=v->owned_tables||
       !b->ops->owns_cpu||!b->ops->owns_cpu(b->ops->mmu.cookie,b,sizeof(*b))||
       !equal(dre_a619_startup_check_live(b->startup),"NATIVE_START_HELD"))
        return fail(b);
    uint64_t pages=b->bootstrap_pages;
    uint32_t entries=0;
    for(uint32_t i=0;i<DRE_GPU_MAX_BOS;i++)if(b->entries[i].held) {
        if(b->entries[i].uncertain)return fail(b);
        entries++;
        pages+=b->entries[i].mapping.page_count;
    }
    if(entries!=b->entries_live||v->mappings!=b->bootstrap_mappings+entries||
       pages!=v->mapped_pages)return fail(b);
    return DRE_GPU_OK;
}
int dre_gpu_vm_binding_init(struct dre_gpu_vm_binding *b,struct dre_a619_startup *startup,
    const struct dre_gpu_vm_reserved *reserved,uint32_t count)
{
    if(!b||!startup||!startup->plan.vm||!startup->plan.vm_ops||count>16||
       (count&&!reserved))return DRE_GPU_INVALID;
    const struct dre_a619_vm_ops *o=startup->plan.vm_ops;
    if(!o->owns_cpu||!o->owns_cpu(o->mmu.cookie,b,sizeof(*b)))return DRE_GPU_NOT_READY;
    /* A live object cannot be erased/reused merely because a task closed. */
    if(b->initialized||b->unsafe||b->task_identity||b->entries_live)return DRE_GPU_BUSY;
    struct dre_a619_vm *v=startup->plan.vm;
    struct dre_a619_smmu *s=v->domain;
    if(!v->initialized||v->unsafe||!s||!s->active||s->unsafe||
       v->domain!=v->owned_domain||v->pages!=v->owned_pages||v->leases!=v->owned_leases||
       s->pages!=v->pages||s->tables!=v->owned_tables||s->page_count!=v->owned_pages_count||
       s->page_capacity!=s->page_count||s->table_capacity_bytes!=v->owned_table_bytes||
       s->va_base!=v->owned_va||s->table_pa!=v->owned_table_pa||
       s->page_count<1024||s->page_count>DRE_A619_SMMU_MAX_PAGES||
       !equal(dre_a619_startup_check_live(startup),"NATIVE_START_HELD"))return DRE_GPU_NOT_READY;
    struct dre_gpu_vm_reserved private_reserved[16];
    if(count&&!o->owns_cpu(o->mmu.cookie,reserved,count*sizeof(*reserved)))return DRE_GPU_NOT_READY;
    for(uint32_t i=0;i<count;i++)private_reserved[i]=reserved[i];
    clear(b,sizeof(*b));b->startup=startup;b->vm=v;b->ops=o;
    b->generation=v->generation;b->base=s->va_base;b->table_pa=s->table_pa;
    b->page_count=s->page_count;b->bootstrap_pages=v->mapped_pages;
    b->bootstrap_mappings=v->mappings;
    /* Snapshot only after real startup. Every existing populated leaf is
     * checked against its owned PA/lease/permission metadata, then its entire
     * L3 block is reserved. Empty CPU-page padding and guard holes inside that
     * block remain reserved even if a bootstrap mapping is later removed. */
    for(uint32_t i=0;i<s->page_count;i++) {
        struct dre_a619_smmu_page page=v->pages[i];
        uint64_t leaf=s->tables[1024+i],lease=v->leases[i];
        if(!page.pa&&!page.permissions&&!leaf&&!lease)continue;
        if(!page.pa||!lease||!(page.permissions&1)||(page.permissions&~7u)||
           (page.permissions&6u)==6u||leaf!=pte(page))return fail(b);
        protect(b,i);
    }
    for(uint32_t i=0;i<count;i++) {
        uint64_t address=private_reserved[i].gpu_va,bytes=private_reserved[i].bytes;
        if(!bytes||(address&4095)||(bytes&4095)||address<b->base||
           address-b->base>(uint64_t)b->page_count*4096||
           bytes>(uint64_t)b->page_count*4096-(address-b->base))return fail(b);
        uint32_t first=(uint32_t)((address-b->base)/4096),n=(uint32_t)(bytes/4096);
        for(uint32_t p=0;p<n;p++)protect(b,first+p);
    }
    b->initialized=1;
    return dre_gpu_vm_binding_live(b);
}
int dre_gpu_vm_binding_open(struct dre_gpu_vm_binding *b,uint64_t task,uint64_t generation)
{
    if(!task||!generation)return DRE_GPU_INVALID;
    int status=dre_gpu_vm_binding_live(b);if(status!=DRE_GPU_OK)return status;
    if(b->task_identity||b->entries_live)return DRE_GPU_BUSY;
    b->task_identity=task;b->task_generation=generation;return DRE_GPU_OK;
}
static int session(struct dre_gpu_vm_binding *b,uint64_t task,uint64_t generation)
{
    int status=dre_gpu_vm_binding_live(b);if(status!=DRE_GPU_OK)return status;
    return task&&generation&&b->task_identity==task&&b->task_generation==generation?
        DRE_GPU_OK:DRE_GPU_STALE;
}
int dre_gpu_vm_binding_map(struct dre_gpu_vm_binding *b,uint64_t task,uint64_t generation,
    const struct dre_gpu_native_memory *m,uint32_t access,uint64_t *gpu_va,uint64_t *lease)
{
    if(!m||!gpu_va||!lease)return DRE_GPU_INVALID;
    *gpu_va=*lease=0;
    int status=session(b,task,generation);if(status!=DRE_GPU_OK)return status;
    if(!m->cpu||!m->pages||!m->bytes||m->bytes>DRE_GPU_BO_MAX_BYTES||
       m->bytes%DRE_GPU_CPU_PAGE_BYTES||m->page_count!=m->bytes/4096||
       m->cache_contract!=DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE||!(access&1)||
       (access&~7u)||(access&6u)==6u)return DRE_GPU_INVALID;
    const struct dre_a619_vm_ops *o=b->ops;
    if(!o->owns_cpu(o->mmu.cookie,m->cpu,m->bytes)||
       !o->owns_cpu(o->mmu.cookie,m->pages,m->page_count*sizeof(*m->pages)))
        return DRE_GPU_NOT_READY;
    for(uint32_t i=0;i<m->page_count;i++)if(m->pages[i].permissions!=access)
        return DRE_GPU_INVALID;
    uint32_t slot=0;while(slot<DRE_GPU_MAX_BOS&&b->entries[slot].held)slot++;
    if(slot==DRE_GPU_MAX_BOS||m->page_count>b->page_count)return DRE_GPU_LIMIT;
    uint32_t found=UINT32_MAX,run=0;
    struct dre_a619_vm *v=b->vm;
    /* One linear pass, independent of BO page count. Respect whole 16KiB
     * CPU granules and reserve all initial bootstrap 2MiB blocks. */
    for(uint32_t i=0;i<b->page_count;i++) {
        if(protected_page(b,i)||v->leases[i]||v->pages[i].pa||v->pages[i].permissions||
           v->domain->tables[1024+i]) { run=0;continue; }
        if(!run&&(i&3))continue;
        if(++run==m->page_count){found=i+1-run;break;}
    }
    if(found==UINT32_MAX)return DRE_GPU_LIMIT;
    struct dre_gpu_vm_entry *e=&b->entries[slot];clear(e,sizeof(*e));
    e->held=1;e->memory=*m;e->access=access;e->gpu_va=b->base+(uint64_t)found*4096;
    const char *result=dre_a619_vm_map(v,o,found,m->pages,m->page_count,&e->mapping);
    *gpu_va=e->gpu_va;*lease=e->mapping.lease;
    if(!equal(result,"VM_MAPPING_ACTIVE")||!e->mapping.lease||
       e->mapping.generation!=b->generation||e->mapping.first_page!=found||
       e->mapping.page_count!=m->page_count||e->mapping.retired) {
        /* The caller must retain this source allocation even for a partial
         * mapping failure. No table or PA guessing can authorize its free. */
        e->uncertain=1;return fail(b);
    }
    b->entries_live++;
    return dre_gpu_vm_binding_live(b);
}
int dre_gpu_vm_binding_retire(struct dre_gpu_vm_binding *b,uint64_t task,uint64_t generation,
    const struct dre_gpu_native_bo *bo)
{
    if(!bo)return DRE_GPU_INVALID;
    int status=session(b,task,generation);if(status!=DRE_GPU_OK)return status;
    struct dre_gpu_vm_entry *e=0;
    for(uint32_t i=0;i<DRE_GPU_MAX_BOS;i++)if(b->entries[i].held&&
       b->entries[i].mapping.lease==bo->mapping_lease&&b->entries[i].gpu_va==bo->gpu_va)
        {e=&b->entries[i];break;}
    if(!e||!bo->mapping_lease||e->memory.cpu!=bo->memory.cpu||
       e->memory.pages!=bo->memory.pages||e->memory.bytes!=bo->memory.bytes||
       e->memory.page_count!=bo->memory.page_count||e->access!=bo->access)
        return DRE_GPU_STALE;
    if(e->uncertain)return fail(b);
    /* vm_unmap requires actual cache/firmware/queue quiescence via ops and
     * completes real leaf clear + ASID/context TLB sync before retired=true. */
    const char *result=dre_a619_vm_unmap(b->vm,b->ops,&e->mapping);
    if(!equal(result,"VM_UNMAPPED_RETIRED")||!e->mapping.retired) {
        e->uncertain=1;return fail(b);
    }
    if(!b->entries_live)return fail(b);
    b->entries_live--;clear(e,sizeof(*e));
    return dre_gpu_vm_binding_live(b);
}
int dre_gpu_vm_binding_close(struct dre_gpu_vm_binding *b,uint64_t task,uint64_t generation)
{
    int status=session(b,task,generation);if(status!=DRE_GPU_OK)return status;
    if(b->entries_live)return DRE_GPU_BUSY;
    b->task_identity=b->task_generation=0;return DRE_GPU_OK;
}
