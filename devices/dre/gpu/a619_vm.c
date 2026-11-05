/* SPDX-License-Identifier: MIT
 * Serialized, explicitly owned A619-Holi BO mappings. No allocator, scheduler,
 * userspace command parser, secure domain, or Metal capability is fabricated. */
#include "a619_vm.h"
#define TOMBSTONE UINT64_MAX
#define MAX_PROBES 128u
static uint64_t generation_seed=1;
static uint64_t fresh_generation(void)
{
    uint64_t old=__atomic_load_n(&generation_seed,__ATOMIC_RELAXED);
    for (unsigned tries=0;tries<1024;tries++) {
        if (old==UINT64_MAX)return 0;
        if (__atomic_compare_exchange_n(&generation_seed,&old,old+1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return old;
    }
    return 0;
}
static int equal(const char *a,const char *b)
{ while (*a && *a==*b) { a++;b++; }return *a==*b; }
static int cpu_range(const void *p,size_t bytes)
{ return p && bytes && (uintptr_t)p<=UINTPTR_MAX-bytes; }
static int overlap(const void *a,size_t n,const void *b,size_t m)
{ return (uintptr_t)a<(uintptr_t)b+m && (uintptr_t)b<(uintptr_t)a+n; }
static int valid_ops(const struct dre_a619_vm_ops *o)
{
    return o && o->owns_cpu && o->table_mapping && o->quiescent && o->mmu.guard &&
        o->mmu.owns_ram && o->mmu.read32 && o->mmu.write32 && o->mmu.read64 &&
        o->mmu.write64 && o->mmu.barrier && o->mmu.now;
}
static const char *unsafe(struct dre_a619_vm *v)
{
    v->unsafe=1;
    if (v->owned_domain && (v->owned_domain->active || v->owned_domain->changed))
        v->owned_domain->unsafe=1;
    return "VM_RESET_REQUIRED";
}
static int same(const struct dre_a619_vm *v)
{
    if (!v->initialized || v->unsafe || v->domain!=v->owned_domain || v->pages!=v->owned_pages ||
        v->leases!=v->owned_leases || v->reverse_pa!=v->owned_reverse)return 0;
    const struct dre_a619_smmu *s=v->domain;
    return !s->unsafe && s->pages==v->pages && s->page_count==v->owned_pages_count &&
        s->page_capacity==v->owned_pages_count && s->table_pa==v->owned_table_pa &&
        s->tables==v->owned_tables && s->va_base==v->owned_va &&
        s->table_capacity_bytes==v->owned_table_bytes && v->reverse_entries==s->page_count*2u &&
        v->generation && v->next_lease && v->mapped_pages<=s->page_count && v->mappings<=v->mapped_pages;
}
static int gpu_idle(const struct dre_a619_vm_ops *o)
{
    void *c=o->mmu.cookie;uint32_t status,irq,fault,rptr,wptr;
    return o->mmu.guard(c) && o->quiescent(c) &&
        o->mmu.read32(c,DRE_A619_GPU_BASE+0x210*4,&status) && !(status&0xfffffffeu) &&
        o->mmu.read32(c,DRE_A619_GPU_BASE+0x201*4,&irq) && !irq &&
        o->mmu.read32(c,DRE_A619_GPU_BASE+0x821*4,&fault) && !fault &&
        o->mmu.read32(c,DRE_A619_RB_RPTR,&rptr) &&
        o->mmu.read32(c,DRE_A619_RB_WPTR,&wptr) && rptr==wptr &&
        o->quiescent(c) && o->mmu.guard(c);
}
static int checked(struct dre_a619_vm *v,const struct dre_a619_vm_ops *o,int hardware)
{
    if (!same(v) || !o->mmu.guard(o->mmu.cookie))return 0;
    if (hardware && !gpu_idle(o))return 0;
    if (v->domain->active)
        return equal(dre_a619_smmu_check_live(v->domain,&o->mmu),"SMMU_LIVE_STATE_VERIFIED");
    return !v->domain->changed;
}
static int protected_cpu(const struct dre_a619_vm *v,const struct dre_a619_vm_ops *o,
    const void *p,size_t bytes)
{
    const struct dre_a619_smmu *s=v->domain;
    return !cpu_range(p,bytes) || !o->owns_cpu(o->mmu.cookie,p,bytes) ||
        overlap(p,bytes,v,sizeof(*v)) || overlap(p,bytes,o,sizeof(*o)) ||
        overlap(p,bytes,s,sizeof(*s)) || overlap(p,bytes,v->pages,(size_t)s->page_count*sizeof(*v->pages)) ||
        overlap(p,bytes,v->leases,(size_t)s->page_count*8) ||
        overlap(p,bytes,v->reverse_pa,(size_t)v->reverse_entries*8) ||
        overlap(p,bytes,s->tables,s->table_capacity_bytes);
}
static uint32_t hash(uint64_t pa,uint32_t mask)
{
    pa>>=12;pa^=pa>>16;pa*=UINT64_C(0x9e3779b185ebca87);pa^=pa>>29;
    return (uint32_t)pa&mask;
}
/* At most 128 probes per page: bounded linear work in the BO page count.
 * Tombstones preserve searches. Exhausted collision budgets fail closed. */
static int insert(struct dre_a619_vm *v,uint64_t pa)
{
    uint32_t mask=v->reverse_entries-1,first=UINT32_MAX,start=hash(pa,mask);
    for (unsigned n=0;n<MAX_PROBES;n++) {
        uint32_t i=(start+n)&mask;uint64_t key=v->reverse_pa[i];
        if (key==pa)return 0;
        if (key==TOMBSTONE && first==UINT32_MAX)first=i;
        if (!key) { v->reverse_pa[first==UINT32_MAX?i:first]=pa;return 1; }
    }
    if (first!=UINT32_MAX) { v->reverse_pa[first]=pa;return 1; }
    return -1;
}
static int remove_pa(struct dre_a619_vm *v,uint64_t pa)
{
    uint32_t mask=v->reverse_entries-1,start=hash(pa,mask);
    for (unsigned n=0;n<MAX_PROBES;n++) {
        uint32_t i=(start+n)&mask;uint64_t key=v->reverse_pa[i];
        if (key==pa) { v->reverse_pa[i]=TOMBSTONE;return 1; }
        if (!key)break;
    }
    return 0;
}
static uint64_t descriptor(struct dre_a619_smmu_page page)
{
    uint64_t d=page.pa|0xe63;
    if (!(page.permissions&DRE_A619_SMMU_WRITE))d|=0x80;
    if (!(page.permissions&DRE_A619_SMMU_EXECUTE))d|=UINT64_C(3)<<53;
    return d;
}
static int topology(const struct dre_a619_vm *v,uint32_t first,uint32_t count)
{
    const struct dre_a619_smmu *s=v->domain;
    uint64_t ns=UINT64_C(0x8000000000000003);
    if (s->tables[(s->va_base>>30)&511]!=((s->table_pa+4096)|ns))return 0;
    for (uint32_t i=first/512;i<=(first+count-1)/512;i++)
        if (s->tables[512+((s->va_base>>21)&511)+i]!=((s->table_pa+8192+(uint64_t)i*4096)|ns))return 0;
    return 1;
}
const char *dre_a619_vm_prepare(struct dre_a619_vm *v,const struct dre_a619_vm_ops *o)
{
    if (!v || !valid_ops(o) || !v->domain)return "INVALID_ARGUMENT";
    struct dre_a619_smmu *s=v->domain;uint32_t n=s->page_count;
    if (v->initialized || v->unsafe || s->active || s->changed || s->unsafe)return "VM_NOT_FRESH";
    size_t bytes=(2u+(n+511u)/512u)*4096u;
    if (n<1024 || n>DRE_A619_SMMU_MAX_PAGES || (n&(n-1)) || s->page_capacity!=n ||
        s->table_capacity_bytes!=bytes || v->reverse_entries!=n*2u || s->pages!=v->pages ||
        (s->va_base&0x1fffff) || s->va_base>=(UINT64_C(1)<<39) ||
        (uint64_t)n*4096>(UINT64_C(1)<<30)-(s->va_base&0x3fffffff) ||
        !s->table_pa || (s->table_pa&4095) || s->table_pa>(UINT64_C(1)<<40)-bytes ||
        ((uintptr_t)s->tables&4095))return "VM_INVALID_GEOMETRY";
    const void *areas[]={v,s,v->pages,v->leases,v->reverse_pa,s->tables};
    size_t lengths[]={sizeof(*v),sizeof(*s),(size_t)n*sizeof(*v->pages),(size_t)n*8,(size_t)n*16,bytes};
    for (unsigned i=0;i<6;i++) {
        if (!cpu_range(areas[i],lengths[i]) || ((uintptr_t)areas[i]&7) ||
            !o->owns_cpu(o->mmu.cookie,areas[i],lengths[i]) || overlap(areas[i],lengths[i],o,sizeof(*o)))
            return "VM_UNOWNED_CPU_MEMORY";
        for (unsigned j=0;j<i;j++)if (overlap(areas[i],lengths[i],areas[j],lengths[j]))return "VM_CPU_ALIAS";
    }
    if (!o->mmu.guard(o->mmu.cookie) || !o->mmu.owns_ram(o->mmu.cookie,s->table_pa,bytes) ||
        !o->table_mapping(o->mmu.cookie,s->table_pa,s->tables,bytes))return "VM_UNOWNED_TABLES";
    uint64_t generation=fresh_generation();if (!generation)return "VM_GENERATION_UNAVAILABLE";
    v->owned_domain=s;v->owned_pages=v->pages;v->owned_leases=v->leases;v->owned_reverse=v->reverse_pa;
    v->owned_tables=s->tables;v->owned_table_pa=s->table_pa;v->owned_va=s->va_base;
    v->owned_pages_count=n;v->owned_table_bytes=bytes;
    for (uint32_t i=0;i<n;i++) { v->pages[i]=(struct dre_a619_smmu_page){0};v->leases[i]=0; }
    for (uint32_t i=0;i<v->reverse_entries;i++)v->reverse_pa[i]=0;
    for (size_t i=0;i<bytes/8;i++)s->tables[i]=0;
    v->generation=generation;v->next_lease=1;v->mapped_pages=v->mappings=0;v->initialized=1;
    return "VM_PREPARED";
}
const char *dre_a619_vm_map(struct dre_a619_vm *v,const struct dre_a619_vm_ops *o,
    uint32_t first,const struct dre_a619_smmu_page *pages,uint32_t count,
    struct dre_a619_vm_mapping *m)
{
    if (!v || !valid_ops(o) || !m)return "INVALID_ARGUMENT";
    if (!same(v))return v->initialized?unsafe(v):"VM_NOT_PREPARED";
    struct dre_a619_smmu *s=v->domain;
    if (!count || first>=s->page_count || count>s->page_count-first ||
        protected_cpu(v,o,pages,(size_t)count*sizeof(*pages)) || protected_cpu(v,o,m,sizeof(*m)) ||
        overlap(pages,(size_t)count*sizeof(*pages),m,sizeof(*m)))return "VM_INVALID_RANGE";
    if (m->lease || m->generation)return "VM_HANDLE_NOT_FRESH";
    if (v->next_lease==UINT64_MAX)return "VM_LEASE_EXHAUSTED";
    if (!checked(v,o,s->active))return unsafe(v);
    if (s->active && !topology(v,first,count))return unsafe(v);
    for (uint32_t i=0;i<count;i++)
        if (v->leases[first+i] || v->pages[first+i].pa || v->pages[first+i].permissions ||
            (s->active && s->tables[1024+first+i]))return "VM_RANGE_OCCUPIED";
    uint32_t inserted=0;const char *error=0;
    for (;inserted<count;inserted++) {
        struct dre_a619_smmu_page p=pages[inserted];
        if (!p.pa || (p.pa&4095) || p.pa>=(UINT64_C(1)<<40) ||
            !(p.permissions&1) || (p.permissions&~7u) || (p.permissions&6u)==6u ||
            (p.pa<s->table_pa+s->table_capacity_bytes && s->table_pa<p.pa+4096) ||
            !o->mmu.owns_ram(o->mmu.cookie,p.pa,4096)) { error="VM_INVALID_PHYSICAL_PAGE";break; }
        int ok=insert(v,p.pa);
        if (ok!=1) { error=ok?"VM_REVERSE_PROBE_LIMIT":"VM_PHYSICAL_ALIAS";break; }
        v->pages[first+inserted]=p; /* retain the checked snapshot, not a reread */
    }
    if (error) {
        for (uint32_t i=0;i<inserted;i++) {
            if (!remove_pa(v,v->pages[first+i].pa))return unsafe(v);
            v->pages[first+i]=(struct dre_a619_smmu_page){0};
        }
        return error;
    }
    uint64_t lease=v->next_lease++;
    for (uint32_t i=0;i<count;i++)v->leases[first+i]=lease;
    *m=(struct dre_a619_vm_mapping){.generation=v->generation,.lease=lease,
        .first_page=first,.page_count=count,.retired=0};v->mapped_pages+=count;v->mappings++;
    if (!checked(v,o,s->active))return unsafe(v);
    if (!s->active)return "VM_MAPPING_STAGED";
    volatile uint64_t *leaf=s->tables+1024+first;
    for (uint32_t i=0;i<count;i++)leaf[i]=descriptor(v->pages[first+i]);
    o->mmu.barrier(o->mmu.cookie);
    if (!equal(dre_a619_smmu_sync_live(s,&o->mmu),"SMMU_LIVE_STATE_VERIFIED") || !checked(v,o,1))return unsafe(v);
    for (uint32_t i=0;i<count;i++)if (leaf[i]!=descriptor(v->pages[first+i]))return unsafe(v);
    return "VM_MAPPING_ACTIVE";
}
const char *dre_a619_vm_activate(struct dre_a619_vm *v,const struct dre_a619_vm_ops *o)
{
    if (!v || !valid_ops(o))return "INVALID_ARGUMENT";
    if (!same(v))return v->initialized?unsafe(v):"VM_NOT_PREPARED";
    if (v->domain->active)return "VM_ALREADY_ACTIVE";
    if (!v->mapped_pages)return "VM_EMPTY";
    if (!checked(v,o,1))return unsafe(v);
    const char *status=dre_a619_smmu_activate(v->domain,&o->mmu);
    if (v->domain->unsafe)return unsafe(v);
    return equal(status,"SMMU_SID0_ACTIVE")?"VM_ACTIVE":status;
}
const char *dre_a619_vm_unmap(struct dre_a619_vm *v,const struct dre_a619_vm_ops *o,
    struct dre_a619_vm_mapping *m)
{
    if (!v || !valid_ops(o) || !m)return "INVALID_ARGUMENT";
    if (!same(v))return v->initialized?unsafe(v):"VM_NOT_PREPARED";
    struct dre_a619_smmu *s=v->domain;
    if (protected_cpu(v,o,m,sizeof(*m)))return "VM_INVALID_HANDLE";
    uint32_t first=m->first_page,count=m->page_count;
    if (!m->lease || m->generation!=v->generation || m->retired || !count || first>=s->page_count || count>s->page_count-first)
        return "VM_INVALID_HANDLE";
    if ((first && v->leases[first-1]==m->lease) ||
        (first+count<s->page_count && v->leases[first+count]==m->lease))return "VM_PARTIAL_HANDLE";
    if (!v->mappings || count>v->mapped_pages)return unsafe(v);
    for (uint32_t i=0;i<count;i++)if (v->leases[first+i]!=m->lease || !v->pages[first+i].pa)return "VM_STALE_HANDLE";
    if (!checked(v,o,s->active))return unsafe(v);
    if (s->active) {
        if (!topology(v,first,count))return unsafe(v);
        volatile uint64_t *leaf=s->tables+1024+first;
        for (uint32_t i=0;i<count;i++)if (leaf[i]!=descriptor(v->pages[first+i]))return unsafe(v);
        for (uint32_t i=0;i<count;i++)leaf[i]=0;
        o->mmu.barrier(o->mmu.cookie);
        if (!equal(dre_a619_smmu_sync_live(s,&o->mmu),"SMMU_LIVE_STATE_VERIFIED") || !checked(v,o,1))return unsafe(v);
        for (uint32_t i=0;i<count;i++)if (leaf[i])return unsafe(v);
    }
    for (uint32_t i=0;i<count;i++) {
        if (!remove_pa(v,v->pages[first+i].pa))return unsafe(v);
        v->pages[first+i]=(struct dre_a619_smmu_page){0};v->leases[first+i]=0;
    }
    v->mapped_pages-=count;v->mappings--;m->retired=1;
    return "VM_UNMAPPED_RETIRED";
}
const char *dre_a619_vm_release(struct dre_a619_vm *v,const struct dre_a619_vm_ops *o)
{
    if (!v || !valid_ops(o))return "INVALID_ARGUMENT";
    if (!same(v))return v->initialized?unsafe(v):"VM_NOT_PREPARED";
    if (v->mapped_pages || v->mappings)return "VM_MAPPINGS_LIVE";
    if (!checked(v,o,v->domain->active))return unsafe(v);
    if (v->domain->active) {
        if (!equal(dre_a619_smmu_release(v->domain,&o->mmu),"SMMU_SID0_RESTORED"))return unsafe(v);
    }
    v->initialized=0;
    return "VM_RELEASED";
}
