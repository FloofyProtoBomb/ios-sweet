/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source. This composes actual implementations and
 * mandatory real platform authorities; it supplies no default hardware policy.
 */
#include "native_provider.h"
#if defined(DRE)
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <string.h>
#include "../../native_board.h"
#include "boot_memory.h"
#include "io_router.h"

/* CPU table/VM/CP control lives INSIDE one actual allocator-owned Normal-NC
 * object, not an exempt arbitrary IOMalloc range posing as owned bookkeeping. */
struct dre_gpu_platform_control {
    dre_gpu_boot_binding binding;
    dre_gpu_boot_memory boot;
    dre_gpu_io_router router;
    dre_a619_rbbm rbbm;
    dre_a619_smmu reservation;
    dre_gpu_boot_domain_ops domain;
    dre_holi_power_config power;
    dre_rpm_resource resources[3];
    dre_rpm_votes votes;
    dre_rpm_mmio rpmMMIO;
    dre_rpm_io rpmIO;
    dre_holi_gcc_policy gccPolicy;
    dre_holi_gcc_hold gcc;
    dre_gpu_fw_native_ops firmwareOps;
};
struct dre_gpu_platform_provider {
    dre_gpu_platform_provider_config config;
    const void *fdt;
    size_t fdtBytes;
    dre_holi_power_profile graph;
    dre_gpu_owner_lock serialization;
    dre_gpu_gic_policy gicPolicy;
    dre_gpu_gic_route *gic;
    dre_gpu_native_broker composed;
    dre_gpu_native_owner *owner;
    dre_rpm_native *rpm;
    dre_gpu_fw_native *firmware;
    dre_gpu_native_allocator allocator;
    dre_gpu_native_io raw;
    dre_gpu_native_allocation *controlAllocation;
    dre_gpu_native_memory controlMemory;
    dre_gpu_platform_control *control;
    dre_a619_startup *startup;
    uint64_t lease,resetEpoch;
    const char *error;
    bool lockAttempted,lockBound,acquireAttempted,acquired,configureAttempted,configured,unsafe;
};
static bool equal(const char *a,const char *b)
{ return a&&b&&!strcmp(a,b); }
static int fail(dre_gpu_platform_provider *p,const char *reason,int status,bool uncertain=false)
{
    p->error=reason;
    if(uncertain) {
        p->unsafe=true;
        if(p->owner)dre_gpu_native_owner_invalidate(p->owner,DRE_GPU_OWNER_LEASE_LOST);
    }
    return status;
}
static bool held(dre_gpu_platform_provider *p)
{
    return p&&p->lockBound&&p->serialization.check_held&&
        p->serialization.check_held(p->serialization.cookie)==DRE_GPU_OK&&
        p->config.gpu.owner_lock_held(p->config.gpu.cookie)==0&&
        p->config.rpm.owner_lock_held(p->config.rpm.cookie)==0;
}
static bool brokerComplete(const dre_gpu_native_broker *g,const dre_rpm_native_broker *r)
{
    return g->version==DRE_GPU_NATIVE_BROKER_VERSION&&g->struct_bytes==sizeof(*g)&&
        g->enter_owner&&g->leave_owner&&g->owner_lock_held&&g->acquire&&g->check_lease&&
        g->mmio_access&&g->cold_exclusive&&g->held_policy&&g->retired&&g->release&&
        r->version==DRE_RPM_NATIVE_BROKER_VERSION&&r->struct_bytes==sizeof(*r)&&
        r->owner_lock_held&&r->acquire&&r->check&&r->device_aliases&&r->access&&r->receive_owned&&r->retired&&r->release;
}
static bool configComplete(const dre_gpu_platform_provider_config *c)
{
    return c&&c->version==DRE_GPU_PLATFORM_PROVIDER_CONFIG_VERSION&&c->struct_bytes==sizeof(*c)&&
        !c->reserved&&c->bind_common_lock&&c->gic_policy&&c->firmware_policy&&
        c->aggregate_policy&&c->gcc_policy&&c->domain_reservation&&brokerComplete(&c->gpu,&c->rpm)&&
        c->window_pages>=1024&&c->window_pages<=DRE_A619_SMMU_MAX_PAGES&&
        !(c->window_pages&(c->window_pages-1))&&c->rpm_budget_ticks&&
        c->rpm_budget_ticks<=19200000&&c->cp_budget_ticks&&c->cp_budget_ticks<=19200000;
}
static int bindLock(void *cookie,const dre_gpu_owner_lock *lock,uint32_t *retained)
{
    dre_gpu_platform_provider *p=(dre_gpu_platform_provider *)cookie;
    if(retained)*retained=0;
    if(!p||!lock||!retained||p->lockAttempted||lock->version!=DRE_GPU_OWNER_LOCK_VERSION||
       lock->struct_bytes!=sizeof(*lock)||!lock->enter||!lock->leave||!lock->check_held||
       lock->check_held(lock->cookie)!=DRE_GPU_OK||!IOService::getPlatform()||
       IOService::getPlatform()->atInterruptLevel())return DRE_GPU_INVALID;
    p->lockAttempted=true;
    *retained=1; /* Before copying the actual kernel service's escaped cookie. */
    p->serialization=*lock;
    uint32_t externalRetained=0;
    int status=p->config.bind_common_lock(p->config.cookie,lock,&externalRetained);
    if(status!=DRE_GPU_OK||externalRetained!=1)
        return fail(p,"NATIVE_PLATFORM_COMMON_LOCK_BINDING_REQUIRED",
            status!=DRE_GPU_OK?status:DRE_GPU_FAULT);
    p->lockBound=true;
    if(!held(p))return fail(p,"NATIVE_PLATFORM_ACTUAL_COMMON_LOCK_MISMATCH",DRE_GPU_FAULT);
    p->error="NATIVE_PLATFORM_LOCK_BOUND_ONLY";return DRE_GPU_OK;
}
static int acquire(void *cookie,uint64_t *lease)
{
    dre_gpu_platform_provider *p=(dre_gpu_platform_provider *)cookie;
    if(!p||!lease||*lease||!held(p)||p->acquireAttempted)return DRE_GPU_INVALID;
    p->acquireAttempted=true;
    int status=p->config.gic_policy(p->config.cookie,&p->gicPolicy);
    if(status)return fail(p,"NATIVE_PLATFORM_ACTUAL_GIC_POLICY_REQUIRED",status);
    status=dre_gpu_gic_route_create(&p->config.gpu,&p->gicPolicy,&p->gic);
    if(status)return fail(p,"NATIVE_PLATFORM_GIC_REFERENCE_RETAINED",status);
    status=dre_gpu_gic_route_broker(p->gic,&p->composed);
    if(status)return fail(p,"NATIVE_PLATFORM_GIC_COMPOSITION_FAILED",status);
    status=dre_gpu_native_owner_create(&p->composed,&p->owner);
    /* owns()/native_owner_lease intentionally reject incomplete registration.
     * Retained metadata instead comes from the real acquire outputs recorded
     * by the bridge, including every failed partial hardware acquisition. */
    int metadata=dre_gpu_gic_retained_acquire(p->gic,&p->lease,&p->resetEpoch);
    *lease=p->lease;
    if(metadata)return fail(p,"NATIVE_PLATFORM_PARTIAL_LEASE_METADATA_LOST",DRE_GPU_FAULT);
    if(status||!p->owner||!p->lease||!p->resetEpoch||
       dre_gpu_native_owner_guard(p->owner)!=1||dre_gpu_native_owner_lease(p->owner)!=p->lease)
        return fail(p,"NATIVE_PLATFORM_GPU_ACQUIRE_RETAINED",status?status:DRE_GPU_NOT_READY);
    p->acquired=true;p->error="NATIVE_PLATFORM_GPU_OWNER_HELD_ONLY";return DRE_GPU_OK;
}
static int owns(void *cookie,uint64_t lease)
{
    dre_gpu_platform_provider *p=(dre_gpu_platform_provider *)cookie;
    /* Nonrecursive BASIC native map/lease/reset/IRQ ownership only. Never
     * call startup_check_live here: startup MMIO itself enters this predicate. */
    return p&&held(p)&&p->acquired&&!p->unsafe&&lease&&lease==p->lease&&p->owner&&
        dre_gpu_native_owner_guard(p->owner)==1&&dre_gpu_native_owner_lease(p->owner)==lease?1:0;
}
static bool ownedControl(dre_gpu_platform_provider *p)
{
    const dre_gpu_native_memory *m=&p->controlMemory;
    return p->controlAllocation&&m->cpu&&m->pages&&m->bytes>=sizeof(dre_gpu_platform_control)&&
        !(m->bytes&16383)&&!((uintptr_t)m->cpu&16383)&&m->page_count==m->bytes/4096&&
        m->cache_contract==DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE&&
        p->allocator.verify_memory(p->allocator.cookie,m)==1&&
        p->allocator.owns_cpu(p->allocator.cookie,m->cpu,sizeof(dre_gpu_platform_control))==1;
}
static int allocateControl(dre_gpu_platform_provider *p,const dre_gpu_native_allocator *a)
{
    if(!a||!a->allocate||!a->allocate_contiguous||!a->discard_unpublished||!a->owns_cpu||
       !a->owns_ram||!a->verify_memory||a->version!=DRE_GPU_ALLOCATOR_VERSION||
       a->struct_bytes!=sizeof(*a)||!a->classify||!a->record_mapping)return DRE_GPU_UNSUPPORTED;
    p->allocator=*a;
    uint64_t bytes=(sizeof(dre_gpu_platform_control)+16383u)&~UINT64_C(16383);
    int status=a->allocate(a->cookie,bytes,DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_WRITE,
        &p->controlAllocation,&p->controlMemory);
    if(status)return status; /* Even partially returned allocation stays held. */
    if(p->controlMemory.bytes!=bytes||!ownedControl(p))return DRE_GPU_NOT_READY;
    for(uint32_t i=0;i<p->controlMemory.page_count;i++)
        if(p->controlMemory.pages[i].permissions!=(DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_WRITE))
            return DRE_GPU_NOT_READY;
    /* Actual constructor provenance, once, on the exact verified allocation
     * before control initialization or any VM/CP pointer escapes. Never guess
     * control/table purpose from an address, retag, publish or free on error.
     */
    status=a->classify(a->cookie,p->controlAllocation,&p->controlMemory,DRE_GPU_ALLOC_PRIVATE_CONTROL);
    if(status!=DRE_GPU_OK)return status;
    p->control=(dre_gpu_platform_control *)p->controlMemory.cpu;
    bzero(p->control,sizeof(*p->control));return DRE_GPU_OK;
}
static int describe(void *cookie,uint64_t lease,dre_gpu_info_reply *out)
{
    dre_gpu_platform_provider *p=(dre_gpu_platform_provider *)cookie;
    if(!out||owns(p,lease)!=1)return DRE_GPU_NOT_READY;
    /* Real board identity, no powered GPU register probe or accelerator claim.
     * Bootstrap marker/BO tables do not supply task isolation/cache/Metal. */
    out->capabilities=0;out->unavailable=(UINT64_C(1)<<10)-1;
    out->unavailable&=~(uint64_t)DRE_GPU_MISSING_NATIVE_LEASE;
    out->chip_id=DRE_A619_CHIP_ID;out->reset_generation=p->resetEpoch;
    out->gmem_bytes=0;out->gmem_base=0;out->uche_trap_base=0;out->timestamp_frequency=0;
    out->highest_bank_bit=0;out->ubwc_swizzle=0;out->macrotile_mode=0;
    out->gpu_va_base=0;out->gpu_va_bytes=0;
    return DRE_GPU_OK;
}
static bool firmwareTable(const dre_gpu_fw_native_ops *f,uint64_t lease)
{
    return f&&f->lease==lease&&f->owns_pas13&&f->scm64_discovered&&f->scm_clocks_held&&
        f->consumers_retired&&f->scm.guard&&f->scm.smc64&&f->scm.barrier&&f->scm.now;
}
static int aggregate(dre_gpu_platform_provider *p)
{
    dre_gpu_platform_control *c=p->control;uint32_t floors[3]={},masks[3]={};
    int status=p->config.aggregate_policy(p->config.cookie,p->lease,&p->graph,floors,masks);
    if(status)return status;
    c->power=p->graph.config;
    for(unsigned rail=0;rail<3;rail++) {
        uint32_t floorMask=dre_rpm_level_mask(floors[rail]);
        if(!floorMask||!masks[rail]||(masks[rail]&~UINT32_C(0xfff))||
           !(masks[rail]&floorMask)||floors[rail]<p->graph.resources[rail].minimum_level||
           floors[rail]>p->graph.resources[rail].maximum_level)return DRE_GPU_NOT_READY;
        c->resources[rail]=p->graph.resources[rail];
        c->resources[rail].allowed_levels=masks[rail];
        c->power.aggregate_floor[rail]=floors[rail];
    }
    return DRE_GPU_OK;
}
static bool freshReservation(const dre_a619_smmu *r,const dre_gpu_boot_domain_ops *d)
{
    return d&&d->reservation_lease&&d->reserved&&d->live&&d->quiescent&&
        r->chip_id==DRE_A619_CHIP_ID&&r->tick_hz==19200000&&
        r->prerequisites==(DRE_A619_SMMU_CB_OWNED|DRE_A619_SMMU_ASID_OWNED)&&
        !r->active&&!r->changed&&!r->unsafe&&!r->tables&&!r->pages&&!r->table_pa&&
        !r->page_count&&!r->page_capacity&&!r->table_capacity_bytes;
}
static int configure(void *cookie,uint64_t lease,const dre_gpu_native_io *raw,
    const dre_gpu_native_allocator *allocator,dre_a619_startup *startup,dre_a619_start_plan *out)
{
    dre_gpu_platform_provider *p=(dre_gpu_platform_provider *)cookie;
    if(!p||!raw||!allocator||!startup||!out||owns(p,lease)!=1||p->configureAttempted||
       raw->cookie!=allocator->cookie||startup->phase!=DRE_A619_START_EMPTY||
       startup->hold_resources||out->io.cookie||out->mmio.cookie||out->votes||
       out->rbbm||out->smmu||out->cp||out->vm||out->vm_ops||out->qualify_domain)
        return DRE_GPU_INVALID;
    p->configureAttempted=true;p->startup=startup;p->raw=*raw;
    int status=dre_gpu_native_owner_bind_io(p->owner,raw);
    if(status)return fail(p,"NATIVE_PLATFORM_ACTUAL_RAW_IO_BINDING_FAILED",status);
    status=dre_gpu_native_owner_bind_startup(p->owner,startup);
    if(status)return fail(p,"NATIVE_PLATFORM_ACTUAL_STARTUP_BINDING_FAILED",status);
    status=allocateControl(p,allocator);
    if(status)return fail(p,"NATIVE_PLATFORM_OWNED_NC_CONTROL_RETAINED",status);
    dre_gpu_platform_control *c=p->control;

    const char *result=dre_gpu_fw_native_load(&p->firmware);
    if(!equal(result,"FW_NATIVE_PRIVATE_SOURCE_VERIFIED"))return fail(p,result,DRE_GPU_NOT_READY);
    status=p->config.firmware_policy(p->config.cookie,lease,&c->firmwareOps);
    if(status||!firmwareTable(&c->firmwareOps,lease))
        return fail(p,"NATIVE_PLATFORM_REAL_SCM_FIRMWARE_POLICY_REQUIRED",status?status:DRE_GPU_NOT_READY);
    /* The actual binding owns the ONE call to native PAS buffer preparation.
     * Current GPU8KiB/IPA shared16KiB granule refuses HERE, before any RPM
     * map/endpoint request, rail vote, GPU power/clock write or cold CP store.
     * No alternate mapper, widening, IPA reset or shared-page retag exists. */
    result=dre_gpu_boot_binding_firmware_prepare(&c->binding,p->firmware,&c->firmwareOps);
    if(!equal(result,"FW_NATIVE_PAS_BUFFERS_PREPARED_ONLY"))return fail(p,result,DRE_GPU_NOT_READY);
    status=aggregate(p);
    if(status)return fail(p,"NATIVE_PLATFORM_REAL_AGGREGATE_RAIL_PLAN_REQUIRED",status);
    status=p->config.domain_reservation(p->config.cookie,lease,&c->reservation,&c->domain);
    if(status||!freshReservation(&c->reservation,&c->domain)||
       c->domain.reserved(c->domain.cookie,lease,c->domain.reservation_lease,&c->reservation)!=1)
        return fail(p,"NATIVE_PLATFORM_REAL_CB_SMR_ASID_RESERVATION_RETAINED",status?status:DRE_GPU_NOT_READY);

    status=dre_rpm_native_create(&p->config.rpm,&p->rpm);
    if(status) {
        const char *reason=NULL;uint32_t problems=0;uint64_t writes=0;
        if(p->rpm)(void)dre_rpm_native_diagnostic(p->rpm,&reason,&problems,&writes);
        return fail(p,reason?reason:"NATIVE_PLATFORM_RPM_MAPPING_OR_PARTIAL_LEASE_RETAINED",status);
    }
    /* Actual common lease identity is checked, never remapped or assigned a
     * matching number here. Foreign vMPM words remain excluded by RPM/router. */
    status=dre_gpu_io_router_bind(&c->router,p->owner,p->rpm,&c->binding);
    if(status)return fail(p,"NATIVE_PLATFORM_ACTUAL_GPU_RPM_LEASE_IDENTITY_REQUIRED",status);
    status=dre_gpu_boot_binding_init(&c->binding,&c->router,&p->allocator,startup,
        &c->boot,&c->reservation,&c->domain);
    if(status)return fail(p,"NATIVE_PLATFORM_REAL_VM_CP_BINDING_REQUIRED",status);
    result=dre_gpu_boot_binding_prepare(&c->binding,p->config.window_pages);
    if(!equal(result,"BOOT_BINDING_CPU_STAGED"))return fail(p,result,DRE_GPU_NOT_READY);

    dre_a619_start_plan plan={};
    c->rbbm.chip_id=DRE_A619_CHIP_ID;c->rbbm.highest_bank_bit=14;
    c->rbbm.gmem_bytes=524288;c->rbbm.min_access_length=32;c->rbbm.ubwc_mode=2;
    plan.rbbm=&c->rbbm;plan.votes=&c->votes;plan.power_config=c->power;
    plan.cp_budget_ticks=p->config.cp_budget_ticks;
    result=dre_gpu_boot_binding_plan(&c->binding,&plan);
    if(!equal(result,"BOOT_BINDING_PLAN_BOUND"))return fail(p,result,DRE_GPU_NOT_READY);
    if(!plan.qualify_domain||plan.vm!=&c->boot.vm||plan.smmu!=&c->boot.domain||
       plan.cp!=&c->boot.cp||plan.io.cookie!=&c->router||plan.mmio.cookie!=&c->router)
        return fail(p,"NATIVE_PLATFORM_ACTUAL_START_PLAN_IDENTITY_MISMATCH",DRE_GPU_FAULT);
    if(!dre_rpm_bind_mmio(&c->rpmMMIO,&plan.mmio,&c->router.rpm_map,&c->rpmIO))
        return fail(p,"NATIVE_PLATFORM_REAL_RPM_IO_BINDING_FAILED",DRE_GPU_NOT_READY);
    /* These are actual bounded GLINK publishes and matching real ACK holds,
     * not prefilled vote state. No rollback/lower/off request is ever issued. */
    result=dre_rpm_votes_open(&c->votes,&c->rpmIO,&c->router.rpm_protocol,c->resources,p->config.rpm_budget_ticks);
    if(!equal(result,"RPM_ENDPOINT_OPEN"))return fail(p,result,DRE_GPU_NOT_READY,true);
    for(unsigned rail=0;rail<3;rail++) {
        result=dre_rpm_hold_level(&c->votes,&c->rpmIO,rail,c->power.aggregate_floor[rail],p->config.rpm_budget_ticks);
        if(!equal(result,"RPM_VOTE_ACKED"))return fail(p,result,DRE_GPU_NOT_READY,true);
    }
    status=p->config.gcc_policy(p->config.cookie,lease,&c->votes,&c->power,&c->gccPolicy);
    if(status||!c->gccPolicy.acquire||!c->gccPolicy.check||
       !c->gccPolicy.lock_register||!c->gccPolicy.unlock_register)
        return fail(p,"NATIVE_PLATFORM_REAL_GCC_SHARED_SOURCE_POLICY_REQUIRED",
            status?status:DRE_GPU_NOT_READY,true);
    /* Only actual named-parent holds: real existing600MHz GPLL0 and genuine
     * source/global-register policy are checked. No PLL/RCG/GDSC/reset/off.
     * Every partial source reference and attempted store stays resident. */
    result=dre_holi_gcc_acquire(&c->gcc,&plan.mmio,&c->gccPolicy,&c->votes,p->config.rpm_budget_ticks);
    if(!equal(result,"GCC_NAMED_PARENTS_HELD"))return fail(p,result,DRE_GPU_NOT_READY,true);
    result=dre_holi_gcc_validate(&c->gcc);
    if(!equal(result,"GCC_NAMED_PARENTS_HELD"))return fail(p,result,DRE_GPU_FAULT,true);
    if(owns(p,lease)!=1||!ownedControl(p))
        return fail(p,"NATIVE_PLATFORM_OWNERSHIP_LOST_AFTER_ACTUAL_RPM_HOLDS",DRE_GPU_FAULT,true);
    /* Kernel binding, not this provider, calls startup_begin exactly once
     * after marking actual allocations published. DOMAIN qualification derives
     * live prerequisites only at its genuine post-power/RBBM phase. */
    *out=plan;p->configured=true;p->error="NATIVE_PLATFORM_START_PLAN_PREPARED_ONLY";
    return DRE_GPU_OK;
}
extern "C" int dre_gpu_platform_provider_create(const dre_gpu_platform_provider_config *config,
    dre_gpu_platform_provider **out,dre_gpu_native_provider *table)
{
    if(!out||*out||!table||!configComplete(config))return DRE_GPU_INVALID;
    dre_gpu_platform_provider *p=IONewZero(dre_gpu_platform_provider,1);if(!p)return DRE_GPU_NO_MEMORY;
    p->config=*config;p->error="NATIVE_PLATFORM_EXTERNAL_POLICY_NOT_BOUND";
    if(dre_native_board_snapshot(&p->fdt,&p->fdtBytes)||!equal(
       dre_holi_power_profile_from_fdt(p->fdt,p->fdtBytes,&p->graph),"HOLI_POWER_GRAPH_OBSERVED_POLICY_REQUIRED")) {
        IODelete(p,dre_gpu_platform_provider,1);return DRE_GPU_UNSUPPORTED;
    }
    dre_gpu_native_provider provider={};
    provider.version=DRE_GPU_PROVIDER_VERSION;provider.struct_bytes=sizeof(provider);provider.cookie=p;
    provider.fdt=p->fdt;provider.fdt_bytes=p->fdtBytes;provider.bind_owner_lock=bindLock;
    provider.acquire=acquire;provider.owns=owns;provider.configure=configure;provider.describe=describe;
    /* No aliases certified by a flag: allocator uses actual selected pmap
     * fallback when these callbacks are NULL; absence of that proof refuses.
     * No session/raw submit/fence/cache/persistent/exclusive-VM capability is
     * enabled by this bootstrap provider or by its genuine marker helper. */
    *out=p;*table=provider;
    return DRE_GPU_OK;
}
extern "C" int dre_gpu_platform_provider_arm_fault_irq(dre_gpu_platform_provider *p)
{
    if(!p||!p->configured||!p->startup||owns(p,p->lease)!=1)return DRE_GPU_NOT_READY;
    return dre_gpu_native_owner_arm_irq(p->owner);
}
extern "C" int dre_gpu_platform_provider_retained(dre_gpu_platform_provider *p,
    uint64_t *lease,uint64_t *epoch,const char **reason)
{
    if(!p||!lease||!epoch||!reason||!held(p))return DRE_GPU_INVALID;
    *lease=p->lease;*epoch=p->resetEpoch;*reason=p->error;return DRE_GPU_OK;
}
#endif
