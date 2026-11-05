/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source. Real per-SPI controller over selected DRE
 * SLEH/PE hooks. No synthetic controller, global reinit or default broker.
 */
#include "dre_spi.h"
#include "spi_low.h"
#include "platform_namespace.h"
#if defined(DRE)
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libfdt.h>
#include <string.h>
#include "../native_board.h"

class DRENativeGIC;
struct dre_spi_controller { DRENativeGIC *instance; };
struct dre_spi_route {
    DRENativeGIC *owner;
    dre_spi_request request;
    dre_spi_hw original;
    uint64_t lease,epoch,token;
    IOService *nub;
    OSArray *names,*specifiers;
    OSData *specifier;
    uint32_t fault;
    bool installed,attached,started,cacheBound,retiring;
};
class DRENativeGIC : public IOInterruptController {
    OSDeclareDefaultStructors(DRENativeGIC);
public:
    dre_spi_controller api;
    dre_spi_broker broker;
    const void *fdt;
    size_t fdtBytes;
    dre_spi_route *routes[1020];
    const OSSymbol *namespaceName;
    IOPlatformExpert *platform;
    bool attached,started,published,namespaceHeld;
    unsigned routeCount;
    bool initialize(const dre_spi_broker *);
    int held() const;
    int inspect(dre_spi_route *,dre_spi_hw *,bool cold);
    int check(dre_spi_route *);
    int publishOwned();
    int bindNub(dre_spi_route *);
    dre_spi_route *find(IOService *,int);
    IOInterruptVector *routeVector(const dre_spi_route *r) {
        return r&&r->owner==this&&r->request.intid<1020&&
            routes[r->request.intid]==r?&vectors[r->request.intid]:NULL;
    }
    virtual void free() override;
    virtual IOReturn registerInterrupt(IOService *,int,void *,IOInterruptHandler,void *) override;
    virtual IOReturn unregisterInterrupt(IOService *,int) override;
    virtual IOReturn getInterruptType(IOService *,int,int *) override;
    virtual IOReturn enableInterrupt(IOService *,int) override;
    virtual IOReturn disableInterrupt(IOService *,int) override;
    virtual IOReturn causeInterrupt(IOService *,int) override;
    virtual IOInterruptAction getInterruptHandlerAddress() override;
    virtual IOReturn handleInterrupt(void *,IOService *,int) override;
    virtual bool vectorCanBeShared(IOInterruptVectorNumber,IOInterruptVector *) override;
};
OSDefineMetaClassAndStructors(DRENativeGIC,IOInterruptController);

static bool brokerComplete(const dre_spi_broker *b)
{
    return b&&b->version==DRE_SPI_BROKER_VERSION&&b->struct_bytes==sizeof(*b)&&
        b->owner_lock_held&&b->acquire&&b->check&&b->group1ns_authority&&
        b->check_fast&&b->source_quiet&&b->release;
}
static bool enabled(const void *f,int node)
{
    for(unsigned depth=0;node>=0&&depth<32;depth++) {
        int bytes;const char *status=(const char *)fdt_getprop(f,node,"status",&bytes);
        if(status&&!((bytes==3&&!memcmp(status,"ok",3))||
                     (bytes==5&&!memcmp(status,"okay",5))))return false;
        if(!node)return true;
        node=fdt_parent_offset(f,node);
    }
    return false;
}
static int unique(const void *f,const char *compatible)
{
    int n=fdt_node_offset_by_compatible(f,-1,compatible);
    return n>=0&&enabled(f,n)&&
        fdt_node_offset_by_compatible(f,n,compatible)==-FDT_ERR_NOTFOUND?n:-1;
}
static bool pairs(const void *f,int node,const uint32_t *want,unsigned count)
{
    int bytes,parent=fdt_parent_offset(f,node);
    if(node<0||parent<0||fdt_address_cells(f,parent)!=1||fdt_size_cells(f,parent)!=1)return false;
    const fdt32_t *v=(const fdt32_t *)fdt_getprop(f,node,"reg",&bytes);
    if(!v||bytes!=(int)(count*8))return false;
    for(unsigned i=0;i<count*2;i++)if(fdt32_to_cpu(v[i])!=want[i])return false;
    return true;
}
static bool profile(const void *f,size_t bytes)
{
    if(!f||bytes<40||bytes>(2u<<20)||fdt_check_full(f,bytes)||
       fdt_totalsize(f)!=bytes||fdt_node_check_compatible(f,0,"qcom,holi"))return false;
    int gic=unique(f,"arm,gic-v3"),length;
    static const uint32_t reg[]={0xf200000,0x10000,0xf240000,0x100000};
    const fdt32_t *cells=(const fdt32_t *)fdt_getprop(f,gic,"#interrupt-cells",&length);
    if(!cells||length!=4||fdt32_to_cpu(*cells)!=3||!pairs(f,gic,reg,2)||
       !fdt_getprop(f,gic,"interrupt-controller",&length)||length)return false;
    uint32_t phandle=fdt_get_phandle(f,gic);unsigned matches=0,visited=0;int at=-1;
    if(!phandle||phandle==UINT32_MAX)return false;
    while((at=fdt_next_node(f,at,NULL))>=0) {
        if(++visited>8192)return false;
        if(fdt_get_phandle(f,at)==phandle)matches++;
    }
    return at==-FDT_ERR_NOTFOUND&&matches==1;
}
/* Only the two grounded original-FDT sources are admitted initially. A caller
 * cannot turn any arbitrary SPI or a wake-controller index into a route. */
static bool sourceProfile(const void *f,const dre_spi_request *q)
{
    if(!q||!q->source_owner||q->trigger!=DRE_SPI_LEVEL_HIGH)return false;
    int node=-1;
    if(q->intid==209)node=unique(f,"qcom,adreno-gpu-a619-holi");
    else if(q->intid==259)node=fdt_path_offset(f,"/soc/pinctrl@400000");
    else return false;
    int bytes;const fdt32_t *irq=(const fdt32_t *)fdt_getprop(f,node,"interrupts",&bytes);
    if(node<0||!enabled(f,node)||!irq||bytes!=12||fdt32_to_cpu(irq[0])||
       fdt32_to_cpu(irq[1])!=q->intid-32||fdt32_to_cpu(irq[2])!=q->trigger||
       fdt_getprop(f,node,"interrupts-extended",NULL))return false;
    uint32_t phandle=0;
    for(unsigned depth=0;node>=0&&depth<32;depth++,node=fdt_parent_offset(f,node)) {
        const fdt32_t *p=(const fdt32_t *)fdt_getprop(f,node,"interrupt-parent",&bytes);
        if(p){if(bytes!=4)return false;phandle=fdt32_to_cpu(*p);break;}
    }
    int gic=unique(f,"arm,gic-v3");
    return phandle&&gic>=0&&fdt_get_phandle(f,gic)==phandle;
}
bool DRENativeGIC::initialize(const dre_spi_broker *b)
{
    api.instance=this;broker={};fdt=NULL;fdtBytes=0;namespaceName=NULL;platform=NULL;
    attached=started=published=namespaceHeld=false;routeCount=0;
    bzero(routes,sizeof(routes));vectors=NULL;controllerLock=NULL;
    if(!brokerComplete(b)||b->owner_lock_held(b->cookie)||!IOInterruptController::init())return false;
    broker=*b;
    if(dre_native_board_snapshot(&fdt,&fdtBytes)||!profile(fdt,fdtBytes)||
       dre_spi_counter_frequency()!=19200000)return false;
    namespaceName=OSSymbol::withCString("DREOwnedHoliGICv3");
    platform=IOService::getPlatform();if(platform)platform->retain();
    vectors=IONewZero(IOInterruptVector,1020);
    return namespaceName&&platform&&vectors;
}
int DRENativeGIC::held() const
{ return broker.owner_lock_held&&broker.owner_lock_held(broker.cookie)==0?0:DRE_SPI_POLICY; }
int DRENativeGIC::inspect(dre_spi_route *r,dre_spi_hw *hw,bool cold)
{
    int status=dre_spi_pe_read(r->request.intid,hw);if(status)return status;
    if(hw->configuration!=0||hw->irouter&(UINT64_C(1)<<31)||hw->groupmod||
       (cold&&(hw->enabled||hw->pending||hw->active)))return DRE_SPI_BUSY;
    /* Fixed affinity and priority are inherited unchanged. Their actual CPU,
     * PMR/BPR/security policy must be acquired externally; no global writes.
     * In single-security-state mode IGROUPR must be visibly group1. In the
     * NS RAZ view only genuine external group authority can qualify it. */
    if((hw->ctlr&(1u<<6))&&!hw->group)return DRE_SPI_POLICY;
    return DRE_SPI_OK;
}
int DRENativeGIC::check(dre_spi_route *r)
{
    if(held()||!r||r->owner!=this||r->request.intid>=1020||routes[r->request.intid]!=r||
       !r->lease||!r->epoch||__atomic_load_n(&r->fault,__ATOMIC_ACQUIRE))return DRE_SPI_LOST;
    dre_spi_hw hw;int status=inspect(r,&hw,false);if(status)return status;
    if(hw.irouter!=r->original.irouter||hw.priority!=r->original.priority||
       hw.configuration!=r->original.configuration||hw.group!=r->original.group||
       hw.groupmod!=r->original.groupmod||
       (hw.ctlr&0x53)!=(r->original.ctlr&0x53)||
       broker.check(broker.cookie,r->lease,r->epoch,&hw)||
       broker.group1ns_authority(broker.cookie,r->lease,r->epoch,&hw))return DRE_SPI_POLICY;
    return DRE_SPI_OK;
}
static int fastRoute(void *cookie,uint32_t intid)
{
    dre_spi_route *r=(dre_spi_route *)cookie;
    if(!r||!r->owner||r->request.intid!=intid||!r->lease||!r->epoch)return DRE_SPI_LOST;
    int status=r->owner->broker.check_fast(r->owner->broker.cookie,r->lease,r->epoch,intid);
    if(status)__atomic_store_n(&r->fault,1,__ATOMIC_RELEASE);
    return status?DRE_SPI_LOST:DRE_SPI_OK;
}
static void dispatchRoute(void *cookie,uint32_t intid)
{
    dre_spi_route *r=(dre_spi_route *)cookie;
    r->owner->handleInterrupt(r,r->nub,(int)intid);
}
int DRENativeGIC::publishOwned()
{
    if(namespaceHeld)return published?DRE_SPI_OK:DRE_SPI_QUARANTINED;
    if(!routeCount||held()||!platform||platform!=IOService::getPlatform())return DRE_SPI_POLICY;
    if(!attached){if(!attach(platform))return DRE_SPI_PLATFORM;attached=true;}
    if(!started){if(!IOInterruptController::start(platform))return DRE_SPI_PLATFORM;started=true;}
    /* Actual registry publication is reached only after a live SPI lease and
     * SLEH route exist. Atomic namespace registration never replaces anyone. */
    if(dre_irq_namespace_register_absent(namespaceName,this)!=kIOReturnSuccess)return DRE_SPI_NAMESPACE;
    namespaceHeld=true;
    registerService();published=true;
    return DRE_SPI_OK;
}
int DRENativeGIC::bindNub(dre_spi_route *r)
{
    int32_t words[2]={(int32_t)r->request.intid,0};
    r->nub=new IOService;r->names=OSArray::withCapacity(1);
    r->specifiers=OSArray::withCapacity(1);r->specifier=OSData::withBytes(words,sizeof(words));
    if(!r->nub||!r->nub->init()||!r->names||!r->specifiers||!r->specifier||
       !r->names->setObject(namespaceName)||!r->specifiers->setObject(r->specifier))return DRE_SPI_MEMORY;
    r->nub->setName("DREOwnedSPI");
    if(!r->nub->setProperty(gIOInterruptControllersKey,r->names)||
       !r->nub->setProperty(gIOInterruptSpecifiersKey,r->specifiers))return DRE_SPI_MEMORY;
    /* false allocates IOService's real arrays but never resolves an absent
     * controller. The real dictionary lock binds only this empty source. */
    IOReturn allocation=dre_irq_source_cache_allocate(r->nub,0);
    if(allocation!=kIOReturnSuccess||r->nub->_numInterruptSources!=1||
       !r->nub->_interruptSources||dre_irq_namespace_bind_cached_source(namespaceName,
         this,r->nub,0,r->specifier)!=kIOReturnSuccess)return DRE_SPI_NAMESPACE;
    r->cacheBound=true;
    if(!r->nub->attach(this))return DRE_SPI_PLATFORM;r->attached=true;
    if(!r->nub->start(this))return DRE_SPI_PLATFORM;r->started=true;
    /* Private real nub: do not start IOKit matching/arbitrary clients. The
     * retained owner receives this physically routed, attached source directly.
     * Only the real controller participates in the namespace/registry. */
    return DRE_SPI_OK;
}
dre_spi_route *DRENativeGIC::find(IOService *nub,int source)
{
    if(!nub||source!=0||nub->_numInterruptSources!=1||!nub->_interruptSources||
       nub->_interruptSources[0].interruptController!=this)return NULL;
    OSData *data=nub->_interruptSources[0].vectorData;
    if(!data||data->getLength()!=8||!data->getBytesNoCopy())return NULL;
    int32_t id;memcpy(&id,data->getBytesNoCopy(),4);
    if(id<32||id>=1020)return NULL;
    dre_spi_route *r=routes[id];
    int32_t options;memcpy(&options,(const uint8_t *)data->getBytesNoCopy()+4,4);
    if(!r||options||r->nub!=nub||r->specifier!=data||!r->cacheBound||!r->installed||
       !r->names||r->names->getCount()!=1||r->names->getObject(0)!=namespaceName||
       !r->specifiers||r->specifiers->getCount()!=1||r->specifiers->getObject(0)!=data)return NULL;
    /* IRQ callers use immutable, already-resolved retained cache objects only.
     * No IORegistry property lookup or namespace mutex in primary context. */
    if(!platform->atInterruptLevel()&&
       (nub->getProperty(gIOInterruptControllersKey)!=r->names||
        nub->getProperty(gIOInterruptSpecifiersKey)!=r->specifiers))return NULL;
    return r;
}
IOReturn DRENativeGIC::registerInterrupt(IOService *nub,int source,void *target,
                                       IOInterruptHandler handler,void *refCon)
{
    dre_spi_route *r=find(nub,source);
    if(!r||!target||!handler||r->retiring||check(r))return kIOReturnNotReady;
    /* Do not enter IODT's global shared-interrupt conversion path. This actual
     * controller owns one exclusive vector and rejects every second handler
     * atomically; no sharing hint can broaden its acquired external lease. */
    IOInterruptVector *v=&vectors[r->request.intid];
    IOLockLock(v->interruptLock);
    if(v->interruptRegistered){IOLockUnlock(v->interruptLock);return kIOReturnExclusiveAccess;}
    int status=dre_spi_route_mask_join(r);
    if(status){IOLockUnlock(v->interruptLock);return kIOReturnNotReady;}
    v->nub=nub;v->source=0;v->target=target;v->handler=handler;v->refCon=refCon;
    __atomic_store_n(&v->interruptDisabledSoft,(char)1,__ATOMIC_RELEASE);
    __atomic_store_n(&v->interruptDisabledHard,(char)1,__ATOMIC_RELEASE);
    __atomic_store_n(&v->interruptRegistered,(char)1,__ATOMIC_RELEASE);
    IOLockUnlock(v->interruptLock);return kIOReturnSuccess;
}
IOReturn DRENativeGIC::unregisterInterrupt(IOService *nub,int source)
{
    dre_spi_route *r=find(nub,source);
    if(!r||held()||dre_spi_route_mask_join(r))return kIOReturnNotReady;
    IOInterruptVector *v=&vectors[r->request.intid];IOLockLock(v->interruptLock);
    v->handler=NULL;v->target=NULL;v->refCon=NULL;v->nub=NULL;v->source=0;
    __atomic_store_n(&v->interruptRegistered,(char)0,__ATOMIC_RELEASE);
    IOLockUnlock(v->interruptLock);return kIOReturnSuccess;
}
IOReturn DRENativeGIC::getInterruptType(IOService *nub,int source,int *type)
{
    dre_spi_route *r=find(nub,source);
    if(!type||!r)return kIOReturnBadArgument;
    if(check(r))return kIOReturnNotReady;
    *type=kIOInterruptTypeLevel;return kIOReturnSuccess;
}
IOReturn DRENativeGIC::enableInterrupt(IOService *nub,int source)
{ dre_spi_route *r=find(nub,source);return r&&!dre_spi_route_unmask(r)?kIOReturnSuccess:kIOReturnNotReady; }
IOReturn DRENativeGIC::disableInterrupt(IOService *nub,int source)
{
    dre_spi_route *r=find(nub,source);if(!r)return kIOReturnNotReady;
    if(platform->atInterruptLevel()) {
        int status=dre_spi_route_mask_fast(r);
        IOInterruptVector *v=&vectors[r->request.intid];
        __atomic_store_n(&v->interruptDisabledSoft,(char)1,__ATOMIC_RELEASE);
        __atomic_store_n(&v->interruptDisabledHard,(char)1,__ATOMIC_RELEASE);
        return status?kIOReturnNotReady:kIOReturnSuccess;
    }
    return !dre_spi_route_mask_join(r)?kIOReturnSuccess:kIOReturnNotReady;
}
IOReturn DRENativeGIC::causeInterrupt(IOService *,int)
{ return kIOReturnUnsupported; } /* no synthetic pending/SGI injection */
IOInterruptAction DRENativeGIC::getInterruptHandlerAddress()
{ return OSMemberFunctionCast(IOInterruptAction,this,&DRENativeGIC::handleInterrupt); }
bool DRENativeGIC::vectorCanBeShared(IOInterruptVectorNumber,IOInterruptVector *)
{ return false; }
IOReturn DRENativeGIC::handleInterrupt(void *refCon,IOService *,int source)
{
    dre_spi_route *r=(dre_spi_route *)refCon;
    if(!r||r->owner!=this||source!=(int)r->request.intid||fastRoute(r,(uint32_t)source))
        return kIOReturnNotReady;
    IOInterruptVector *v=&vectors[source];
    __atomic_store_n(&v->interruptActive,(char)1,__ATOMIC_RELEASE);
    __asm__ volatile("dsb sy" ::: "memory");
    if(!__atomic_load_n(&v->interruptDisabledSoft,__ATOMIC_ACQUIRE)&&
       __atomic_load_n(&v->interruptRegistered,__ATOMIC_ACQUIRE)&&v->handler) {
        v->handler(v->target,v->refCon,v->nub,0);
    } else {
        __atomic_store_n(&v->interruptDisabledHard,(char)1,__ATOMIC_RELEASE);
        if(dre_spi_route_mask_fast(r))__atomic_store_n(&r->fault,1,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&v->interruptActive,(char)0,__ATOMIC_RELEASE);
    return kIOReturnSuccess; /* Existing SLEH owns ACK/EOI; do neither here. */
}
void DRENativeGIC::free()
{
    /* Only close/failed initialize with no retained route may release us. */
    if(vectors){IODelete(vectors,IOInterruptVector,1020);vectors=NULL;}
    OSSafeReleaseNULL(namespaceName);OSSafeReleaseNULL(platform);
    IOInterruptController::free();
}

extern "C" int dre_spi_controller_create(const dre_spi_broker *b,dre_spi_controller **out)
{
    if(!out||*out||!brokerComplete(b))return DRE_SPI_ARGUMENT;
    DRENativeGIC *ic=new DRENativeGIC;
    if(!ic)return DRE_SPI_MEMORY;
    if(!ic->initialize(b)){ic->release();return DRE_SPI_PLATFORM;}
    *out=&ic->api;return DRE_SPI_OK;
}
extern "C" int dre_spi_reserve(dre_spi_controller *c,const dre_spi_request *q,dre_spi_route **out)
{
    if(!c||!c->instance||!q||!out||*out)return DRE_SPI_ARGUMENT;
    DRENativeGIC *ic=c->instance;
    if(ic->held()||!sourceProfile(ic->fdt,q)||ic->routes[q->intid])return DRE_SPI_POLICY;
    dre_spi_route *r=IONewZero(dre_spi_route,1);if(!r)return DRE_SPI_MEMORY;
    r->owner=ic;r->request=*q;
    int status=ic->inspect(r,&r->original,true);
    if(status){IODelete(r,dre_spi_route,1);return status;}
    /* Hold output/slot before any external acquisition; a failed callback
     * which retains real resources can never hide them from recovery. */
    ic->retain();ic->routes[q->intid]=r;ic->routeCount++;*out=r;
    status=ic->broker.acquire(ic->broker.cookie,ic->fdt,ic->fdtBytes,q,&r->original,&r->lease,&r->epoch);
    if(status||!r->lease||!r->epoch)return DRE_SPI_POLICY;
    if(ic->check(r)||ic->broker.source_quiet(ic->broker.cookie,r->lease,r->epoch,q->intid))return DRE_SPI_POLICY;
    /* Re-observe after actual acquisition; no enabled/pending/active inherited
     * route is silently disabled or stolen for our initial registration. */
    dre_spi_hw current;if(ic->inspect(r,&current,true))return DRE_SPI_BUSY;
    IOInterruptVector *v=ic->routeVector(r);if(!v)return DRE_SPI_LOST;
    if(v->interruptLock||v->interruptRegistered)return DRE_SPI_BUSY;
    v->interruptLock=IOLockAlloc();if(!v->interruptLock)return DRE_SPI_MEMORY;
    status=dre_spi_summary_install(q->intid,r,dispatchRoute,fastRoute,&r->token);
    if(status)return status;r->installed=true;
    status=ic->publishOwned();if(status)return status;
    return ic->bindNub(r);
}
extern "C" uint64_t dre_spi_route_token(const dre_spi_route *r)
{ return r?r->token:0; }
extern "C" int dre_spi_route_check_fast(dre_spi_route *r,uint64_t token)
{
    return r&&r->installed&&r->token==token&&token?
        dre_spi_summary_live(r->request.intid,token):DRE_SPI_LOST;
}
extern "C" int dre_spi_route_check_handler(dre_spi_route *r,const void *target)
{
    if(!r||r->owner->check(r)||r->retiring||!r->installed||
       dre_spi_summary_live(r->request.intid,r->token)||
       !r->nub||r->owner->find(r->nub,0)!=r)return DRE_SPI_LOST;
    IOInterruptVector *v=r->owner->routeVector(r);if(!v)return DRE_SPI_LOST;
    if(!target)return !v->interruptRegistered?DRE_SPI_OK:DRE_SPI_BUSY;
    return v->interruptRegistered&&v->target==target&&v->handler&&
           v->nub==r->nub&&v->source==0&&!v->sharedController?DRE_SPI_OK:DRE_SPI_LOST;
}
extern "C" int dre_spi_route_nub(dre_spi_route *r,IOService **nub,unsigned *source,IOInterruptController **controller)
{
    if(nub)*nub=NULL;if(source)*source=0;if(controller)*controller=NULL;
    if(!r||!nub||!source||!controller||r->owner->check(r)||r->retiring||
       !r->owner->namespaceHeld||!r->owner->published||!r->cacheBound||
       r->owner->find(r->nub,0)!=r)return DRE_SPI_LOST;
    IOInterruptController *actual=dre_irq_namespace_copy_nowait(r->owner->namespaceName);
    bool same=actual==r->owner;OSSafeReleaseNULL(actual);
    if(!same||!(r->owner->getState()&kIOServiceRegisteredState))return DRE_SPI_NAMESPACE;
    *nub=r->nub;*controller=r->owner;return DRE_SPI_OK;
}
extern "C" int dre_spi_route_mask_fast(dre_spi_route *r)
{
    if(!r||!r->installed||fastRoute(r,r->request.intid))return DRE_SPI_LOST;
    int status=dre_spi_pe_enable(r->request.intid,r->token,0);
    if(status)__atomic_store_n(&r->fault,1,__ATOMIC_RELEASE);
    return status;
}
extern "C" int dre_spi_route_mask_join(dre_spi_route *r)
{
    if(!r||r->owner->held()||IOService::getPlatform()->atInterruptLevel())return DRE_SPI_POLICY;
    int status=dre_spi_route_mask_fast(r);if(status)return status;
    IOInterruptVector *v=r->owner->routeVector(r);if(!v)return DRE_SPI_LOST;
    __atomic_store_n(&v->interruptDisabledSoft,(char)1,__ATOMIC_RELEASE);
    __atomic_store_n(&v->interruptDisabledHard,(char)1,__ATOMIC_RELEASE);
    uint64_t start=dre_spi_counter();
    for(unsigned poll=0;poll<131072;poll++) {
        dre_spi_hw hw;status=dre_spi_pe_read(r->request.intid,&hw);if(status)return status;
        if(!hw.enabled&&!hw.active&&
           !__atomic_load_n(&v->interruptActive,__ATOMIC_ACQUIRE)&&
           dre_spi_summary_inflight(r->request.intid,r->token)==0)return DRE_SPI_OK;
        if(dre_spi_counter()-start>1920000)break; /* 100ms, no scheduler wait */
    }
    __atomic_store_n(&r->fault,1,__ATOMIC_RELEASE);return DRE_SPI_TIMEOUT;
}
extern "C" int dre_spi_route_unmask(dre_spi_route *r)
{
    if(!r||r->owner->check(r)||r->retiring||!r->installed)return DRE_SPI_LOST;
    IOInterruptVector *v=r->owner->routeVector(r);if(!v)return DRE_SPI_LOST;
    if(!v->interruptRegistered||!v->handler)return DRE_SPI_POLICY;
    __atomic_store_n(&v->interruptDisabledSoft,(char)0,__ATOMIC_RELEASE);
    __atomic_store_n(&v->interruptDisabledHard,(char)0,__ATOMIC_RELEASE);
    int status=dre_spi_pe_enable(r->request.intid,r->token,1);
    if(status) {
        __atomic_store_n(&v->interruptDisabledSoft,(char)1,__ATOMIC_RELEASE);
        __atomic_store_n(&r->fault,1,__ATOMIC_RELEASE);
    }
    return status;
}
extern "C" int dre_spi_route_release(dre_spi_route **value)
{
    if(!value||!*value)return DRE_SPI_ARGUMENT;
    dre_spi_route *r=*value;DRENativeGIC *ic=r->owner;
    if(ic->held()||!r->lease||!r->epoch)return DRE_SPI_QUARANTINED;
    IOInterruptVector *v=ic->routeVector(r);if(!v)return DRE_SPI_LOST;
    if(v->interruptRegistered||ic->broker.source_quiet(ic->broker.cookie,r->lease,r->epoch,r->request.intid))return DRE_SPI_BUSY;
    dre_spi_hw held;
    if(ic->inspect(r,&held,false)||held.irouter!=r->original.irouter||
       held.priority!=r->original.priority||held.group!=r->original.group||
       held.groupmod!=r->original.groupmod||
       ic->broker.check(ic->broker.cookie,r->lease,r->epoch,&held)||
       ic->broker.group1ns_authority(ic->broker.cookie,r->lease,r->epoch,&held))
        return DRE_SPI_QUARANTINED;
    if(!r->installed&&(held.enabled||held.active||held.pending))return DRE_SPI_BUSY;
    if(r->installed) {
        if(!r->retiring) {
            int status=dre_spi_route_mask_join(r);if(status)return status;
        }
        dre_spi_hw hw;if(ic->inspect(r,&hw,false)||hw.enabled||hw.active||hw.pending)return DRE_SPI_BUSY;
        if(!r->retiring) {
            if(dre_spi_summary_close(r->request.intid,r->token))return DRE_SPI_LOST;
            r->retiring=true;
        }
        if(dre_spi_summary_inflight(r->request.intid,r->token)||
           dre_spi_summary_remove(r->request.intid,r->token))return DRE_SPI_BUSY;
        r->installed=false;
    }
    /* External release is last while all original references still exist.
     * A failed/partial release retains the route and mappings for recovery. */
    if(ic->broker.release(ic->broker.cookie,r->lease,r->epoch))return DRE_SPI_QUARANTINED;
    if(r->nub) {
        if(r->cacheBound&&r->nub->_interruptSources) {
            r->nub->_interruptSources[0].interruptController=NULL;
            r->nub->_interruptSources[0].vectorData=NULL;
        }
        if(r->started)r->nub->stop(ic);
        if(r->attached)r->nub->detach(ic);
    }
    OSSafeReleaseNULL(r->nub);OSSafeReleaseNULL(r->names);
    OSSafeReleaseNULL(r->specifiers);OSSafeReleaseNULL(r->specifier);
    if(v->interruptLock){IOLockFree(v->interruptLock);v->interruptLock=NULL;}
    ic->routes[r->request.intid]=NULL;ic->routeCount--;IODelete(r,dre_spi_route,1);
    *value=NULL;ic->release();return DRE_SPI_OK;
}
extern "C" int dre_spi_controller_close(dre_spi_controller **value)
{
    if(!value||!*value||!(*value)->instance)return DRE_SPI_ARGUMENT;
    DRENativeGIC *ic=(*value)->instance;
    if(ic->held()||ic->routeCount)return DRE_SPI_BUSY;
    /* IOService registration can escape references to matching/notification
     * consumers. No general terminate/client-join provider is supplied by this
     * slice, so an actually published controller remains boot-resident. */
    if(ic->published)return DRE_SPI_QUARANTINED;
    if(ic->namespaceHeld) {
        if(dre_irq_namespace_unregister_same(ic->namespaceName,ic)!=kIOReturnSuccess)return DRE_SPI_NAMESPACE;
        ic->namespaceHeld=false;
    }
    if(ic->started)ic->IOInterruptController::stop(ic->platform);
    if(ic->attached)ic->detach(ic->platform);
    *value=NULL;ic->release();return DRE_SPI_OK;
}
#endif
