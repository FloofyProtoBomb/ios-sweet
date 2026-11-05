/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only catalog connection.
 * Descriptor data+padding observation only. Genuine DATA_ONLY coverage cannot
 * satisfy plan.c's complete protected-physical inventory. NO planner is called.
 */
#include "registry_binding.h"
#include <string.h>

static int region(const void *p,size_t bytes)
{return p&&bytes&&(uintptr_t)p<=UINTPTR_MAX-bytes;}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    return region(a,an)&&region(b,bn)&&
        ((uintptr_t)a+an<=(uintptr_t)b||(uintptr_t)b+bn<=(uintptr_t)a);
}
static void sift(uint64_t *a,uint32_t root,uint32_t count)
{
    while(root<count/2) {
        uint32_t child=root*2+1;
        if(child+1<count&&a[child]<a[child+1])child++;
        if(a[root]>=a[child])break;
        uint64_t value=a[root];a[root]=a[child];a[child]=value;root=child;
    }
}
static void sort(uint64_t *a,uint32_t count)
{
    for(uint32_t i=count/2;i;i--)sift(a,i-1,count);
    for(uint32_t end=count;end>1;end--) {
        uint64_t value=a[0];a[0]=a[end-1];a[end-1]=value;sift(a,0,end-1);
    }
}
/* Ephemeral synchronous arguments, not ownership proofs or planner objects.
 * The real service retains authoritative descriptor/client/task references. */
struct invocation {
    const struct dre_gpu_native_allocator *allocator;
    struct dre_taskvm_registry_binding *binding;
    struct dre_taskvm_registry_workspace *workspace;
    struct dre_taskvm_registry_observation *output;
    uint64_t connection_generation;
    uint32_t visits;
    const char *reason;
};
static int scratch_range(const struct invocation *i,const void *p,size_t bytes)
{
    const struct dre_gpu_allocation_catalog_snapshot *s=&i->binding->borrowed;
    const struct dre_gpu_native_allocator *a=i->allocator;
    if(!region(p,bytes)||s->record_count>DRE_GPU_CATALOG_MAX_ALLOCATIONS)return 0;
    for(uint32_t n=0;n<s->record_count;n++) {
        const struct dre_gpu_allocation_catalog_record *r=&s->records[n];
        uintptr_t base=(uintptr_t)r->memory.cpu,address=(uintptr_t)p;
        if(r->purpose!=DRE_GPU_ALLOC_CATALOG_SCRATCH||r->task_identity||r->connection_generation||
           r->cpu_mapped||r->published||r->mapping_state!=DRE_GPU_CATALOG_NO_MAPPING_RECORD||
           r->gpu_va||r->mapping_lease||r->mapping_generation||r->mapped_prefix_pages||
           !base||address<base||address-base>r->memory.bytes||bytes>r->memory.bytes-(address-base))continue;
        /* Not a generic private-range label: resolve THIS real immutable
         * no-GPU scratch allocation under the genuine current registry. */
        return a->catalog_record_current(a->cookie,s,n)==1&&
            a->owns_private_cpu(a->cookie,p,bytes)==1;
    }
    return 0;
}
static int private_workspace(const struct invocation *i)
{
    const struct dre_gpu_native_allocator *a=i->allocator;
    const struct dre_taskvm_registry_workspace *w=i->workspace;
    if(!scratch_range(i,i->binding,sizeof(*i->binding))||!scratch_range(i,w,sizeof(*w))||
       !scratch_range(i,i->output,sizeof(*i->output)))return 0;
    if(w->reserved||!w->page_capacity||w->page_capacity>DRE_GPU_CATALOG_MAX_PAGES||
       ((uintptr_t)w->sorted_pa&7))return 0;
    size_t bytes=(size_t)w->page_capacity*sizeof(*w->sorted_pa);
    return separate(i->binding,sizeof(*i->binding),w->sorted_pa,bytes)&&
        separate(w,sizeof(*w),w->sorted_pa,bytes)&&
        separate(i->output,sizeof(*i->output),w->sorted_pa,bytes)&&
        separate(a,sizeof(*a),w->sorted_pa,bytes)&&
        scratch_range(i,w->sorted_pa,bytes);
}
static int private_page_list(const struct invocation *i,const struct dre_gpu_allocation_catalog_record *r)
{
    const struct dre_gpu_native_allocator *a=i->allocator;
    if(!r->memory.pages||!r->memory.page_count||r->memory.page_count>DRE_GPU_CATALOG_MAX_PAGES||
       ((uintptr_t)r->memory.pages&7))return 0;
    size_t bytes=(size_t)r->memory.page_count*sizeof(*r->memory.pages);
    /* Protect EVERY authoritative metadata range before any scratch/output
     * store. Kernel-only ownership of its exact bytes is not ownership of the
     * surrounding shared16KiB heap granule as task-mappable physical backing. */
    return separate(i->binding,sizeof(*i->binding),r->memory.pages,bytes)&&
        separate(i->output,sizeof(*i->output),r->memory.pages,bytes)&&
        separate(i->workspace,sizeof(*i->workspace),r->memory.pages,bytes)&&
        separate(a,sizeof(*a),r->memory.pages,bytes)&&
        separate(i->workspace->sorted_pa,(size_t)i->workspace->page_capacity*sizeof(uint64_t),r->memory.pages,bytes)&&
        a->owns_private_cpu(a->cookie,r->memory.pages,bytes)==1;
}
static int known_purpose(uint32_t p)
{
    switch(p) {
    case DRE_GPU_ALLOC_UNKNOWN:case DRE_GPU_ALLOC_BOOT_TABLES:
    case DRE_GPU_ALLOC_CPU_METADATA:case DRE_GPU_ALLOC_PRIVATE_CONTROL:
    case DRE_GPU_ALLOC_RING:case DRE_GPU_ALLOC_SQE_UCODE:case DRE_GPU_ALLOC_CP_SCRATCH:
    case DRE_GPU_ALLOC_CP_POWERUP:case DRE_GPU_ALLOC_PRIVATE_COMPLETION:
    case DRE_GPU_ALLOC_IMMUTABLE_IB:case DRE_GPU_ALLOC_TRUSTED_STATE:
    case DRE_GPU_ALLOC_CATALOG_SCRATCH:case DRE_GPU_ALLOC_TASK_BO:return 1;
    default:return 0;
    }
}
static const char *record_shape(const struct dre_gpu_allocation_catalog_snapshot *s,
    const struct dre_gpu_allocation_catalog_record *r)
{
    if(!r->allocation||!r->allocation_generation||!r->pin_lease||r->reserved||r->reserved2||
       r->published>1||r->cpu_mapped>1||!r->memory.cpu||!r->memory.page_count||
       (r->memory.page_count&3u)||r->memory.page_count>DRE_GPU_CATALOG_MAX_PAGES||
       r->memory.bytes!=(uint64_t)r->memory.page_count*4096||((uintptr_t)r->memory.cpu&16383)||
       r->memory.cache_contract!=DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE||
       r->mapped_prefix_pages>r->memory.page_count||!known_purpose(r->purpose)||
       (r->granted_access!=DRE_GPU_ACCESS_READ&&
        r->granted_access!=(DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_WRITE)&&
        r->granted_access!=(DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_EXECUTE)))
        return "TASKVM_REGISTRY_FULL_DESCRIPTOR_DATA_AND_PADDING_REQUIRED";
    if(r->purpose==DRE_GPU_ALLOC_TASK_BO) {
        if(!r->task_identity||!r->connection_generation)return "TASKVM_REGISTRY_REGISTERED_TASK_OWNER_REQUIRED";
    } else if(r->task_identity||r->connection_generation||r->cpu_mapped)
        return "TASKVM_REGISTRY_KERNEL_PURPOSE_OWNER_MISMATCH";
    switch(r->mapping_state) {
    case DRE_GPU_CATALOG_NO_MAPPING_RECORD:
        /* NO record is UNKNOWN, never evidence that a GPU cannot reference it. */
        if(r->gpu_va||r->mapping_lease||r->mapping_generation||r->mapped_prefix_pages)
            return "TASKVM_REGISTRY_UNKNOWN_MAPPING_CARRIES_UNPROVEN_VALUES";
        break;
    case DRE_GPU_CATALOG_MAPPING_STAGED:
    case DRE_GPU_CATALOG_MAPPING_ACTIVE:
        if(!r->mapping_lease||!r->mapping_generation||!r->mapped_prefix_pages||
           (r->gpu_va&4095)||r->gpu_va>=(UINT64_C(1)<<39)||
           (uint64_t)r->mapped_prefix_pages*4096>(UINT64_C(1)<<39)-r->gpu_va)
            return "TASKVM_REGISTRY_ACTUAL_TYPED_MAPPING_HANDLE_REQUIRED";
        /* Only a target's real shared reservation permits this comparison;
         * a foreign/native VM record is not relabeled to that reservation. */
        if(r->purpose==DRE_GPU_ALLOC_TASK_BO&&r->task_identity==s->task_identity&&
           r->connection_generation==s->connection_generation&&
           s->window_kind==DRE_GPU_CATALOG_BOOTSTRAP_SHARED_WINDOW) {
            uint64_t bytes=(uint64_t)r->mapped_prefix_pages*4096;
            uint64_t window=(uint64_t)s->window_pages*4096;
            if(r->gpu_va<s->va_base||bytes>window||r->gpu_va-s->va_base>window-bytes)
                return "TASKVM_REGISTRY_TARGET_MAPPING_OUTSIDE_ACTUAL_RESERVATION";
        }
        break;
    case DRE_GPU_CATALOG_MAPPING_RETIRED:
        if(r->gpu_va||r->mapped_prefix_pages||!r->mapping_lease||!r->mapping_generation)
            return "TASKVM_REGISTRY_RETIRED_HANDLE_HISTORY_REQUIRED";
        break;
    default:return "TASKVM_REGISTRY_UNRECOGNIZED_MAPPING_STATE";
    }
    for(uint32_t p=0;p<r->memory.page_count;p++) {
        uint64_t pa=r->memory.pages[p].pa;
        if(!pa||(pa&4095)||pa>=(UINT64_C(1)<<40)||
           (!(p&3u)&&(pa&16383))||
           ((p&3u)&&pa!=r->memory.pages[p&~3u].pa+(uint64_t)(p&3u)*4096)||
           r->memory.pages[p].permissions!=r->granted_access)
            return "TASKVM_REGISTRY_FULL_DATA_GRANULE_OR_ACCESS_MISMATCH";
    }
    return 0;
}
static int visit(void *cookie,const struct dre_gpu_allocation_catalog_snapshot *s)
{
    struct invocation *i=cookie;const struct dre_gpu_native_allocator *a=i->allocator;
    if(i->visits++||s!=&i->binding->borrowed||a->catalog_current(a->cookie,s)!=1) {
        i->reason="TASKVM_REGISTRY_REAL_SYNCHRONOUS_SCOPE_REQUIRED";return DRE_GPU_STALE;
    }
    if(!private_workspace(i)) {
        i->reason="TASKVM_REGISTRY_PRIVATE_WIRED_INSPECTION_WORKSPACE_REQUIRED";return DRE_GPU_UNSUPPORTED;
    }
    if(s->version!=DRE_GPU_CATALOG_VERSION||s->struct_bytes!=sizeof(*s)||s->reserved||
       !s->scope_token||!s->catalog_generation||!s->common_lease||!s->reset_epoch||
       !s->task_identity||s->connection_generation!=i->connection_generation||
       !s->record_count||s->record_count>DRE_GPU_CATALOG_MAX_ALLOCATIONS||
       !s->total_pages||s->total_pages>DRE_GPU_CATALOG_MAX_PAGES||
       s->coverage_kind!=DRE_GPU_CATALOG_ALLOCATOR_DATA_ONLY) {
        i->reason="TASKVM_REGISTRY_UNQUALIFIED_INVENTORY_SCOPE";return DRE_GPU_UNSUPPORTED;
    }
    if(s->window_kind==DRE_GPU_CATALOG_WINDOW_ABSENT) {
        if(s->window_pages||s->va_base) {i->reason="TASKVM_REGISTRY_ABSENT_WINDOW_HAS_VALUES";return DRE_GPU_INVALID;}
    } else if(s->window_kind!=DRE_GPU_CATALOG_BOOTSTRAP_SHARED_WINDOW||
       s->window_pages<1024||s->window_pages>DRE_GPU_CATALOG_MAX_PAGES||
       (s->window_pages&(s->window_pages-1))||(s->va_base&0x1fffff)||
       s->va_base>=(UINT64_C(1)<<39)||
       (uint64_t)s->window_pages*4096>(UINT64_C(1)<<30)-(s->va_base&0x3fffffff)) {
        i->reason="TASKVM_REGISTRY_ACTUAL_WINDOW_SHAPE_REFUSED";return DRE_GPU_INVALID;
    }
    uint32_t total=0;
    for(uint32_t n=0;n<s->record_count;n++) {
        const struct dre_gpu_allocation_catalog_record *r=&s->records[n];
        if(a->catalog_record_current(a->cookie,s,n)!=1) {
            i->reason="TASKVM_REGISTRY_DESCRIPTOR_GENERATION_CHANGED";return DRE_GPU_STALE;
        }
        if(!private_page_list(i,r)) {
            i->reason="TASKVM_REGISTRY_PRIVATE_RETAINED_PAGE_LIST_REQUIRED";return DRE_GPU_UNSUPPORTED;
        }
        const char *error=record_shape(s,r);
        if(error) {i->reason=error;return DRE_GPU_INVALID;}
        if(r->memory.page_count>DRE_GPU_CATALOG_MAX_PAGES-total) {
            i->reason="TASKVM_REGISTRY_DATA_PAGE_COUNT_LIMIT";return DRE_GPU_LIMIT;
        }
        total+=r->memory.page_count;
    }
    if(total!=s->total_pages||total>i->workspace->page_capacity||a->catalog_current(a->cookie,s)!=1) {
        i->reason="TASKVM_REGISTRY_FULL_DATA_COUNT_OR_GENERATION_REFUSED";return DRE_GPU_STALE;
    }
    /* All metadata/scratch/output ranges pass before these first stores. */
    uint32_t at=0;
    for(uint32_t n=0;n<s->record_count;n++)
        for(uint32_t p=0;p<s->records[n].memory.page_count;p++)
            i->workspace->sorted_pa[at++]=s->records[n].memory.pages[p].pa;
    sort(i->workspace->sorted_pa,total);
    for(uint32_t p=1;p<total;p++)if(i->workspace->sorted_pa[p]==i->workspace->sorted_pa[p-1]) {
        i->reason="TASKVM_REGISTRY_DESCRIPTOR_DATA_PHYSICAL_ALIAS_REFUSED";return DRE_GPU_INVALID;
    }
    if(a->catalog_current(a->cookie,s)!=1) {
        i->reason="TASKVM_REGISTRY_GENERATION_CHANGED_AFTER_DATA_INSPECTION";return DRE_GPU_STALE;
    }
    struct dre_taskvm_registry_observation *out=i->output;memset(out,0,sizeof(*out));
    out->version=DRE_TASKVM_REGISTRY_OBSERVATION_VERSION;out->struct_bytes=sizeof(*out);
    out->catalog_generation=s->catalog_generation;out->common_lease=s->common_lease;
    out->reset_epoch=s->reset_epoch;out->connection_generation=s->connection_generation;
    out->va_base=s->va_base;out->coverage_kind=s->coverage_kind;out->window_kind=s->window_kind;
    out->window_pages=s->window_pages;out->record_count=s->record_count;out->total_pages=total;
    for(uint32_t n=0;n<s->record_count;n++) {
        const struct dre_gpu_allocation_catalog_record *r=&s->records[n];
        uint32_t relation=DRE_TASKVM_REGISTRY_KERNEL_ALLOCATION;
        if(r->purpose==DRE_GPU_ALLOC_TASK_BO) {
            relation=r->task_identity==s->task_identity&&r->connection_generation==s->connection_generation?
                DRE_TASKVM_REGISTRY_TARGET_CONNECTION:DRE_TASKVM_REGISTRY_FOREIGN_CONNECTION;
            if(relation==DRE_TASKVM_REGISTRY_FOREIGN_CONNECTION)out->foreign_connection_count++;
        }
        out->records[n]=(struct dre_taskvm_registry_record_observation){r->allocation_generation,r->pin_lease,
            r->connection_generation,r->gpu_va,r->mapping_lease,r->mapping_generation,r->purpose,
            r->granted_access,r->memory.page_count,r->mapped_prefix_pages,r->mapping_state,
            r->published,r->cpu_mapped,relation};
        if(r->purpose==DRE_GPU_ALLOC_UNKNOWN)out->unknown_purpose_count++;
        if(r->mapping_state==DRE_GPU_CATALOG_NO_MAPPING_RECORD)out->no_mapping_record_count++;
    }
    if(a->catalog_current(a->cookie,s)!=1) {
        memset(out,0,sizeof(*out));i->reason="TASKVM_REGISTRY_FINAL_GENERATION_CHANGED";return DRE_GPU_STALE;
    }
    /* This actual coverage lacks shared heap/reserved firmware physical
     * exclusion. NO dre_taskvm_build_proposal call or pretend-complete flag.
     * Status0 here reports factual inventory observation, never plan acceptance. */
    i->reason="TASKVM_REGISTRY_DATA_OBSERVED_PLANNER_REFUSED";return DRE_GPU_OK;
}
const char *dre_taskvm_inspect_registered_catalog(const struct dre_gpu_native_allocator *a,
    const void *registered_client,uint64_t connection_generation,
    struct dre_taskvm_registry_binding *b,struct dre_taskvm_registry_workspace *w,
    struct dre_taskvm_registry_observation *out)
{
    if(!a||!registered_client||!connection_generation||!b||!w||!out||((uintptr_t)a&7)||
       ((uintptr_t)b&7)||((uintptr_t)w&7)||((uintptr_t)out&7)||
       !separate(b,sizeof(*b),a,sizeof(*a))||!separate(b,sizeof(*b),w,sizeof(*w))||
       !separate(b,sizeof(*b),out,sizeof(*out))||!separate(w,sizeof(*w),out,sizeof(*out))||
       !separate(out,sizeof(*out),a,sizeof(*a))||!separate(w,sizeof(*w),a,sizeof(*a))||
       a->version!=DRE_GPU_ALLOCATOR_VERSION||a->struct_bytes!=sizeof(*a)||!a->cookie||
       !a->with_catalog||!a->catalog_current||!a->catalog_record_current||!a->owns_private_cpu)
        return "TASKVM_REGISTRY_ACTUAL_ALLOCATOR_API_REQUIRED";
    struct invocation invocation={a,b,w,out,connection_generation,0,
        "TASKVM_REGISTRY_REAL_DESCRIPTOR_TASK_SCOPE_REFUSED"};
    int status=a->with_catalog(a->cookie,registered_client,connection_generation,&b->borrowed,visit,&invocation);
    if(status==DRE_GPU_OK&&invocation.visits==1&&
       !strcmp(invocation.reason,"TASKVM_REGISTRY_DATA_OBSERVED_PLANNER_REFUSED"))return invocation.reason;
    if(status==DRE_GPU_STALE||status==DRE_GPU_FAULT)
        return "TASKVM_REGISTRY_SCOPE_OR_DESCRIPTOR_GENERATION_CHANGED";
    if(!invocation.visits&&status==DRE_GPU_NOT_READY)
        return "TASKVM_REGISTRY_PRIVATE_SCRATCH_OR_NATIVE_SCOPE_REQUIRED";
    if(!invocation.visits&&status==DRE_GPU_LIMIT)
        return "TASKVM_REGISTRY_NATIVE_DESCRIPTOR_OR_PAGE_BOUND_REFUSED";
    return invocation.reason;
}
