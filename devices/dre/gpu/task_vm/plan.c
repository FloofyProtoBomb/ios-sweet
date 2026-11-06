/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only proposal builder. */
#include "plan.h"
#include "../../native_board.h"
#include <libfdt.h>
#include <string.h>
static int region(const void *p,size_t n)
{ return p&&n&&(uintptr_t)p<=UINTPTR_MAX-n; }
static int separate(const void *a,size_t na,const void *b,size_t nb)
{
    return region(a,na)&&region(b,nb)&&
        ((uintptr_t)a+na<=(uintptr_t)b||(uintptr_t)b+nb<=(uintptr_t)a);
}
static int workspace_separate(const struct dre_taskvm_workspace *w,const void *p,size_t bytes)
{
    return separate(w->rows,(size_t)w->row_capacity*sizeof(*w->rows),p,bytes)&&
        separate(w->sorted_pa,(size_t)w->pa_capacity*sizeof(*w->sorted_pa),p,bytes);
}
static int actual_holi(void)
{
    const void *f=0;size_t bytes=0;
    if(dre_native_board_snapshot(&f,&bytes)||!f||bytes<40||bytes>(2u<<20)||
       fdt_check_full(f,bytes)||fdt_totalsize(f)!=bytes||
       fdt_node_check_compatible(f,0,"qcom,holi"))return 0;
    int n=fdt_node_offset_by_compatible(f,-1,"qcom,adreno-gpu-a619-holi"),size;
    if(n<0||fdt_node_offset_by_compatible(f,n,"qcom,adreno-gpu-a619-holi")!=-FDT_ERR_NOTFOUND)return 0;
    const fdt32_t *chip=fdt_getprop(f,n,"qcom,chipid",&size);
    return chip&&size==4&&fdt32_to_cpu(*chip)==UINT32_C(0x06010900);
}
static int snapshot(const struct dre_taskvm_proof_ops *o,const struct dre_taskvm_catalog *c)
{ return o->owner_held(o->cookie)==1&&o->catalog_current(o->cookie,c)==1; }
static void sift(uint64_t *a,uint32_t root,uint32_t n)
{
    while(root<n/2) {
        uint32_t child=root*2+1;
        if(child+1<n&&a[child]<a[child+1])child++;
        if(a[root]>=a[child])break;
        uint64_t value=a[root];a[root]=a[child];a[child]=value;root=child;
    }
}
static void sort(uint64_t *a,uint32_t n)
{
    for(uint32_t i=n/2;i;i--)sift(a,i-1,n);
    for(uint32_t end=n;end>1;end--) {
        uint64_t value=a[0];a[0]=a[end-1];a[end-1]=value;sift(a,0,end-1);
    }
}
/* A role is accepted only AFTER allocation_current resolved its actual
 * immutable private purpose. This table alone supplies no ownership proof. */
static int role(const struct dre_taskvm_catalog *c,const struct dre_taskvm_allocation *a,
                enum dre_taskvm_privilege *p,int *write,int *fetch,int *excluded)
{
    *p=DRE_TASKVM_USER_ACCESSIBLE;*write=0;*fetch=0;*excluded=0;
    if(a->gpu_access!=1&&a->gpu_access!=3&&a->gpu_access!=5)return 0;
    if(a->role==DRE_TASKVM_TASK_DATA||a->role==DRE_TASKVM_TASK_COMMAND_SNAPSHOT) {
        if(a->owner_task!=c->task_identity)return 0;
    } else if(a->owner_task)return 0;
    switch(a->role) {
    case DRE_TASKVM_TASK_DATA:
        if(a->gpu_access&4)return 0;
        *write=!!(a->gpu_access&2);break;
    case DRE_TASKVM_TASK_COMMAND_SNAPSHOT:
    case DRE_TASKVM_BOOT_RING:
        if(a->gpu_access!=1)return 0;
        break; /* KGSL ordinary IB and non-APRIV ring are RO +NOEXEC. */
    case DRE_TASKVM_ANCHORED_SQE:
        if(a->gpu_access!=5)return 0;
        *fetch=1;break; /* Only actual UCODE removes NOEXEC in pinned KGSL. */
    case DRE_TASKVM_BOOT_SCRATCH:
    case DRE_TASKVM_POWERUP_REGISTERS:
        if(a->gpu_access!=3)return 0;
        *p=DRE_TASKVM_PRIVILEGED_ONLY;*write=1;break;
    case DRE_TASKVM_PRIVATE_COMPLETION:
    case DRE_TASKVM_CPU_BOOKKEEPING:
    case DRE_TASKVM_TRANSLATION_TABLE:
    case DRE_TASKVM_OTHER_PRIVATE:
        /* No user mapping: neither current RW result pages nor a VA label
         * establish a safe privileged completion/reference retirement path. */
        *excluded=1;break;
    default:return 0;
    }
    return 1;
}
const char *dre_taskvm_build_proposal(const struct dre_taskvm_proof_ops *o,
    const struct dre_taskvm_catalog *c,struct dre_taskvm_workspace *w,
    struct dre_taskvm_proposal *out)
{
    if(!o||!c||!w||!out||((uintptr_t)o&7)||((uintptr_t)c&7)||
       ((uintptr_t)w&7)||((uintptr_t)out&7)||!o->owner_held||!o->catalog_current||!o->allocation_current||
       !o->owns_cpu)return "TASKVM_ACTUAL_REGISTRY_PROVIDERS_REQUIRED";
    if(!c->common_lease||!c->reset_epoch||!c->task_identity||!c->catalog_generation||
       !c->allocation_count||c->allocation_count>DRE_TASKVM_MAX_ALLOCATIONS||
       c->window_pages<1024||c->window_pages>65536||(c->window_pages&(c->window_pages-1))||
       (c->va_base&0x1fffff)||c->va_base>=(UINT64_C(1)<<39)||
       (uint64_t)c->window_pages*4096>(UINT64_C(1)<<30)-(c->va_base&0x3fffffff))
        return "TASKVM_INVALID_PRIVATE_CATALOG";
    size_t allocationBytes=(size_t)c->allocation_count*sizeof(*c->allocations);
    if(!region(c->allocations,allocationBytes)||((uintptr_t)c->allocations&7)||
       ((uintptr_t)w->rows&7)||((uintptr_t)w->sorted_pa&7)||!w->pa_capacity||!w->row_capacity||
       w->pa_capacity>DRE_TASKVM_MAX_CATALOG_PAGES||w->row_capacity>DRE_TASKVM_MAX_CATALOG_PAGES||
       !separate(w->rows,(size_t)w->row_capacity*sizeof(*w->rows),
                 w->sorted_pa,(size_t)w->pa_capacity*sizeof(*w->sorted_pa))||
       !separate(w->rows,(size_t)w->row_capacity*sizeof(*w->rows),c->allocations,allocationBytes)||
       !separate(w->sorted_pa,(size_t)w->pa_capacity*sizeof(*w->sorted_pa),c->allocations,allocationBytes)||
       !workspace_separate(w,o,sizeof(*o))||!workspace_separate(w,c,sizeof(*c))||
       !workspace_separate(w,w,sizeof(*w))||!workspace_separate(w,out,sizeof(*out))||
       !separate(out,sizeof(*out),o,sizeof(*o))||!separate(out,sizeof(*out),c,sizeof(*c))||
       !separate(out,sizeof(*out),w,sizeof(*w))||!separate(out,sizeof(*out),c->allocations,allocationBytes)||
       o->owns_cpu(o->cookie,o,sizeof(*o))!=1||
       o->owns_cpu(o->cookie,c,sizeof(*c))!=1||o->owns_cpu(o->cookie,c->allocations,allocationBytes)!=1||
       o->owns_cpu(o->cookie,w,sizeof(*w))!=1||o->owns_cpu(o->cookie,out,sizeof(*out))!=1||
       o->owns_cpu(o->cookie,w->rows,(size_t)w->row_capacity*sizeof(*w->rows))!=1||
       o->owns_cpu(o->cookie,w->sorted_pa,(size_t)w->pa_capacity*sizeof(*w->sorted_pa))!=1)
        return "TASKVM_UNOWNED_OR_OVERLAPPING_WORKSPACE";
    if(!snapshot(o,c)||!actual_holi())return "TASKVM_ACTUAL_HOLI_AND_CATALOG_OWNER_REQUIRED";
    /* Qualify ALL immutable page-list ranges before the first workspace
     * store. Discovering a later input/output overlap after writing earlier
     * rows would already have corrupted the authoritative catalog. */
    uint32_t preflightPages=0,roleSeen=0,taskAllocations=0;
    for(uint32_t i=0;i<c->allocation_count;i++) {
        const struct dre_taskvm_allocation *a=&c->allocations[i];
        if(!a->allocation||!a->allocation_generation||!a->pin_lease||
           a->role<DRE_TASKVM_TASK_DATA||a->role>DRE_TASKVM_OTHER_PRIVATE||
           !a->page_count||(a->page_count&3)||a->mapped_prefix_pages>a->page_count||
           a->page_count>DRE_TASKVM_MAX_CATALOG_PAGES-preflightPages||
           ((uintptr_t)a->pages&7)||!region(a->pages,(size_t)a->page_count*sizeof(*a->pages))||
           !workspace_separate(w,a->pages,(size_t)a->page_count*sizeof(*a->pages))||
           !separate(out,sizeof(*out),a->pages,(size_t)a->page_count*sizeof(*a->pages))||
           o->owns_cpu(o->cookie,a->pages,(size_t)a->page_count*sizeof(*a->pages))!=1||
           o->allocation_current(o->cookie,c,a)!=1)
            return "TASKVM_ACTUAL_COMPLETE_PAGE_LIST_PREFLIGHT_REQUIRED";
        preflightPages+=a->page_count;
        if(a->role<DRE_TASKVM_TASK_DATA||a->role>DRE_TASKVM_OTHER_PRIVATE)
            return "TASKVM_UNKNOWN_ALLOCATION_PURPOSE";
        uint32_t bit=UINT32_C(1)<<(unsigned)a->role;
        if(a->role>=DRE_TASKVM_BOOT_RING&&a->role<=DRE_TASKVM_POWERUP_REGISTERS&&
           (roleSeen&bit))return "TASKVM_SINGLE_BOOTSTRAP_CONTEXT_REQUIRED";
        roleSeen|=bit;
        if(a->role==DRE_TASKVM_TASK_DATA||a->role==DRE_TASKVM_TASK_COMMAND_SNAPSHOT)taskAllocations++;
    }
    const uint32_t required=(1u<<DRE_TASKVM_BOOT_RING)|(1u<<DRE_TASKVM_ANCHORED_SQE)|
        (1u<<DRE_TASKVM_BOOT_SCRATCH)|(1u<<DRE_TASKVM_POWERUP_REGISTERS)|
        (1u<<DRE_TASKVM_PRIVATE_COMPLETION)|(1u<<DRE_TASKVM_CPU_BOOKKEEPING)|
        (1u<<DRE_TASKVM_TRANSLATION_TABLE);
    if((roleSeen&required)!=required||!taskAllocations)
        return "TASKVM_COMPLETE_REAL_BOOTSTRAP_AND_TASK_INVENTORY_REQUIRED";
    if(preflightPages>w->pa_capacity||!snapshot(o,c))return "TASKVM_CATALOG_OR_WORKSPACE_BOUND_REFUSED";
    uint32_t total=0,rows=0,excludedPages=0;
    for(uint32_t index=0;index<c->allocation_count;index++) {
        const struct dre_taskvm_allocation *a=&c->allocations[index];
        if(!a->allocation||!a->allocation_generation||!a->pin_lease||!a->page_count||
           (a->page_count&3)||a->page_count>DRE_TASKVM_MAX_CATALOG_PAGES-total||
           a->mapped_prefix_pages>a->page_count||
           !region(a->pages,(size_t)a->page_count*sizeof(*a->pages))||((uintptr_t)a->pages&7)||
           !workspace_separate(w,a->pages,(size_t)a->page_count*sizeof(*a->pages))||
           !separate(out,sizeof(*out),a->pages,(size_t)a->page_count*sizeof(*a->pages))||
           o->owns_cpu(o->cookie,a->pages,(size_t)a->page_count*sizeof(*a->pages))!=1||
           o->allocation_current(o->cookie,c,a)!=1)
            return "TASKVM_ACTUAL_IMMUTABLE_ALLOCATION_PURPOSE_REQUIRED";
        enum dre_taskvm_privilege privilege;int write,fetch,excluded;
        if(!role(c,a,&privilege,&write,&fetch,&excluded))return "TASKVM_UNSUPPORTED_ROLE_OR_GRANTED_ACCESS";
        uint32_t mapped=excluded?0:a->mapped_prefix_pages;
        if(!excluded&&!mapped)return "TASKVM_MAPPED_PREFIX_REQUIRED";
        if(mapped&&((a->gpu_va&4095)||a->gpu_va<c->va_base||
           (uint64_t)mapped*4096>(uint64_t)c->window_pages*4096||
           a->gpu_va-c->va_base>(uint64_t)(c->window_pages-mapped)*4096))
            return "TASKVM_OUTSIDE_ACTUAL_TASK_VA_RESERVATION";
        if(a->page_count>w->pa_capacity-total||mapped>w->row_capacity-rows)
            return "TASKVM_BOUNDED_WORKSPACE_EXHAUSTED";
        for(uint32_t previous=0;mapped&&previous<index;previous++) {
            const struct dre_taskvm_allocation *b=&c->allocations[previous];
            enum dre_taskvm_privilege bp;int bw,bf,be;
            if(!role(c,b,&bp,&bw,&bf,&be))return "TASKVM_CATALOG_CHANGED";
            uint32_t bm=be?0:b->mapped_prefix_pages;
            if(bm&&a->gpu_va<b->gpu_va+(uint64_t)bm*4096&&
               b->gpu_va<a->gpu_va+(uint64_t)mapped*4096)return "TASKVM_VA_ALIAS_REFUSED";
        }
        for(uint32_t page=0;page<a->page_count;page++) {
            uint64_t pa=a->pages[page].pa;
            if(!pa||(pa&4095)||pa>=(UINT64_C(1)<<40)||
               (!(page&3)&&(pa&16383))||
               ((page&3)&&pa!=a->pages[page&~3u].pa+(uint64_t)(page&3)*4096))
                return "TASKVM_WHOLE_CPU_GRANULE_OWNERSHIP_REQUIRED";
            w->sorted_pa[total++]=pa;
            if(page<mapped) {
                uint64_t leaf;
                if(dre_taskvm_encode_leaf(pa,privilege,write,fetch,&leaf))return "TASKVM_PTE_ENCODING_REFUSED";
                w->rows[rows++]=(struct dre_taskvm_row){
                    a->gpu_va+(uint64_t)page*4096,pa,leaf,a->allocation_generation,
                    a->pin_lease,index,page,a->role,0};
            } else excludedPages++;
        }
        if(!snapshot(o,c)||o->allocation_current(o->cookie,c,a)!=1)return "TASKVM_REGISTRY_GENERATION_CHANGED";
    }
    /* All physical pages, including task-excluded private backing/padding,
     * participate. No user alias of an unlisted "protected VA" is accepted.
     * O(n log n) pages; bounded128^2 allocation-range checks above. */
    sort(w->sorted_pa,total);
    for(uint32_t i=1;i<total;i++)if(w->sorted_pa[i]==w->sorted_pa[i-1])return "TASKVM_PRIVATE_PHYSICAL_ALIAS_REFUSED";
    if(!snapshot(o,c))return "TASKVM_REGISTRY_GENERATION_CHANGED";
    *out=(struct dre_taskvm_proposal){c->common_lease,c->reset_epoch,c->task_identity,
        c->catalog_generation,rows,excludedPages};
    return "TASKVM_CPU_PERMISSION_PROPOSAL_ONLY";
}

const char *dre_taskvm_stage_table(const struct dre_taskvm_proof_ops *o,
    const struct dre_taskvm_catalog *c,struct dre_taskvm_workspace *w,
    struct dre_taskvm_proposal *out,const struct dre_taskvm_table_ops *ops,
    const struct dre_a619_smmu_view *view,uint64_t *table)
{
    if(!o||!c||!w||!out||!ops||!ops->owns_unpublished||!ops->barrier||!view||
       c->window_pages<1024||c->window_pages>DRE_A619_SMMU_MAX_PAGES||
       !view->reservation_lease||!view->task_generation||
       view->task_generation!=c->catalog_generation||!view->asid||view->asid>255)
        return "TASKVM_INVALID_TABLE_RESERVATION";
    const uint32_t l3=(c->window_pages+511u)/512u;
    const size_t bytes=(((size_t)l3+2)*4096+16383)&~(size_t)16383;
    if(view->table_bytes!=bytes||!view->table_pa||(view->table_pa&16383)||
       view->table_pa>(UINT64_C(1)<<40)-bytes||((uintptr_t)table&16383)||
       !region(table,bytes)||w->pa_capacity>DRE_TASKVM_MAX_CATALOG_PAGES||
       w->row_capacity>DRE_TASKVM_MAX_CATALOG_PAGES||
       !separate(table,bytes,w,sizeof(*w))||!workspace_separate(w,table,bytes)||
       !separate(table,bytes,out,sizeof(*out))||
       !workspace_separate(w,ops,sizeof(*ops))||!workspace_separate(w,view,sizeof(*view))||
       !separate(out,sizeof(*out),ops,sizeof(*ops))||!separate(out,sizeof(*out),view,sizeof(*view)))
        return "TASKVM_INVALID_TABLE_WORKSPACE";

    const char *status=dre_taskvm_build_proposal(o,c,w,out);
    if(strcmp(status,"TASKVM_CPU_PERMISSION_PROPOSAL_ONLY"))return status;
    if(!separate(table,bytes,c,sizeof(*c))||!separate(table,bytes,o,sizeof(*o))||
       !separate(table,bytes,ops,sizeof(*ops))||!separate(table,bytes,view,sizeof(*view))||
       !separate(table,bytes,c->allocations,(size_t)c->allocation_count*sizeof(*c->allocations)))
        return "TASKVM_TABLE_ALIASES_CATALOG";
    int in_catalog=0;
    for(uint32_t i=0;i<c->allocation_count;i++) {
        const struct dre_taskvm_allocation *a=&c->allocations[i];
        if(!separate(table,bytes,a->pages,(size_t)a->page_count*sizeof(*a->pages)))
            return "TASKVM_TABLE_ALIASES_CATALOG";
        if(a->role!=DRE_TASKVM_TRANSLATION_TABLE||a->pages[0].pa!=view->table_pa)continue;
        if(a->mapped_prefix_pages||a->page_count!=bytes/4096)
            return "TASKVM_TABLE_EXTENT_MISMATCH";
        for(uint32_t j=0;j<a->page_count;j++)
            if(a->pages[j].pa!=view->table_pa+(uint64_t)j*4096)
                return "TASKVM_TABLE_EXTENT_MISMATCH";
        in_catalog=1;
    }
    if(!in_catalog||!snapshot(o,c)||ops->owns_unpublished(ops->cookie,c,view,table)!=1)
        return "TASKVM_UNPUBLISHED_TABLE_OWNERSHIP_REQUIRED";

    const uint64_t descriptor=UINT64_C(0x8000000000000003);
    memset(table,0,bytes);
    table[(c->va_base>>30)&511]=(view->table_pa+4096)|descriptor;
    for(uint32_t i=0;i<l3;i++)
        table[512+((c->va_base>>21)&511)+i]=(view->table_pa+8192+(uint64_t)i*4096)|descriptor;
    for(uint32_t i=0;i<out->row_count;i++)
        table[1024+(w->rows[i].va-c->va_base)/4096]=w->rows[i].proposed_leaf;
    ops->barrier(ops->cookie);
    if(!snapshot(o,c)||ops->owns_unpublished(ops->cookie,c,view,table)!=1)
        return "TASKVM_TABLE_STAGING_UNCERTAIN";
    return "TASKVM_TABLE_STAGED_UNPUBLISHED";
}
