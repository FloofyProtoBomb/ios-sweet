/* SPDX-License-Identifier: MIT -- real XNU serialized SCM64/PAS13 adapter.
 * Never auto-created/published. No default platform/security/PIL policy.
 */
#include "scm_native.h"
#if defined(DRE)
#include "../../native_board.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/libkern.h>

struct dre_gpu_scm_native {
    dre_gpu_owner_lock owner;
    dre_gpu_scm_request_policy policy;
    dre_gpu_fw_native *firmware;
    dre_gpu_fw_relocation_view view;
    dre_gpu_scm_profile profile;
    dre_gpu_scm_channel channel;
    const void *fdt;
    size_t fdtBytes;
    uint64_t commonLease,requestLease,transaction;
    bool sourceHeld,attempted,acquired,quarantined,released,escaped;
    bool retirementObserved,policyReleased;
};
/* Actual module-wide serialization. Unknown secure continuations reserve the
 * logical owner/ticket permanently until a separately proven reset recovery.
 * No ordinary IOLock is held on return from ANY function. */
static IOLock *scmMutex;
static dre_gpu_scm_native *logicalOwner;
static uint64_t nextTransaction;
static bool logicalQuarantined;
static IOLock *mutex()
{
    IOLock *p=__atomic_load_n(&scmMutex,__ATOMIC_ACQUIRE);
    if(p)return p;
    IOLock *created=IOLockAlloc();if(!created)return NULL;
    p=NULL;
    if(!__atomic_compare_exchange_n(&scmMutex,&p,created,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        IOLockFree(created);
    return __atomic_load_n(&scmMutex,__ATOMIC_ACQUIRE);
}
static bool ownerMutexHeld(dre_gpu_scm_native *c)
{
    return c&&c->owner.check_held&&
        c->owner.check_held(c->owner.cookie)==DRE_GPU_OK&&
        IOService::getPlatform()&&!IOService::getPlatform()->atInterruptLevel();
}
static bool ownerHeld(dre_gpu_scm_native *c)
{return ownerMutexHeld(c)&&!c->released;}
static int guard(void *cookie)
{
    auto *c=(dre_gpu_scm_native *)cookie;
    if(!ownerHeld(c)||c->quarantined||!c->acquired||!c->requestLease||!c->sourceHeld)return 0;
    const void *fdt=NULL;size_t bytes=0;
    return !dre_native_board_snapshot(&fdt,&bytes)&&fdt==c->fdt&&bytes==c->fdtBytes&&
        c->profile.clock_mask==0&&c->policy.check(c->policy.cookie,c->commonLease,c->requestLease,&c->view)==1;
}
static int reserve(void *cookie)
{
    auto *c=(dre_gpu_scm_native *)cookie;
    if(guard(c)!=1)return 0;
    IOLockLock(scmMutex);
    bool ok=!logicalOwner&&!logicalQuarantined&&!c->transaction&&nextTransaction!=UINT64_MAX;
    if(ok){c->transaction=++nextTransaction;logicalOwner=c;}
    IOLockUnlock(scmMutex);return ok?1:0;
}
static int reservationHeld(void *cookie)
{
    auto *c=(dre_gpu_scm_native *)cookie;
    if(!ownerHeld(c))return 0;
    IOLockLock(scmMutex);
    bool ok=logicalOwner==c&&c->transaction&&!logicalQuarantined&&!c->quarantined;
    IOLockUnlock(scmMutex);return ok?1:0;
}
static int finish(void *cookie,int uncertain)
{
    auto *c=(dre_gpu_scm_native *)cookie;
    /* Also records uncertainty after an ownership check was lost. Only our
     * actual registry slot may be changed; never clear a foreign owner. */
    IOLockLock(scmMutex);
    bool ours=logicalOwner==c&&c->transaction;
    if(uncertain)c->quarantined=true;
    if(ours) {
        if(uncertain)logicalQuarantined=true;
        else if(!logicalQuarantined&&!c->quarantined){logicalOwner=NULL;c->transaction=0;}
        else ours=false;
    }
    IOLockUnlock(scmMutex);return ours?1:0;
}
static int directSMC(void *cookie,dre_a619_smc_regs *r)
{
    auto *c=(dre_gpu_scm_native *)cookie;
    if(!r||guard(c)!=1)return 0;
    IOLockLock(scmMutex);
    bool held=logicalOwner==c&&c->transaction&&!logicalQuarantined&&!c->quarantined;
    int result=held?dre_a619_smc64_arm64(NULL,r):0;
    IOLockUnlock(scmMutex);return result==1?1:0;
}
static void barrier(void *){__asm__ volatile("dsb sy":::"memory");}
static uint64_t now(void *)
{uint64_t n;__asm__ volatile("isb; mrs %0,cntpct_el0":"=r"(n)::"memory");return n;}
static int discovered(void *p)
{return dre_gpu_scm_channel_discovered(&((dre_gpu_scm_native *)p)->channel);}
static int clocks(void *p,uint64_t lease)
{
    auto *c=(dre_gpu_scm_native *)p;
    /* This exact original profile has NO required named SCM clocks. Actual
     * PIL bandwidth/interconnect holds are separately required by policy. */
    return lease==c->commonLease&&c->profile.clock_mask==0&&guard(c)==1;
}
static bool sameLoan(const dre_gpu_scm_native *c,const dre_gpu_fw_region *original,
    const dre_gpu_fw_relocation *p,uint64_t image,uint64_t bytes)
{
    const auto *a=&c->view.original_identity;const auto *b=&c->view.plan;
    return original&&p&&original->pa==a->pa&&original->bytes==a->bytes&&
        original->cpu_granule_pa==a->cpu_granule_pa&&original->cpu_granule_bytes==a->cpu_granule_bytes&&
        original->phandle==a->phandle&&original->limitations==a->limitations&&
        image==c->view.image_pa&&bytes==c->view.image_bytes&&
        p->load_pa==b->load_pa&&p->load_bytes==b->load_bytes&&p->metadata_pa==b->metadata_pa&&
        p->metadata_bytes==b->metadata_bytes&&p->zap.memory_bytes==b->zap.memory_bytes&&
        p->zap.alignment==b->zap.alignment&&p->entry_offset==b->entry_offset&&!p->reserved;
}
static int requestOwned(void *p,uint64_t lease,const dre_gpu_fw_region *original,
    const dre_gpu_fw_relocation *loan,uint64_t image,uint64_t bytes)
{
    auto *c=(dre_gpu_scm_native *)p;
    return lease==c->commonLease&&sameLoan(c,original,loan,image,bytes)&&guard(c)==1;
}
static int retired(void *p,uint64_t lease)
{
    auto *c=(dre_gpu_scm_native *)p;
    /* After actual request release, no live request proof exists to query.
     * This completed private ledger is needed for the final firmware object's
     * release callback; all facts were established before releasing policy,
     * and this escaped channel can never submit or reacquire again. */
    if(ownerMutexHeld(c)&&lease==c->commonLease&&c->released&&c->retirementObserved&&
       c->policyReleased&&!c->sourceHeld&&!c->quarantined&&!c->requestLease&&
       !c->transaction&&!c->channel.uncertain&&!c->channel.inflight&&
       c->channel.phase==DRE_SCM_PAS_OFF)return 1;
    return lease==c->commonLease&&guard(c)==1&&dre_gpu_scm_channel_retirable(&c->channel)==1&&
        c->policy.consumers_retired(c->policy.cookie,c->commonLease,c->requestLease)==1;
}
extern "C" int dre_gpu_scm_native_create(const dre_gpu_owner_lock *owner,uint64_t lease,
    dre_gpu_fw_native *fw,const dre_gpu_scm_request_policy *policy,dre_gpu_scm_native **out)
{
    if(!owner||!lease||!fw||!policy||!out||*out||owner->version!=DRE_GPU_OWNER_LOCK_VERSION||
       owner->struct_bytes!=sizeof(*owner)||!owner->check_held||!owner->enter||!owner->leave||
       owner->check_held(owner->cookie)!=DRE_GPU_OK||
       policy->version!=DRE_GPU_SCM_REQUEST_POLICY_VERSION||policy->struct_bytes!=sizeof(*policy)||
       !policy->acquire||!policy->check||!policy->consumers_retired||!policy->release||
       !IOService::getPlatform()||IOService::getPlatform()->atInterruptLevel())return DRE_GPU_INVALID;
    uint64_t el,hz;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));__asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
    if(el!=4||hz!=19200000||page_size!=16384||!mutex())return DRE_GPU_NOT_READY;
    auto *c=(dre_gpu_scm_native *)IOMallocZero(sizeof(dre_gpu_scm_native));
    if(!c)return DRE_GPU_NO_MEMORY;
    c->owner=*owner;c->commonLease=lease;c->policy=*policy;c->firmware=fw;
    if(dre_native_board_snapshot(&c->fdt,&c->fdtBytes)||
       strcmp(dre_gpu_scm_profile_from_fdt(c->fdt,c->fdtBytes,&c->profile),"SCM_EMPTY_NAMED_CLOCK_PROFILE_ONLY")||
       dre_gpu_fw_native_relocation_snapshot(fw,&c->view)!=1||dre_gpu_fw_native_source_hold(fw)!=1) {
        IOFree(c,sizeof(*c));return DRE_GPU_NOT_READY;
    }
    c->sourceHeld=true;*out=c;return DRE_GPU_OK;
}
extern "C" const char *dre_gpu_scm_native_acquire(dre_gpu_scm_native *c)
{
    if(!ownerHeld(c)||c->attempted)return "SCM_NATIVE_REQUEST_NOT_FRESH_OR_OWNED";
    c->attempted=true;
    int status=c->policy.acquire(c->policy.cookie,c->commonLease,&c->view,&c->requestLease);
    if(status||!c->requestLease)return "SCM_NATIVE_REAL_PAS_REQUEST_POLICY_REQUIRED_RETAINED";
    c->acquired=true;
    dre_gpu_scm_channel_ops ops={c,guard,reserve,reservationHeld,finish,directSMC,barrier,now};
    if(dre_gpu_scm_channel_init(&c->channel,&ops,&c->view.plan)!=1)return "SCM_NATIVE_REQUEST_BINDING_REFUSED_RETAINED";
    return dre_gpu_scm_channel_discover(&c->channel);
}
extern "C" int dre_gpu_scm_native_firmware_ops(dre_gpu_scm_native *c,dre_gpu_fw_native_ops *out)
{
    if(!out||!ownerHeld(c)||dre_gpu_scm_channel_discovered(&c->channel)!=1)return DRE_GPU_NOT_READY;
    dre_gpu_fw_native_ops result={};result.cookie=c;result.lease=c->commonLease;
    result.scm64_discovered=discovered;result.scm_clocks_held=clocks;
    result.consumers_retired=retired;result.owns_relocated_pas13=requestOwned;
    if(dre_gpu_scm_channel_pas_ops(&c->channel,&result.scm)!=1)return DRE_GPU_NOT_READY;
    c->escaped=true;*out=result;return DRE_GPU_OK;
}
extern "C" const char *dre_gpu_scm_native_release(dre_gpu_scm_native *c)
{
    if(!ownerHeld(c)||!c->acquired||c->quarantined||
       dre_gpu_scm_channel_retirable(&c->channel)!=1||retired(c,c->commonLease)!=1)
        return "SCM_NATIVE_REQUEST_OR_FIRMWARE_NOT_RETIRED";
    IOLockLock(scmMutex);bool clear=logicalOwner!=c&&!c->transaction;IOLockUnlock(scmMutex);
    if(!clear)return "SCM_NATIVE_POLICY_RELEASE_UNCERTAIN_RETAINED";
    c->retirementObserved=true; /* actual retired() above succeeded */
    if(c->policy.release(c->policy.cookie,c->commonLease,c->requestLease)!=DRE_GPU_OK) {
        c->quarantined=true;
        return "SCM_NATIVE_POLICY_RELEASE_UNCERTAIN_RETAINED";
    }
    c->policyReleased=true;c->acquired=false;c->requestLease=0;
    if(!c->sourceHeld||dre_gpu_fw_native_source_drop(c->firmware)!=1)
        return "SCM_NATIVE_SOURCE_HOLD_UNCERTAIN_RETAINED";
    c->sourceHeld=false;c->released=true;
    /* Escaped callback cookies and private ledger remain boot resident. */
    return "SCM_NATIVE_REQUEST_RETIRED_OBJECT_RESIDENT";
}
#endif
