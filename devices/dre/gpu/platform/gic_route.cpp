/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source, authored only. No testing/compilation/runs.
 * Actual native_owner IRQ callbacks over the real dre_spi route implementation.
 */
#include "gic_route.h"
#if defined(DRE)
#include <IOKit/IOLib.h>
#include <IOKit/IOInterruptController.h>
#include <libfdt.h>
#include <string.h>
#include "../../native_board.h"
#include "../../irq/spi_low.h"

#define FAST_ACTIVE UINT32_C(0x40000000)
#define FAST_CLOSING UINT32_C(0x80000000)
#define FAST_REFS UINT32_C(0x3fffffff)
struct dre_gpu_gic_route {
    dre_gpu_native_broker upstream;
    dre_gpu_gic_policy policy;
    const void *fdt;
    size_t fdtBytes;
    uint32_t gicPhandle;
    uint64_t controllerHold,gpuLease,gpuEpoch,sourceOwner;
    dre_spi_route *spi;
    dre_gpu_owner_irq_route bound;
    uint32_t fastGate;
    bool escaped,acquireAttempted,reserveAttempted,released,retiring;
};
static bool outerComplete(const dre_gpu_native_broker *b)
{
    return b&&b->version==DRE_GPU_NATIVE_BROKER_VERSION&&b->struct_bytes==sizeof(*b)&&
        b->enter_owner&&b->leave_owner&&b->owner_lock_held&&b->acquire&&b->check_lease&&
        b->mmio_access&&b->cold_exclusive&&b->held_policy&&b->retired&&b->release;
}
static bool policyComplete(const dre_gpu_gic_policy *p)
{
    return p&&p->version==DRE_GPU_GIC_POLICY_VERSION&&p->struct_bytes==sizeof(*p)&&
        p->controller&&p->hold_controller&&p->check_controller&&p->drop_controller&&
        p->source_owner&&p->check_source&&p->check_source_fast;
}
static int fromSPI(int value)
{
    switch(value) {
    case DRE_SPI_OK:return DRE_GPU_OK;
    case DRE_SPI_ARGUMENT:return DRE_GPU_INVALID;
    case DRE_SPI_BUSY:return DRE_GPU_BUSY;
    case DRE_SPI_MEMORY:return DRE_GPU_NO_MEMORY;
    case DRE_SPI_TIMEOUT:return DRE_GPU_TIMEOUT;
    case DRE_SPI_LOST:return DRE_GPU_STALE;
    case DRE_SPI_QUARANTINED:return DRE_GPU_FAULT;
    default:return DRE_GPU_NOT_READY;
    }
}
static bool held(dre_gpu_gic_route *b)
{ return b&&b->upstream.owner_lock_held(b->upstream.cookie)==0; }
static bool lease(dre_gpu_gic_route *b,uint64_t value,uint64_t epoch)
{ return b&&value&&b->gpuLease==value&&b->gpuEpoch==epoch&&!b->released; }
static int source(dre_gpu_gic_route *b)
{
    if(!held(b)||!b->controllerHold||!b->gpuLease||!b->gpuEpoch||!b->sourceOwner||b->released)
        return DRE_GPU_NOT_READY;
    if(b->policy.check_controller(b->policy.cookie,b->policy.controller,b->controllerHold)||
       b->upstream.check_lease(b->upstream.cookie,b->gpuLease,b->gpuEpoch)||
       b->policy.check_source(b->policy.cookie,b->gpuLease,b->gpuEpoch,
           b->policy.controller,b->controllerHold,b->sourceOwner))return DRE_GPU_NOT_READY;
    return DRE_GPU_OK;
}
static bool sameRoute(const dre_gpu_owner_irq_route *a,const dre_gpu_owner_irq_route *b)
{
    return a&&b&&a->io_service==b->io_service&&a->interrupt_controller==b->interrupt_controller&&
        a->route_lease==b->route_lease&&a->source==b->source&&a->intid==b->intid&&
        a->gic_phandle==b->gic_phandle&&a->reserved==0&&b->reserved==0;
}
static int unique(const void *f,const char *name)
{
    int n=fdt_node_offset_by_compatible(f,-1,name);
    return n>=0&&fdt_node_offset_by_compatible(f,n,name)==-FDT_ERR_NOTFOUND?n:-1;
}
static bool pairs(const void *f,int n,const uint32_t *want,unsigned count)
{
    int bytes,parent=fdt_parent_offset(f,n);
    if(n<0||parent<0||fdt_address_cells(f,parent)!=1||fdt_size_cells(f,parent)!=1)return false;
    const fdt32_t *v=(const fdt32_t *)fdt_getprop(f,n,"reg",&bytes);
    if(!v||bytes!=(int)(count*8))return false;
    for(unsigned i=0;i<count*2;i++)if(fdt32_to_cpu(v[i])!=want[i])return false;
    return true;
}
static bool enabled(const void *f,int n)
{
    for(unsigned depth=0;n>=0&&depth<32;depth++,n=fdt_parent_offset(f,n)) {
        int bytes;const char *s=(const char *)fdt_getprop(f,n,"status",&bytes);
        if(s&&!((bytes==3&&!memcmp(s,"ok",3))||(bytes==5&&!memcmp(s,"okay",5))))return false;
        if(!n)return true;
    }
    return false;
}
static bool originalProfile(dre_gpu_gic_route *b)
{
    if(dre_native_board_snapshot(&b->fdt,&b->fdtBytes)||!b->fdt||b->fdtBytes<40||
       b->fdtBytes>(2u<<20)||fdt_check_full(b->fdt,b->fdtBytes)||
       fdt_totalsize(b->fdt)!=b->fdtBytes||fdt_node_check_compatible(b->fdt,0,"qcom,holi"))return false;
    const void *f=b->fdt;int gpu=unique(f,"qcom,adreno-gpu-a619-holi"),gic=unique(f,"arm,gic-v3"),bytes;
    static const uint32_t gicreg[]={0xf200000,0x10000,0xf240000,0x100000};
    static const uint32_t gpureg[]={0x5900000,0x40000,0x5961000,0x800,0x596a000,0x30000,0x599e000,0x1000};
    static const uint32_t smmureg[]={0x5940000,0x10000},gccreg[]={0x1400000,0x1f0000};
    static const uint32_t gpuccreg[]={0x5990000,0x9000};
    if(gpu<0||gic<0||!enabled(f,gpu)||!enabled(f,gic)||
       !pairs(f,gic,gicreg,2)||!pairs(f,gpu,gpureg,4)||
       !pairs(f,unique(f,"qcom,kgsl-smmu-v2"),smmureg,1)||
       !pairs(f,unique(f,"qcom,holi-gpucc"),gpuccreg,1)||
       !pairs(f,unique(f,"qcom,holi-gcc"),gccreg,1))return false;
    const fdt32_t *v=(const fdt32_t *)fdt_getprop(f,gpu,"qcom,chipid",&bytes);
    if(!v||bytes!=4||fdt32_to_cpu(*v)!=0x06010900)return false;
    v=(const fdt32_t *)fdt_getprop(f,gpu,"interrupts",&bytes);
    if(!v||bytes!=12||fdt32_to_cpu(v[0])||fdt32_to_cpu(v[1])!=177||fdt32_to_cpu(v[2])!=4||
       fdt_getprop(f,gpu,"interrupts-extended",NULL))return false;
    uint32_t parent=0;
    for(int at=gpu,depth=0;at>=0&&depth<32;at=fdt_parent_offset(f,at),depth++) {
        v=(const fdt32_t *)fdt_getprop(f,at,"interrupt-parent",&bytes);
        if(v){if(bytes!=4)return false;parent=fdt32_to_cpu(*v);break;}
    }
    v=(const fdt32_t *)fdt_getprop(f,gic,"#interrupt-cells",&bytes);
    if(!v||bytes!=4||fdt32_to_cpu(*v)!=3||!fdt_getprop(f,gic,"interrupt-controller",&bytes)||bytes||
       !parent||parent==UINT32_MAX||fdt_get_phandle(f,gic)!=parent)return false;
    unsigned matches=0,visited=0;int at=-1;
    while((at=fdt_next_node(f,at,NULL))>=0) {
        if(++visited>8192)return false;
        if(fdt_get_phandle(f,at)==parent)matches++;
    }
    if(at!=-FDT_ERR_NOTFOUND||matches!=1)return false;
    b->gicPhandle=parent;return true;
}
static bool resources(dre_gpu_gic_route *b,const dre_gpu_owner_resources *r)
{
    static const dre_gpu_owner_aperture apertures[]={
        {0x5900000,0x40000},{0x5940000,0x10000},{0x5961000,0x800},
        {0x596a000,0x30000},{0x599e000,0x1000},{0x1400000,0x1f0000}
    };
    if(!r||r->chip_id!=0x06010900||r->gic_phandle!=b->gicPhandle||r->gic_spi!=177||
       r->gic_intid!=209||r->irq_flags!=4||r->aperture_count!=6||
       r->gicd_pa!=0xf200000||r->gicd_bytes!=0x10000||
       r->gicr_pa!=0xf240000||r->gicr_bytes!=0x100000)return false;
    for(unsigned i=0;i<6;i++)if(r->aperture[i].pa!=apertures[i].pa||r->aperture[i].bytes!=apertures[i].bytes)return false;
    return true;
}
static bool fastEnter(dre_gpu_gic_route *b)
{
    uint32_t gate=__atomic_load_n(&b->fastGate,__ATOMIC_ACQUIRE);
    for(unsigned tries=0;tries<32;tries++) {
        if(!(gate&FAST_ACTIVE)||(gate&FAST_CLOSING)||(gate&FAST_REFS)==FAST_REFS)return false;
        uint32_t next=gate+1;
        if(__atomic_compare_exchange_n(&b->fastGate,&gate,next,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return true;
    }
    return false;
}
static void fastLeave(dre_gpu_gic_route *b)
{ __atomic_fetch_sub(&b->fastGate,1,__ATOMIC_RELEASE); }
static int fastCheckHeld(dre_gpu_gic_route *b,uint64_t gpu,uint64_t epoch,uint64_t token)
{
    if(!lease(b,gpu,epoch)||!epoch||!b->controllerHold||!b->sourceOwner||!b->spi||
       !token||token!=b->bound.route_lease||
       b->policy.check_source_fast(b->policy.cookie,gpu,epoch,b->policy.controller,
           b->controllerHold,b->sourceOwner))return DRE_GPU_STALE;
    return fromSPI(dre_spi_route_check_fast(b->spi,token));
}
static int fastCloseJoin(dre_gpu_gic_route *b)
{
    uint32_t gate=__atomic_load_n(&b->fastGate,__ATOMIC_ACQUIRE);
    if(!gate)return DRE_GPU_OK;
    __atomic_fetch_or(&b->fastGate,FAST_CLOSING,__ATOMIC_ACQ_REL);
    uint64_t start=dre_spi_counter();
    for(unsigned poll=0;poll<131072;poll++) {
        if(!(__atomic_load_n(&b->fastGate,__ATOMIC_ACQUIRE)&FAST_REFS))return DRE_GPU_OK;
        if(dre_spi_counter()-start>1920000)break;
    }
    return DRE_GPU_TIMEOUT;
}
/* Caller must already have unregistered its actual handler. This deliberately
 * handles a retained pre-install SPI route whose genuine route token is still
 * zero: outer GPU release cannot silently skip it or fabricate a token. */
static int retireRoute(dre_gpu_gic_route *b)
{
    if(!held(b))return DRE_GPU_NOT_READY;
    if(!b->spi)return DRE_GPU_OK;
    if(!b->gpuLease||b->upstream.retired(b->upstream.cookie,b->gpuLease,b->gpuEpoch))return DRE_GPU_NOT_READY;
    if(!b->retiring&&b->bound.io_service) {
        /* Never mask somebody else's registered handler merely because a
         * caller is trying to retire our metadata. Only an actually empty
         * exclusive route reaches the hardware retirement below. */
        int unregistered=fromSPI(dre_spi_route_check_handler(b->spi,NULL));
        if(unregistered)return unregistered;
        int status=fromSPI(dre_spi_route_mask_join(b->spi));if(status)return status;
    }
    int status=fastCloseJoin(b);if(status)return status;
    b->retiring=true;
    status=fromSPI(dre_spi_route_release(&b->spi));
    if(status)return status; /* every partial held pointer/epoch stays alive */
    __atomic_store_n(&b->fastGate,0,__ATOMIC_RELEASE);b->bound={};
    return DRE_GPU_OK;
}
extern "C" int dre_gpu_gic_route_create(const dre_gpu_native_broker *up,
    const dre_gpu_gic_policy *policy,dre_gpu_gic_route **out)
{
    if(!out||*out||!outerComplete(up)||!policyComplete(policy)||
       up->owner_lock_held(up->cookie))return DRE_GPU_INVALID;
    dre_gpu_gic_route *b=IONewZero(dre_gpu_gic_route,1);if(!b)return DRE_GPU_NO_MEMORY;
    b->upstream=*up;b->policy=*policy;
    if(!originalProfile(b)||dre_spi_counter_frequency()!=19200000) {
        IODelete(b,dre_gpu_gic_route,1);return DRE_GPU_NOT_READY;
    }
    *out=b;
    int status=policy->hold_controller(policy->cookie,policy->controller,&b->controllerHold);
    if(status||!b->controllerHold||policy->check_controller(policy->cookie,policy->controller,b->controllerHold))
        return DRE_GPU_NOT_READY;
    return DRE_GPU_OK;
}
extern "C" int dre_gpu_gic_reserve_irq_route(void *cookie,uint64_t gpu,
    const dre_gpu_owner_resources *r,dre_gpu_owner_irq_route *out)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)cookie;
    if(!b||!out||out->io_service||out->interrupt_controller||out->route_lease||
       out->source||out->intid||out->gic_phandle||out->reserved||!resources(b,r)||
       !lease(b,gpu,b->gpuEpoch)||source(b))return DRE_GPU_NOT_READY;
    if(b->reserveAttempted||b->spi)return DRE_GPU_BUSY;
    b->reserveAttempted=true;
    dre_spi_request q={209,DRE_SPI_LEVEL_HIGH,b->sourceOwner};
    int status=fromSPI(dre_spi_reserve(b->policy.controller,&q,&b->spi));
    if(b->spi) {
        b->bound.route_lease=dre_spi_route_token(b->spi);
        b->bound.intid=209;b->bound.gic_phandle=b->gicPhandle;
        *out=b->bound; /* real partial token may be0; never fabricate one */
    }
    if(status||!b->spi)return status?status:DRE_GPU_FAULT;
    IOService *nub=NULL;IOInterruptController *controller=NULL;unsigned local=0;
    status=fromSPI(dre_spi_route_nub(b->spi,&nub,&local,&controller));
    if(status)return status;
    if(!nub||!controller||local!=0||!b->bound.route_lease)return DRE_GPU_FAULT;
    status=fromSPI(dre_spi_route_check_handler(b->spi,NULL));if(status)return status;
    b->bound.io_service=nub;b->bound.interrupt_controller=controller;b->bound.source=local;
    *out=b->bound;
    __atomic_store_n(&b->fastGate,FAST_ACTIVE,__ATOMIC_RELEASE);
    return DRE_GPU_OK;
}
extern "C" int dre_gpu_gic_check_irq_route(void *cookie,uint64_t gpu,
    const dre_gpu_owner_irq_route *route,const void *target)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)cookie;
    if(!b||!lease(b,gpu,b->gpuEpoch)||source(b)||!b->spi||b->retiring||
       !sameRoute(route,&b->bound)||!route->route_lease||
       dre_spi_route_token(b->spi)!=route->route_lease)return DRE_GPU_STALE;
    IOService *nub=NULL;IOInterruptController *controller=NULL;unsigned local=0;
    int status=fromSPI(dre_spi_route_nub(b->spi,&nub,&local,&controller));if(status)return status;
    if(nub!=route->io_service||controller!=route->interrupt_controller||local!=route->source)
        return DRE_GPU_STALE;
    return fromSPI(dre_spi_route_check_handler(b->spi,target));
}
extern "C" int dre_gpu_gic_irq_lease_fast(void *cookie,uint64_t gpu,uint64_t epoch,uint64_t token)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)cookie;
    if(!b||!fastEnter(b))return DRE_GPU_STALE;
    int status=fastCheckHeld(b,gpu,epoch,token);fastLeave(b);return status;
}
extern "C" int dre_gpu_gic_mask_owned_irq_fast(void *cookie,uint64_t gpu,uint64_t token)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)cookie;
    if(!b||!fastEnter(b))return DRE_GPU_STALE;
    int status=fastCheckHeld(b,gpu,b->gpuEpoch,token);
    if(!status)status=fromSPI(dre_spi_route_mask_fast(b->spi));
    fastLeave(b);return status;
}
extern "C" int dre_gpu_gic_release_irq_route(void *cookie,uint64_t gpu,const dre_gpu_owner_irq_route *route)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)cookie;
    if(!b||!held(b)||!lease(b,gpu,b->gpuEpoch)||!sameRoute(route,&b->bound))return DRE_GPU_STALE;
    return retireRoute(b);
}
/* Full typed forwarding: preserve every original non-IRQ policy cookie and
 * argument. No success stub, synthetic lease, reset or implicit GPU power. */
static void enter(void *p)
{ dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;b->upstream.enter_owner(b->upstream.cookie); }
static void leave(void *p)
{ dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;b->upstream.leave_owner(b->upstream.cookie); }
static int ownerHeld(void *p)
{ dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;return b->upstream.owner_lock_held(b->upstream.cookie); }
static int acquire(void *p,const void *f,size_t bytes,const dre_gpu_owner_resources *r,uint64_t *gpu,uint64_t *epoch)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;
    if(!held(b)||!gpu||!epoch||*gpu||*epoch||b->acquireAttempted||f!=b->fdt||
       bytes!=b->fdtBytes||!resources(b,r)||!b->controllerHold||
       b->policy.check_controller(b->policy.cookie,b->policy.controller,b->controllerHold))return DRE_GPU_NOT_READY;
    b->acquireAttempted=true;
    int status=b->upstream.acquire(b->upstream.cookie,f,bytes,r,gpu,epoch);
    b->gpuLease=*gpu;b->gpuEpoch=*epoch; /* retain even upstream failure */
    if(status)return status;
    if(!*gpu||!*epoch||b->upstream.check_lease(b->upstream.cookie,*gpu,*epoch))return DRE_GPU_NOT_READY;
    status=b->policy.source_owner(b->policy.cookie,*gpu,*epoch,b->policy.controller,
        b->controllerHold,&b->sourceOwner);
    if(status||!b->sourceOwner||source(b))return DRE_GPU_NOT_READY;
    return DRE_GPU_OK;
}
static int checkLease(void *p,uint64_t gpu,uint64_t epoch)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;
    return held(b)&&lease(b,gpu,epoch)?b->upstream.check_lease(b->upstream.cookie,gpu,epoch):DRE_GPU_STALE;
}
static int access(void *p,uint64_t gpu,uint64_t epoch,uint64_t pa,size_t bytes,int write)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;
    return held(b)&&lease(b,gpu,epoch)?b->upstream.mmio_access(b->upstream.cookie,gpu,epoch,pa,bytes,write):DRE_GPU_STALE;
}
static int cold(void *p,uint64_t gpu,uint64_t epoch)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;
    return held(b)&&lease(b,gpu,epoch)?b->upstream.cold_exclusive(b->upstream.cookie,gpu,epoch):DRE_GPU_STALE;
}
static int power(void *p,uint64_t gpu,uint64_t epoch,const dre_rpm_votes *votes,const dre_holi_power_config *config)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;
    return held(b)&&lease(b,gpu,epoch)?b->upstream.held_policy(b->upstream.cookie,gpu,epoch,votes,config):DRE_GPU_STALE;
}
static int retired(void *p,uint64_t gpu,uint64_t epoch)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;
    /* GPU retirement does not imply IRQ handler removal. Native_owner must
     * still mask/join/unregister before the real SPI route can release. */
    return held(b)&&lease(b,gpu,epoch)?b->upstream.retired(b->upstream.cookie,gpu,epoch):DRE_GPU_STALE;
}
static int release(void *p,uint64_t gpu,uint64_t epoch)
{
    dre_gpu_gic_route *b=(dre_gpu_gic_route *)p;
    if(!held(b)||!lease(b,gpu,epoch))return DRE_GPU_STALE;
    int status=retireRoute(b);if(status)return status;
    status=b->upstream.release(b->upstream.cookie,gpu,epoch);
    if(status)return status; /* actual partial failure retains original lease */
    b->gpuLease=0;b->gpuEpoch=0;b->sourceOwner=0;b->released=true;
    return DRE_GPU_OK;
}
extern "C" int dre_gpu_gic_route_broker(dre_gpu_gic_route *b,dre_gpu_native_broker *out)
{
    if(!b||!out||!held(b)||b->escaped||b->acquireAttempted||!b->controllerHold||
       b->policy.check_controller(b->policy.cookie,b->policy.controller,b->controllerHold))return DRE_GPU_NOT_READY;
    dre_gpu_native_broker result={};
    result.version=DRE_GPU_NATIVE_BROKER_VERSION;result.struct_bytes=sizeof(result);result.cookie=b;
    result.enter_owner=enter;result.leave_owner=leave;result.owner_lock_held=ownerHeld;
    result.acquire=acquire;result.check_lease=checkLease;
    result.irq_lease_fast=dre_gpu_gic_irq_lease_fast;result.mask_owned_irq_fast=dre_gpu_gic_mask_owned_irq_fast;
    result.reserve_irq_route=dre_gpu_gic_reserve_irq_route;result.check_irq_route=dre_gpu_gic_check_irq_route;
    result.release_irq_route=dre_gpu_gic_release_irq_route;
    result.mmio_access=access;result.cold_exclusive=cold;result.held_policy=power;
    result.retired=retired;result.release=release;
    b->escaped=true; /* set before publishing callback cookie, even stack copy */
    *out=result;return DRE_GPU_OK;
}
extern "C" int dre_gpu_gic_retained_acquire(dre_gpu_gic_route *b,uint64_t *gpu,uint64_t *epoch)
{
    if(!b||!gpu||!epoch||!held(b))return DRE_GPU_INVALID;
    *gpu=b->gpuLease;*epoch=b->gpuEpoch;
    return DRE_GPU_OK; /* Retained ownership metadata, never a readiness proof. */
}
extern "C" int dre_gpu_gic_route_close(dre_gpu_gic_route **out)
{
    if(!out||!*out)return DRE_GPU_INVALID;
    dre_gpu_gic_route *b=*out;
    if(!held(b)||b->escaped||b->gpuLease||b->spi||b->fastGate)return DRE_GPU_BUSY;
    if(b->controllerHold&&b->policy.drop_controller(b->policy.cookie,b->policy.controller,b->controllerHold))
        return DRE_GPU_NOT_READY;
    IODelete(b,dre_gpu_gic_route,1);*out=NULL;return DRE_GPU_OK;
}
#endif
