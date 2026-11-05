/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight source.
 * Real IOKit route/work-loop binding around an externally acquired owner.
 * No synthetic broker, reset, GIC global hook replacement or rail policy. */
#include "native_owner.h"
#include "../../native_board.h"
#include "../../libfdt/libfdt.h"
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOEventSource.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <kern/locks.h>

class DREGPUNativeOwner;
class DREGPUOwnerWork : public IOEventSource {
    OSDeclareDefaultStructors(DREGPUOwnerWork);
public:
    DREGPUNativeOwner *adapter;
    uint32_t pending;
    bool initOwner(DREGPUNativeOwner *);
    void post();
    void discard();
    bool checkForWork() APPLE_KEXT_OVERRIDE;
};
class DREGPUNativeOwner : public IOService {
    OSDeclareDefaultStructors(DREGPUNativeOwner);
public:
    dre_gpu_native_broker broker;
    dre_gpu_owner_resources resources;
    dre_gpu_owner_irq_route route;
    dre_gpu_native_io raw;
    const void *fdt;
    size_t fdtBytes;
    uint64_t lease,resetEpoch;
    IOService *nub;
    IOInterruptController *controller;
    OSSymbol *controllerName;
    OSData *specifier;
    OSArray *controllerList,*specifierList;
    uint8_t specifierBytes[64];
    uint32_t specifierLength,sourceCount;
    IOWorkLoop *loop;
    DREGPUOwnerWork *work;
    dre_a619_startup *startup;
    uint32_t fault,closing,irqCount,faultReason;
    int32_t fastMaskStatus;
    bool held,registered,registrationAttempted,armed,ioBound,workAttached;
    bool initOwner(const dre_gpu_native_broker *);
    void free() APPLE_KEXT_OVERRIDE;
    bool owns();
    bool routeMatches(bool registeredOwner);
    int acquire();
    void invalidate(dre_gpu_native_owner_fault);
    void runWorker();
    static void interrupt(OSObject *,void *,IOService *,int);
    static IOReturn joinWork(OSObject *,void *,void *,void *,void *);
    static int read32(void *,uint64_t,uint32_t *);
    static int write32(void *,uint64_t,uint32_t);
    static int read64(void *,uint64_t,uint64_t *);
    static int write64(void *,uint64_t,uint64_t);
    static void barrier(void *);
    static uint64_t now(void *);
    bool access(uint64_t,size_t,bool);
};
OSDefineMetaClassAndStructors(DREGPUNativeOwner,IOService);
OSDefineMetaClassAndStructors(DREGPUOwnerWork,IOEventSource);
LCK_GRP_DECLARE(dre_gpu_native_owner_group,"DRE native GPU owner");
LCK_MTX_DECLARE(dre_gpu_native_owner_ledger,&dre_gpu_native_owner_group);
static DREGPUNativeOwner *boardOwner;

bool DREGPUOwnerWork::initOwner(DREGPUNativeOwner *o)
{ adapter=o;pending=0;return o&&IOEventSource::init(o); }
void DREGPUOwnerWork::post()
{ __atomic_store_n(&pending,1,__ATOMIC_RELEASE);signalWorkAvailable(); }
void DREGPUOwnerWork::discard()
{ disable();__atomic_store_n(&pending,0,__ATOMIC_RELEASE); }
bool DREGPUOwnerWork::checkForWork()
{
    if(isEnabled()&&__atomic_exchange_n(&pending,0,__ATOMIC_ACQ_REL))adapter->runWorker();
    return isEnabled()&&__atomic_load_n(&pending,__ATOMIC_ACQUIRE);
}
static bool callbacks(const dre_gpu_native_broker *b)
{
    return b&&b->version==DRE_GPU_NATIVE_BROKER_VERSION&&b->struct_bytes==sizeof(*b)&&
        b->enter_owner&&b->leave_owner&&b->owner_lock_held&&b->acquire&&b->check_lease&&
        b->irq_lease_fast&&b->mask_owned_irq_fast&&b->reserve_irq_route&&b->check_irq_route&&
        b->release_irq_route&&b->mmio_access&&b->cold_exclusive&&b->held_policy&&b->retired&&b->release;
}
static int unique(const void *dt,const char *name)
{
    int n=fdt_node_offset_by_compatible(dt,-1,name);
    return n>=0&&fdt_node_offset_by_compatible(dt,n,name)==-FDT_ERR_NOTFOUND?n:-1;
}
static bool pairs(const void *dt,int n,const uint64_t *expected,unsigned count)
{
    int length,parent=fdt_parent_offset(dt,n);
    if(n<0||parent<0||fdt_address_cells(dt,parent)!=1||fdt_size_cells(dt,parent)!=1)return false;
    const fdt32_t *p=(const fdt32_t *)fdt_getprop(dt,n,"reg",&length);
    if(!p||length!=(int)(count*8))return false;
    for(unsigned i=0;i<count*2;i++)if(fdt32_to_cpu(p[i])!=expected[i])return false;
    return true;
}
static bool profile(const void *dt,size_t bytes,dre_gpu_owner_resources *out)
{
    if(!dt||bytes<40||bytes>(2u<<20)||fdt_check_full(dt,bytes)||fdt_totalsize(dt)!=bytes||
       fdt_node_check_compatible(dt,0,"qcom,holi"))return false;
    int gpu=unique(dt,"qcom,adreno-gpu-a619-holi"),length;
    const fdt32_t *chip=(const fdt32_t *)fdt_getprop(dt,gpu,"qcom,chipid",&length);
    if(!chip||length!=4||fdt32_to_cpu(*chip)!=DRE_A619_CHIP_ID||
       fdt_getprop(dt,gpu,"interrupts-extended",NULL))return false;
    const fdt32_t *interrupt=(const fdt32_t *)fdt_getprop(dt,gpu,"interrupts",&length);
    if(!interrupt||length!=12||fdt32_to_cpu(interrupt[0])||
       fdt32_to_cpu(interrupt[1])!=177||fdt32_to_cpu(interrupt[2])!=4)return false;
    uint32_t phandle=0;
    for(int n=gpu;n>=0;n=fdt_parent_offset(dt,n)) {
        const fdt32_t *p=(const fdt32_t *)fdt_getprop(dt,n,"interrupt-parent",&length);
        if(p){if(length!=4)return false;phandle=fdt32_to_cpu(*p);break;}
    }
    int gic=fdt_node_offset_by_phandle(dt,phandle);
    if(gic<0||gic!=unique(dt,"arm,gic-v3")||
       !fdt_getprop(dt,gic,"interrupt-controller",&length)||length)return false;
    unsigned phandleMatches=0;
    for(int n=fdt_next_node(dt,-1,NULL);n>=0;n=fdt_next_node(dt,n,NULL))
        if(fdt_get_phandle(dt,n)==phandle)phandleMatches++;
    if(phandleMatches!=1)return false;
    const fdt32_t *cells=(const fdt32_t *)fdt_getprop(dt,gic,"#interrupt-cells",&length);
    if(!cells||length!=4||fdt32_to_cpu(*cells)!=3)return false;
    static const uint64_t gp[]={0x5900000,0x40000,0x5961000,0x800,0x596a000,0x30000,0x599e000,0x1000};
    static const uint64_t sm[]={0x5940000,0x10000},cc[]={0x5990000,0x9000},gc[]={0x1400000,0x1f0000};
    static const uint64_t gi[]={0xf200000,0x10000,0xf240000,0x100000};
    if(!pairs(dt,gpu,gp,4)||!pairs(dt,unique(dt,"qcom,kgsl-smmu-v2"),sm,1)||
       !pairs(dt,unique(dt,"qcom,holi-gpucc"),cc,1)||!pairs(dt,unique(dt,"qcom,holi-gcc"),gc,1)||
       !pairs(dt,gic,gi,2))return false;
    bzero(out,sizeof(*out));out->chip_id=DRE_A619_CHIP_ID;out->gic_phandle=phandle;
    out->gic_spi=177;out->gic_intid=209;out->irq_flags=4;out->aperture_count=6;
    out->aperture[0]={0x5900000,0x40000};out->aperture[1]={0x5940000,0x10000};
    out->aperture[2]={0x5961000,0x800};out->aperture[3]={0x596a000,0x30000};
    /* Wrapper ends at0x0599a000 and fully encloses GPUCC0x05990000+0x9000.
     * One Device map/guard admits that existing overlap; no seventh alias. */
    out->aperture[4]={0x599e000,0x1000};out->aperture[5]={0x1400000,0x1f0000};
    out->gicd_pa=gi[0];out->gicd_bytes=gi[1];out->gicr_pa=gi[2];out->gicr_bytes=gi[3];
    return !strcmp(dre_holi_power_profile_from_fdt(dt,bytes,&out->power),
        "HOLI_POWER_GRAPH_OBSERVED_POLICY_REQUIRED");
}
bool DREGPUNativeOwner::initOwner(const dre_gpu_native_broker *b)
{
    broker={};resources={};route={};raw={};fdt=NULL;fdtBytes=0;lease=resetEpoch=0;
    nub=NULL;controller=NULL;controllerName=NULL;specifier=NULL;
    controllerList=specifierList=NULL;specifierLength=sourceCount=0;
    loop=NULL;work=NULL;startup=NULL;fault=closing=irqCount=faultReason=0;fastMaskStatus=0;
    held=registered=registrationAttempted=armed=ioBound=workAttached=false;
    if(!callbacks(b)||!IOService::init())return false;
    broker=*b;
    if(dre_native_board_snapshot(&fdt,&fdtBytes)||!profile(fdt,fdtBytes,&resources))return false;
    uint64_t hz;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
    if(hz!=19200000)return false;
    loop=IOWorkLoop::workLoop();work=new DREGPUOwnerWork;
    if(!loop||!work||!work->initOwner(this)||loop->addEventSource(work)!=kIOReturnSuccess)return false;
    workAttached=true;return true;
}
bool DREGPUNativeOwner::routeMatches(bool afterRegister)
{
    if(!nub||!controller||nub->isInactive()||controller->isInactive()||!controllerName||!specifier||
       route.source>255||route.intid!=209||route.gic_phandle!=resources.gic_phandle||
       !route.route_lease||route.reserved)return false;
    OSObject *names=nub->copyProperty(gIOInterruptControllersKey);
    OSObject *vectors=nub->copyProperty(gIOInterruptSpecifiersKey);
    bool same=names==controllerList&&vectors==specifierList;
    OSSafeReleaseNULL(names);OSSafeReleaseNULL(vectors);
    if(!same||!controllerList||!specifierList||!sourceCount||sourceCount>256||
       controllerList->getCount()!=sourceCount||specifierList->getCount()!=sourceCount||
       route.source>=sourceCount||controllerList->getObject(route.source)!=controllerName||
       specifierList->getObject(route.source)!=specifier||
       specifier->getLength()!=specifierLength||memcmp(specifier->getBytesNoCopy(),specifierBytes,specifierLength)||
       nub->_numInterruptSources!=(int)sourceCount||!nub->_interruptSources||
       nub->_interruptSources[route.source].interruptController!=controller||
       nub->_interruptSources[route.source].vectorData!=specifier||
       !(controller->getState()&kIOServiceRegisteredState)||
       OSDynamicCast(IOSharedInterruptController,controller)||!controller->getInterruptHandlerAddress())return false;
    /* lookUpInterruptController waits indefinitely on a missing name in this
     * selected XNU. The actual publisher must already have resolved this nub;
     * inspect only that concrete cache and known controller, never name-probe. */
    int type=0;IOOptionBits options=0;
    if(controller->getInterruptType(nub,(int)route.source,&type)!=kIOReturnSuccess||type!=kIOInterruptTypeLevel||
       IODTGetInterruptOptions(nub,route.source,&options)!=kIOReturnSuccess||
       (options&kIODTInterruptShared))return false;
    return broker.check_irq_route(broker.cookie,lease,&route,afterRegister?this:NULL)==0;
}
bool DREGPUNativeOwner::owns()
{
    return lease&&resetEpoch&&!__atomic_load_n(&fault,__ATOMIC_ACQUIRE)&&
        !__atomic_load_n(&closing,__ATOMIC_ACQUIRE)&&broker.owner_lock_held(broker.cookie)==0&&
        broker.check_lease(broker.cookie,lease,resetEpoch)==0&&registered&&routeMatches(true);
}
int DREGPUNativeOwner::acquire()
{
    if(broker.owner_lock_held(broker.cookie)!=0)return DRE_GPU_INVALID;
    int result=broker.acquire(broker.cookie,fdt,fdtBytes,&resources,&lease,&resetEpoch);
    if(lease&&!held){retain();held=true;}
    if(result||!lease||!resetEpoch||broker.check_lease(broker.cookie,lease,resetEpoch))
        return result?result:DRE_GPU_NOT_READY;
    result=broker.reserve_irq_route(broker.cookie,lease,&resources,&route);
    if(result)return result;
    IOService *leasedNub=OSDynamicCast(IOService,(OSObject *)route.io_service);
    IOInterruptController *leasedController=OSDynamicCast(IOInterruptController,(OSObject *)route.interrupt_controller);
    if(!leasedNub||!leasedController)return DRE_GPU_NOT_READY;
    nub=leasedNub;controller=leasedController;
    nub->retain();controller->retain();
    OSObject *names=nub->copyProperty(gIOInterruptControllersKey);
    OSObject *vectors=nub->copyProperty(gIOInterruptSpecifiersKey);
    controllerList=OSDynamicCast(OSArray,names);specifierList=OSDynamicCast(OSArray,vectors);
    if(!controllerList)OSSafeReleaseNULL(names);
    if(!specifierList)OSSafeReleaseNULL(vectors);
    if(!controllerList||!specifierList||route.source>255)return DRE_GPU_NOT_READY;
    sourceCount=controllerList->getCount();
    if(!sourceCount||sourceCount>256||specifierList->getCount()!=sourceCount||route.source>=sourceCount)
        return DRE_GPU_NOT_READY;
    OSSymbol *leasedName=OSDynamicCast(OSSymbol,controllerList->getObject(route.source));
    OSData *leasedSpecifier=OSDynamicCast(OSData,specifierList->getObject(route.source));
    if(!leasedName||!leasedSpecifier||leasedSpecifier->getLength()<4||leasedSpecifier->getLength()>64)
        return DRE_GPU_NOT_READY;
    controllerName=leasedName;specifier=leasedSpecifier;
    controllerName->retain();specifier->retain();specifierLength=specifier->getLength();
    bcopy(specifier->getBytesNoCopy(),specifierBytes,specifierLength);
    if(!routeMatches(false))return DRE_GPU_NOT_READY;
    /* Actual nonsharing/exclusive reservation precedes any call that could
     * otherwise disable or share another installed consumer's source. */
    registrationAttempted=true;
    if(controller->registerInterrupt(nub,(int)route.source,this,(IOInterruptHandler)interrupt,this)!=kIOReturnSuccess) {
        __atomic_store_n(&fault,1,__ATOMIC_RELEASE);return DRE_GPU_FAULT;
    }
    registered=true;
    if(!routeMatches(true)||broker.mask_owned_irq_fast(broker.cookie,lease,route.route_lease)||
       controller->disableInterrupt(nub,(int)route.source)!=kIOReturnSuccess)
        return DRE_GPU_FAULT;
    return DRE_GPU_OK;
}
void DREGPUNativeOwner::interrupt(OSObject *target,void *,IOService *provider,int source)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)target;
    if(!s)return;
    __atomic_fetch_add(&s->irqCount,1,__ATOMIC_RELAXED);
    uint32_t first=0;
    __atomic_compare_exchange_n(&s->faultReason,&first,DRE_GPU_OWNER_IRQ,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    __atomic_store_n(&s->fault,1,__ATOMIC_RELEASE); /* block new MMIO/submission immediately */
    int masked=DRE_GPU_FAULT;
    if(provider==s->nub&&source==(int)s->route.source) {
        int check=s->broker.irq_lease_fast(s->broker.cookie,s->lease,s->resetEpoch,s->route.route_lease);
        masked=check?check:s->broker.mask_owned_irq_fast(s->broker.cookie,s->lease,s->route.route_lease);
    }
    __atomic_store_n(&s->fastMaskStatus,masked,__ATOMIC_RELEASE);
    /* No GPU, rail or GIC register access here. Real controller owns dispatch,
     * acknowledgement/EOI and its generation-aware mask operation above. */
    if(!__atomic_load_n(&s->closing,__ATOMIC_ACQUIRE))s->work->post();
}
void DREGPUNativeOwner::invalidate(dre_gpu_native_owner_fault reason)
{
    __atomic_store_n(&fault,1,__ATOMIC_RELEASE);
    uint32_t first=0;
    __atomic_compare_exchange_n(&faultReason,&first,(uint32_t)reason,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    reason=(dre_gpu_native_owner_fault)__atomic_load_n(&faultReason,__ATOMIC_ACQUIRE);
    const char *message=reason==DRE_GPU_OWNER_IRQ?"NATIVE_GPU_IRQ_FAULT_RETAINED":
        reason==DRE_GPU_OWNER_RESET_CHANGED?"NATIVE_GPU_RESET_EPOCH_CHANGED":
        reason==DRE_GPU_OWNER_CLOCK_LOST?"NATIVE_GPU_CLOCK_OWNERSHIP_LOST":"NATIVE_GPU_LEASE_LOST";
    if(startup)dre_a619_startup_invalidate(startup,message);
}
void DREGPUNativeOwner::runWorker()
{
    broker.enter_owner(broker.cookie);
    /* A GPU route event is unexpected in this polling profile. It always
     * invalidates startup; never read/ack RBBM registers while power is
     * unknown or clear a fault then manufacture a successful completion. */
    invalidate(DRE_GPU_OWNER_IRQ);armed=false;
    broker.leave_owner(broker.cookie);
}
bool DREGPUNativeOwner::access(uint64_t pa,size_t bytes,bool write)
{
    if(!ioBound||!bytes||(pa&(bytes-1))||!owns())return false;
    bool range=false;
    for(unsigned i=0;i<resources.aperture_count;i++) {
        const dre_gpu_owner_aperture *a=&resources.aperture[i];
        if(pa>=a->pa&&pa-a->pa<=a->bytes&&bytes<=a->bytes-(pa-a->pa))range=true;
    }
    return range&&broker.mmio_access(broker.cookie,lease,resetEpoch,pa,bytes,write)==0;
}
int DREGPUNativeOwner::read32(void *cookie,uint64_t pa,uint32_t *value)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)cookie;
    if(!value||!s->access(pa,4,false))return 0;
    int result=s->raw.read32(s->raw.cookie,pa,value);
    if(result!=1)s->invalidate(DRE_GPU_OWNER_LEASE_LOST);return result==1;
}
int DREGPUNativeOwner::write32(void *cookie,uint64_t pa,uint32_t value)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)cookie;
    if(!s->access(pa,4,true))return 0;
    int result=s->raw.write32(s->raw.cookie,pa,value);
    if(result!=1)s->invalidate(DRE_GPU_OWNER_LEASE_LOST);return result==1;
}
int DREGPUNativeOwner::read64(void *cookie,uint64_t pa,uint64_t *value)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)cookie;
    if(!value||!s->access(pa,8,false))return 0;
    int result=s->raw.read64(s->raw.cookie,pa,value);
    if(result!=1)s->invalidate(DRE_GPU_OWNER_LEASE_LOST);return result==1;
}
int DREGPUNativeOwner::write64(void *cookie,uint64_t pa,uint64_t value)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)cookie;
    if(!s->access(pa,8,true))return 0;
    int result=s->raw.write64(s->raw.cookie,pa,value);
    if(result!=1)s->invalidate(DRE_GPU_OWNER_LEASE_LOST);return result==1;
}
void DREGPUNativeOwner::barrier(void *cookie)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)cookie;
    __asm__ volatile("dsb sy":::"memory");if(s->ioBound)s->raw.barrier(s->raw.cookie);
}
uint64_t DREGPUNativeOwner::now(void *cookie)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)cookie;
    return s->ioBound?s->raw.now(s->raw.cookie):0;
}
IOReturn DREGPUNativeOwner::joinWork(OSObject *target,void *,void *,void *,void *)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)target;
    /* Discard inside the actual loop gate, after any running worker. A mere
     * empty barrier followed by an outside disable could race new dispatch. */
    s->work->discard();return kIOReturnSuccess;
}
void DREGPUNativeOwner::free()
{
    /* A real acquired lease owns an extra resident reference until close's
     * actual retirement succeeds. Ordinary IOService destruction cannot drop it. */
    if(!lease) {
        if(loop&&work&&workAttached)loop->removeEventSource(work);
        OSSafeReleaseNULL(work);OSSafeReleaseNULL(loop);
        OSSafeReleaseNULL(controllerName);OSSafeReleaseNULL(specifier);
        OSSafeReleaseNULL(controllerList);OSSafeReleaseNULL(specifierList);
        OSSafeReleaseNULL(controller);OSSafeReleaseNULL(nub);
    }
    IOService::free();
}
static DREGPUNativeOwner *owner_cast(dre_gpu_native_owner *opaque)
{ return (DREGPUNativeOwner *)opaque; }
extern "C" int dre_gpu_native_owner_create(const dre_gpu_native_broker *broker,dre_gpu_native_owner **out)
{
    if(!out)return DRE_GPU_INVALID;*out=NULL;
    if(!callbacks(broker)||broker->owner_lock_held(broker->cookie)!=0)return DRE_GPU_UNSUPPORTED;
    DREGPUNativeOwner *s=new DREGPUNativeOwner;
    if(!s)return DRE_GPU_NO_MEMORY;
    if(!s->initOwner(broker)){s->release();return DRE_GPU_NOT_READY;}
    lck_mtx_lock(&dre_gpu_native_owner_ledger);
    if(boardOwner){lck_mtx_unlock(&dre_gpu_native_owner_ledger);s->release();return DRE_GPU_BUSY;}
    boardOwner=s;lck_mtx_unlock(&dre_gpu_native_owner_ledger);
    int result=s->acquire();
    if(!s->lease) {
        lck_mtx_lock(&dre_gpu_native_owner_ledger);if(boardOwner==s)boardOwner=NULL;
        lck_mtx_unlock(&dre_gpu_native_owner_ledger);s->release();return result;
    }
    *out=(dre_gpu_native_owner *)s;return result;
}
extern "C" int dre_gpu_native_owner_bind_io(dre_gpu_native_owner *opaque,const dre_gpu_native_io *io)
{
    DREGPUNativeOwner *s=owner_cast(opaque);
    if(!s||!io||!s->owns()||s->ioBound||io->cookie==s||!io->read32||!io->write32||
       !io->read64||!io->write64||!io->barrier||!io->now)return DRE_GPU_NOT_READY;
    s->raw=*io;s->ioBound=true;return DRE_GPU_OK;
}
extern "C" int dre_gpu_native_owner_bind_startup(dre_gpu_native_owner *opaque,dre_a619_startup *startup)
{
    DREGPUNativeOwner *s=owner_cast(opaque);
    if(!s||!startup||!s->owns()||s->startup)return DRE_GPU_NOT_READY;
    s->startup=startup;return DRE_GPU_OK;
}
extern "C" int dre_gpu_native_owner_arm_irq(dre_gpu_native_owner *opaque)
{
    DREGPUNativeOwner *s=owner_cast(opaque);
    if(!s||!s->owns()||!s->startup||!s->ioBound||s->armed||
       strcmp(dre_a619_startup_check_live(s->startup),"NATIVE_START_HELD"))return DRE_GPU_NOT_READY;
    if(s->controller->enableInterrupt(s->nub,(int)s->route.source)!=kIOReturnSuccess) {
        s->invalidate(DRE_GPU_OWNER_LEASE_LOST);
        (void)s->broker.mask_owned_irq_fast(s->broker.cookie,s->lease,s->route.route_lease);
        return DRE_GPU_FAULT;
    }
    s->armed=true;return DRE_GPU_OK;
}
extern "C" uint64_t dre_gpu_native_owner_lease(dre_gpu_native_owner *opaque)
{ DREGPUNativeOwner *s=owner_cast(opaque);return s&&s->owns()?s->lease:0; }
extern "C" int dre_gpu_native_owner_io(dre_gpu_native_owner *opaque,dre_gpu_native_io *out)
{
    DREGPUNativeOwner *s=owner_cast(opaque);
    if(!s||!out||!s->owns()||!s->ioBound)return DRE_GPU_NOT_READY;
    *out={s,DREGPUNativeOwner::read32,DREGPUNativeOwner::write32,DREGPUNativeOwner::read64,
        DREGPUNativeOwner::write64,DREGPUNativeOwner::barrier,DREGPUNativeOwner::now};return DRE_GPU_OK;
}
extern "C" int dre_gpu_native_owner_guard(void *opaque)
{ DREGPUNativeOwner *s=(DREGPUNativeOwner *)opaque;return s&&s->owns(); }
extern "C" int dre_gpu_native_owner_cold(void *opaque,uint64_t lease)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)opaque;
    return s&&s->owns()&&s->lease==lease&&s->broker.cold_exclusive(s->broker.cookie,lease,s->resetEpoch)==0;
}
extern "C" int dre_gpu_native_owner_policy(void *opaque,uint64_t lease,const dre_rpm_votes *votes,
    const dre_holi_power_config *config)
{
    DREGPUNativeOwner *s=(DREGPUNativeOwner *)opaque;
    return s&&s->owns()&&s->lease==lease&&votes&&config&&votes->link.lease==lease&&
        s->broker.held_policy(s->broker.cookie,lease,s->resetEpoch,votes,config)==0;
}
extern "C" void dre_gpu_native_owner_invalidate(dre_gpu_native_owner *opaque,dre_gpu_native_owner_fault reason)
{
    DREGPUNativeOwner *s=owner_cast(opaque);if(!s)return;
    __atomic_store_n(&s->fault,1,__ATOMIC_RELEASE);
    uint32_t first=0;
    __atomic_compare_exchange_n(&s->faultReason,&first,(uint32_t)reason,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    if(s->broker.owner_lock_held(s->broker.cookie)==0)s->invalidate(reason);
    else s->work->post();
}
extern "C" int dre_gpu_native_owner_close(dre_gpu_native_owner **opaque)
{
    if(!opaque||!*opaque)return DRE_GPU_INVALID;
    DREGPUNativeOwner *s=owner_cast(*opaque);
    if(s->broker.owner_lock_held(s->broker.cookie)==0||s->loop->onThread())return DRE_GPU_BUSY;
    s->broker.enter_owner(s->broker.cookie);
    if(!s->lease||s->broker.retired(s->broker.cookie,s->lease,s->resetEpoch)) {
        s->broker.leave_owner(s->broker.cookie);return DRE_GPU_NOT_READY;
    }
    if(s->registrationAttempted) {
        if(s->broker.check_irq_route(s->broker.cookie,s->lease,&s->route,s)==0)s->registered=true;
        else if(s->broker.check_irq_route(s->broker.cookie,s->lease,&s->route,NULL)==0)s->registered=false;
        else {s->broker.leave_owner(s->broker.cookie);return DRE_GPU_NOT_READY;}
    }
    if(s->registered&&(s->broker.irq_lease_fast(s->broker.cookie,s->lease,s->resetEpoch,s->route.route_lease)||
        s->broker.mask_owned_irq_fast(s->broker.cookie,s->lease,s->route.route_lease))) {
        s->broker.leave_owner(s->broker.cookie);return DRE_GPU_NOT_READY;
    }
    __atomic_store_n(&s->closing,1,__ATOMIC_RELEASE);s->invalidate(DRE_GPU_OWNER_LEASE_LOST);
    /* Actual SPI disable joins only primary ISR/GIC ACTIVE and needs the same
     * board owner mutex. The primary handler never acquires that mutex. */
    if(s->registered&&s->controller->disableInterrupt(s->nub,(int)s->route.source)!=kIOReturnSuccess) {
        s->broker.leave_owner(s->broker.cookie);return DRE_GPU_FAULT;
    }
    s->broker.leave_owner(s->broker.cookie);
    /* No owner mutex while joining a worker that may need that same mutex. */
    if(s->loop->runAction(DREGPUNativeOwner::joinWork,s)!=kIOReturnSuccess)return DRE_GPU_FAULT;
    s->broker.enter_owner(s->broker.cookie);
    int result=s->broker.retired(s->broker.cookie,s->lease,s->resetEpoch);
    if(!result&&s->registered) {
        if(s->broker.check_irq_route(s->broker.cookie,s->lease,&s->route,s)||
           s->controller->unregisterInterrupt(s->nub,(int)s->route.source)!=kIOReturnSuccess)result=DRE_GPU_FAULT;
        else s->registered=false;
    }
    if(!result&&s->route.route_lease) {
        result=s->broker.release_irq_route(s->broker.cookie,s->lease,&s->route);
        if(!result)s->route.route_lease=0;
    }
    if(!result)result=s->broker.release(s->broker.cookie,s->lease,s->resetEpoch);
    if(!result)s->lease=0;
    s->broker.leave_owner(s->broker.cookie);
    if(result)return result;
    lck_mtx_lock(&dre_gpu_native_owner_ledger);if(boardOwner==s)boardOwner=NULL;
    lck_mtx_unlock(&dre_gpu_native_owner_ledger);
    if(s->held){s->held=false;s->release();}
    s->release();*opaque=NULL;return DRE_GPU_OK;
}
