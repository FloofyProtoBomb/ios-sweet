/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight draft. Concrete bounded ownership transitions only.
 * No allocation/free, command publication, test/compile or capability grant.
 */
#include "dre_gpu_access_epoch.h"
static int quarantine(struct dre_gpu_access_epoch *s)
{ s->unsafe=1;s->phase=DRE_GPU_ACCESS_QUARANTINED;return DRE_GPU_FAULT; }
static int live(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o)
{
    if(!s||!o||!s->initialized)return DRE_GPU_INVALID;
    if(s->unsafe)return DRE_GPU_FAULT;
    if(!o->owns_memory||o->owns_memory(o->cookie,s)!=1)return quarantine(s);
    if(s->map_lease&&(!o->owns_task_map||o->owns_task_map(o->cookie,s)!=1))return quarantine(s);
    return DRE_GPU_OK;
}
static int transfer_available(const struct dre_gpu_access_ops *o)
{ return o&&o->cache&&o->cpu_begin&&o->cpu_end&&o->barrier; }
static int barrier(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o)
{ return o->barrier(o->cookie)==DRE_GPU_OK&&live(s,o)==DRE_GPU_OK?DRE_GPU_OK:quarantine(s); }
int dre_gpu_access_attach(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o,
    uint64_t bo,uint64_t generation,uint64_t va,uint64_t gpu_lease,uint64_t bytes,
    uint32_t granted,uint64_t reset,uint64_t map_lease,uint64_t address)
{
    if(!s||!o||!bo||!generation||!gpu_lease||!map_lease||!va||!bytes||!address||
       bytes%DRE_GPU_CPU_PAGE_BYTES||address%DRE_GPU_CPU_PAGE_BYTES||
       address>UINT64_MAX-bytes||va>UINT64_MAX-bytes||!(granted&DRE_GPU_ACCESS_READ)||
       (granted&~7u)||(granted&6u)==6u)return DRE_GPU_INVALID;
    if(s->unsafe)return DRE_GPU_FAULT;
    if(!transfer_available(o)||!o->unmap_actual||!o->query||!o->owns_memory||!o->owns_task_map)
        return DRE_GPU_UNSUPPORTED;
    if(s->initialized) {
        if(s->map_lease||s->active_epoch||s->ticket||s->phase!=DRE_GPU_ACCESS_IDLE)return DRE_GPU_BUSY;
        if(s->bo!=bo||s->session_generation!=generation||s->gpu_va!=va||s->gpu_mapping_lease!=gpu_lease||
           s->bytes!=bytes||s->granted_access!=granted||s->reset_generation!=reset)return DRE_GPU_STALE;
        s->map_lease=map_lease;s->task_address=address;
        return live(s,o); /* preserve monotonically allocated ownership epochs */
    }
    s->bo=bo;s->session_generation=generation;s->gpu_va=va;s->gpu_mapping_lease=gpu_lease;
    s->bytes=bytes;s->granted_access=granted;s->reset_generation=reset;
    s->map_lease=map_lease;s->task_address=address;s->next_epoch=0;s->active_epoch=0;s->ticket=0;
    s->cpu_access=s->gpu_access=0;s->phase=DRE_GPU_ACCESS_IDLE;s->initialized=1;
    return live(s,o);
}
int dre_gpu_access_cpu_begin(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o,
    uint32_t access,uint64_t *epoch)
{
    if(!epoch||!access||(access&~3u))return DRE_GPU_INVALID;
    *epoch=0;int r=live(s,o);if(r!=DRE_GPU_OK)return r;
    if(!s->map_lease||!transfer_available(o))return DRE_GPU_UNSUPPORTED;
    if(s->phase!=DRE_GPU_ACCESS_IDLE||s->active_epoch||s->ticket)return DRE_GPU_BUSY;
    if(s->next_epoch==UINT64_MAX)return DRE_GPU_LIMIT;
    s->active_epoch=++s->next_epoch;s->cpu_access=access;s->phase=DRE_GPU_ACCESS_CPU_TRANSFERRING;
    *epoch=s->active_epoch;
    if(o->cache(o->cookie,DRE_GPU_CACHE_FROM_GPU)!=DRE_GPU_OK||
       o->cpu_begin(o->cookie,access)!=DRE_GPU_OK||barrier(s,o)!=DRE_GPU_OK)
        return quarantine(s);
    s->phase=DRE_GPU_ACCESS_CPU_OWNED;return DRE_GPU_OK;
}
int dre_gpu_access_cpu_end(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o,uint64_t epoch)
{
    int r=live(s,o);if(r!=DRE_GPU_OK)return r;
    if(!epoch||s->active_epoch!=epoch||s->phase!=DRE_GPU_ACCESS_CPU_OWNED)return DRE_GPU_STALE;
    if(!transfer_available(o))return DRE_GPU_UNSUPPORTED;
    s->phase=DRE_GPU_ACCESS_CPU_TRANSFERRING;
    if(o->cache(o->cookie,DRE_GPU_CACHE_TO_GPU)!=DRE_GPU_OK||
       o->cpu_end(o->cookie,s->cpu_access)!=DRE_GPU_OK||barrier(s,o)!=DRE_GPU_OK)
        return quarantine(s);
    s->cpu_access=0;s->active_epoch=0;s->phase=DRE_GPU_ACCESS_IDLE;
    return DRE_GPU_OK;
}
int dre_gpu_access_gpu_prepare(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o,
    uint32_t access,uint64_t *epoch)
{
    if(!epoch||!access||(access&~3u))return DRE_GPU_INVALID;
    *epoch=0;int r=live(s,o);if(r!=DRE_GPU_OK)return r;
    if(access&~s->granted_access)return DRE_GPU_INVALID;
    if(!transfer_available(o)||!o->query)return DRE_GPU_UNSUPPORTED;
    if(s->phase!=DRE_GPU_ACCESS_IDLE||s->active_epoch||s->ticket)return DRE_GPU_BUSY;
    if(s->next_epoch==UINT64_MAX)return DRE_GPU_LIMIT;
    s->active_epoch=++s->next_epoch;s->gpu_access=access;s->phase=DRE_GPU_ACCESS_GPU_PREPARED;
    *epoch=s->active_epoch;
    if(o->cache(o->cookie,DRE_GPU_CACHE_TO_GPU)!=DRE_GPU_OK||barrier(s,o)!=DRE_GPU_OK)
        return quarantine(s);
    return DRE_GPU_OK;
}
int dre_gpu_access_gpu_publish(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o,
    uint64_t epoch,uint64_t ticket)
{
    if(!s||!s->initialized||!epoch||!ticket||s->active_epoch!=epoch)return DRE_GPU_STALE;
    if(s->ticket)return quarantine(s);
    /* The actual native provider has already published. Preserve its ticket
     * before checking aliases, so an ensuing failure cannot forget DMA. */
    s->ticket=ticket;
    if(s->unsafe||s->phase!=DRE_GPU_ACCESS_GPU_PREPARED)return quarantine(s);
    s->phase=DRE_GPU_ACCESS_GPU_PUBLISHED;
    return live(s,o);
}
int dre_gpu_access_cancel_unpublished(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o,uint64_t epoch)
{
    int r=live(s,o);if(r!=DRE_GPU_OK)return r;
    if(!epoch||s->phase!=DRE_GPU_ACCESS_GPU_PREPARED||s->active_epoch!=epoch||s->ticket)return DRE_GPU_STALE;
    if(!transfer_available(o))return DRE_GPU_UNSUPPORTED;
    if(o->cache(o->cookie,DRE_GPU_CACHE_FROM_GPU)!=DRE_GPU_OK||barrier(s,o)!=DRE_GPU_OK)return quarantine(s);
    s->gpu_access=0;s->active_epoch=0;s->phase=DRE_GPU_ACCESS_IDLE;return DRE_GPU_OK;
}
int dre_gpu_access_gpu_retire(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o,
    uint64_t epoch,uint64_t ticket)
{
    int r=live(s,o);if(r!=DRE_GPU_OK)return r;
    if(!epoch||!ticket||s->phase!=DRE_GPU_ACCESS_GPU_PUBLISHED||s->active_epoch!=epoch||s->ticket!=ticket)
        return DRE_GPU_STALE;
    if(!o->query||!transfer_available(o))return DRE_GPU_UNSUPPORTED;
    struct dre_gpu_native_completion completion={0};
    r=o->query(o->cookie,ticket,&completion);if(r!=DRE_GPU_OK)return quarantine(s);
    if(completion.reserved||completion.resources_retired>1||completion.state>DRE_GPU_FENCE_RESET||
       completion.reset_generation!=s->reset_generation||
       (completion.state==DRE_GPU_FENCE_PENDING&&completion.resources_retired)||
       (completion.state==DRE_GPU_FENCE_COMPLETE&&completion.hardware_error))return quarantine(s);
    if(completion.state==DRE_GPU_FENCE_PENDING||!completion.resources_retired)return DRE_GPU_BUSY;
    if(o->cache(o->cookie,DRE_GPU_CACHE_FROM_GPU)!=DRE_GPU_OK||barrier(s,o)!=DRE_GPU_OK)return quarantine(s);
    s->ticket=0;s->active_epoch=0;s->gpu_access=0;s->phase=DRE_GPU_ACCESS_IDLE;
    /* A terminal GPU error remains visible; actual retired ownership can be
     * reclaimed without pretending the workload completed successfully. */
    return completion.state==DRE_GPU_FENCE_COMPLETE?DRE_GPU_OK:DRE_GPU_FAULT;
}
int dre_gpu_access_detach(struct dre_gpu_access_epoch *s,const struct dre_gpu_access_ops *o)
{
    int r=live(s,o);if(r!=DRE_GPU_OK)return r;
    if(!s->map_lease)return DRE_GPU_OK;
    if(s->phase==DRE_GPU_ACCESS_CPU_OWNED) {
        r=dre_gpu_access_cpu_end(s,o,s->active_epoch);if(r!=DRE_GPU_OK)return r;
    }
    if(s->phase!=DRE_GPU_ACCESS_IDLE&&s->phase!=DRE_GPU_ACCESS_GPU_PUBLISHED)return DRE_GPU_BUSY;
    if(!o->unmap_actual)return DRE_GPU_UNSUPPORTED;
    if(o->unmap_actual(o->cookie)!=DRE_GPU_OK)return quarantine(s);
    s->map_lease=0;s->task_address=0;
    /* GPU-published epoch/ticket and every BO pin remain held independently. */
    return live(s,o);
}
