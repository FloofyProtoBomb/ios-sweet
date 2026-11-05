/* SPDX-License-Identifier: MIT
 * UNVALIDATED source-only native GPIO22 controller. No default policy exists.
 * Actual TLMM pin/MPM topology from Holi98a38783 and boot-held original FDT.
 * Actual SPI delivery comes from the separately owned irq/dre_spi provider.
 */
#include "xnu_tlmm.h"
#if defined(DRE)
#include "../irq/platform_namespace.h"
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <machine/machine_routines.h>
#include <vm/memory_types.h>
#include <mach/mach_time.h>
#include <vm/pmap.h>
extern "C" {
#include "../native_board.h"
vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned);
void ml_io_unmap(vm_offset_t,vm_size_t);
}

class DRETouchTLMMController : public IOInterruptController {
    OSDeclareDefaultStructors(DRETouchTLMMController);
public:
    dre_touch_tlmm_policy policy;
    dre_tn_tlmm_profile profile;
    dre_tn_tlmm_pin pin;
    IOService *brokerOwner,*child,*parentNub;
    IOInterruptController *parentController;
    const OSSymbol *name;
    dre_spi_route *parentRoute;
    void *pinLease;
    uint64_t sourceOwner,parentToken;
    vm_offset_t pinMap;
    IOLock *lifecycle;
    OSObject *childTarget;
    IOInterruptHandler childHandler;
    void *childRef;
    uint32_t childInFlight,closing,quarantined;
    uint64_t realPinIRQs,emptyParentIRQs;
    unsigned parentSource;
    bool childRegistered,childEnabled,parentAttempted,parentRegistered,namespaceRegistered;
    bool hardwareHold,started,attached,childStarted,publishedEver;

    bool initNative(const dre_touch_tlmm_policy *);
    int openNative();
    int closeNative();
    int enterOwner(int *);
    void leaveOwner(int);
    bool owned();
    bool fastOwned();
    bool stopping();
    bool exact(IOService *,int);
    int maskJoin(); /* Caller holds actual board owner and lifecycle locks. */
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn registerInterrupt(IOService *,int,void *,IOInterruptHandler,void *) APPLE_KEXT_OVERRIDE;
    IOReturn unregisterInterrupt(IOService *,int) APPLE_KEXT_OVERRIDE;
    IOReturn getInterruptType(IOService *,int,int *) APPLE_KEXT_OVERRIDE;
    IOReturn enableInterrupt(IOService *,int) APPLE_KEXT_OVERRIDE;
    IOReturn disableInterrupt(IOService *,int) APPLE_KEXT_OVERRIDE;
    IOReturn causeInterrupt(IOService *,int) APPLE_KEXT_OVERRIDE;
    IOInterruptAction getInterruptHandlerAddress() APPLE_KEXT_OVERRIDE;
    IOReturn handleInterrupt(void *,IOService *,int) APPLE_KEXT_OVERRIDE;
    static void parentAction(OSObject *,void *,IOService *,int);
    static int pinGuard(void *);
    static uintptr_t pinLock(void *);
    static void pinUnlock(void *,uintptr_t);
    static int pinRead(void *,unsigned,uint32_t *);
    static int pinWrite(void *,unsigned,uint32_t);
    static void pinFinish(void *);
};
OSDefineMetaClassAndStructors(DRETouchTLMMController,IOInterruptController);

static bool policy_valid(const dre_touch_tlmm_policy *p)
{
    return p && p->version==DRE_TOUCH_TLMM_POLICY_VERSION && p->struct_bytes==sizeof(*p) &&
        p->owner_service && p->gic && p->enter_owner && p->leave_owner &&
        p->reserve && p->check && p->check_fast && p->release;
}
bool DRETouchTLMMController::initNative(const dre_touch_tlmm_policy *p)
{
    policy={}; profile={}; pin={}; brokerOwner=child=parentNub=NULL; parentController=NULL;
    name=NULL; parentRoute=NULL; pinLease=NULL; sourceOwner=parentToken=0; pinMap=0;
    lifecycle=NULL; childTarget=NULL; childHandler=NULL; childRef=NULL;
    childInFlight=closing=quarantined=0; realPinIRQs=emptyParentIRQs=0; parentSource=0;
    childRegistered=childEnabled=parentAttempted=parentRegistered=namespaceRegistered=false;
    hardwareHold=started=attached=childStarted=publishedEver=false;
    controllerLock=NULL; vectors=NULL;
    if (!IOInterruptController::init() || !policy_valid(p)) return false;
    brokerOwner=OSDynamicCast(IOService,(OSObject *)p->owner_service);
    if (!brokerOwner || brokerOwner->isInactive()) { brokerOwner=NULL; return false; }
    brokerOwner->retain(); policy=*p;
    const void *fdt=NULL; size_t bytes=0;
    dre_tn_profile selectedTouch={};
    if (dre_native_board_snapshot(&fdt,&bytes) || dre_tn_profile_from_fdt(fdt,bytes,&selectedTouch) ||
        selectedTouch.irq_gpio!=22 || dre_tn_tlmm_profile_from_fdt(fdt,bytes,&profile)) return false;
    lifecycle=IOLockAlloc(); controllerLock=IOSimpleLockAlloc();
    name=OSSymbol::withCString("DRETouchTLMM.GPIO22");
    return lifecycle && controllerLock && name;
}
int DRETouchTLMMController::enterOwner(int *entered)
{
    if (!entered) return DRE_TN_ARGUMENT;
    *entered=-1;
    if (policy.enter_owner(policy.cookie,entered) || (*entered!=0 && *entered!=1)) return DRE_TN_OWNERSHIP;
    return DRE_TN_OK;
}
void DRETouchTLMMController::leaveOwner(int entered)
{ policy.leave_owner(policy.cookie,entered); }
bool DRETouchTLMMController::owned()
{ return pinLease && sourceOwner && !policy.check(policy.cookie,pinLease,sourceOwner); }
bool DRETouchTLMMController::fastOwned()
{ return pinLease && sourceOwner && !policy.check_fast(policy.cookie,pinLease,sourceOwner); }
bool DRETouchTLMMController::stopping()
{ return __atomic_load_n(&closing,__ATOMIC_ACQUIRE) || __atomic_load_n(&quarantined,__ATOMIC_ACQUIRE); }
int DRETouchTLMMController::pinGuard(void *v)
{ return ((DRETouchTLMMController *)v)->fastOwned()?DRE_TN_OK:DRE_TN_OWNERSHIP; }
uintptr_t DRETouchTLMMController::pinLock(void *v)
{ return IOSimpleLockLockDisableInterrupt(((DRETouchTLMMController *)v)->controllerLock); }
void DRETouchTLMMController::pinUnlock(void *v,uintptr_t state)
{ IOSimpleLockUnlockEnableInterrupt(((DRETouchTLMMController *)v)->controllerLock,(IOInterruptState)state); }
int DRETouchTLMMController::pinRead(void *v,unsigned off,uint32_t *out)
{
    DRETouchTLMMController *s=(DRETouchTLMMController *)v;
    if (!out || !s->pinMap || (off!=0 && off!=4 && off!=8 && off!=12) || !s->fastOwned()) return DRE_TN_MMIO;
    *out=*(volatile uint32_t *)(s->pinMap+0x2000+off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return DRE_TN_OK;
}
int DRETouchTLMMController::pinWrite(void *v,unsigned off,uint32_t value)
{
    DRETouchTLMMController *s=(DRETouchTLMMController *)v;
    if (!s->pinMap || (off!=8 && off!=12) || !s->fastOwned()) return DRE_TN_MMIO;
    *(volatile uint32_t *)(s->pinMap+0x2000+off)=value;
    __asm__ volatile("dsb sy" ::: "memory");
    return DRE_TN_OK;
}
void DRETouchTLMMController::pinFinish(void *)
{ __asm__ volatile("dsb sy" ::: "memory"); }

int DRETouchTLMMController::openNative()
{
    int entered=0;
    int r=enterOwner(&entered);
    if (r) return r;
    IOLockLock(lifecycle);
    r=policy.reserve(policy.cookie,&profile,&pinLease,&sourceOwner);
    if ((pinLease || sourceOwner) && !hardwareHold) { retain(); hardwareHold=true; }
    if (r || !owned()) { r=r?r:DRE_TN_OWNERSHIP; goto done; }
    if (!attach(brokerOwner)) { r=DRE_TN_PROVIDER; goto done; }
    attached=true;
    if (!IOInterruptController::start(brokerOwner)) { r=DRE_TN_PROVIDER; goto done; }
    started=true;
    pinMap=ml_io_map_unmappable(0x00514000,0x4000,VM_WIMG_IO);
    if (!pinMap) { r=DRE_TN_MMIO; goto done; }
    {
        dre_tn_tlmm_ops ops={this,pinGuard,pinLock,pinUnlock,pinRead,pinWrite,pinFinish};
        r=dre_tn_tlmm_begin(&pin,&profile,&ops);
        if (r) goto done;
        dre_spi_request request={profile.parent_intid,DRE_SPI_LEVEL_HIGH,sourceOwner};
        r=dre_spi_reserve(policy.gic,&request,&parentRoute);
        if (r || !parentRoute) { r=r?r:DRE_TN_OWNERSHIP; goto done; }
        parentToken=dre_spi_route_token(parentRoute);
        IOService *parent=NULL; IOInterruptController *controller=NULL; unsigned source=0;
        if (!parentToken || dre_spi_route_mask_join(parentRoute) ||
            dre_spi_route_nub(parentRoute,&parent,&source,&controller) ||
            !parent || !controller || source!=0 ||
            dre_spi_route_check_handler(parentRoute,NULL)) { r=DRE_TN_OWNERSHIP; goto done; }
        parentNub=parent; parentController=controller; parentSource=source;
        parentNub->retain(); parentController->retain();
        /* Real GIC source is acquired, qualified, masked and joined before the
         * only pin target/type configuration. MPM/PDC/wake remain untouched. */
        r=dre_tn_tlmm_configure(&pin);
        if (r) goto done;
    }
    if (dre_irq_namespace_register_absent(name,this)!=kIOReturnSuccess) { r=DRE_TN_OWNERSHIP; goto done; }
    namespaceRegistered=true;
    setName("DRETouchTLMMController"); setProperty("UNVALIDATED",true);
    registerService();
    publishedEver=true;
    child=new IOService;
    if (!child || !child->init() || !child->attach(this)) { r=DRE_TN_PROVIDER; goto done; }
    if (!child->start(this)) { r=DRE_TN_PROVIDER; goto done; }
    childStarted=true;
    child->setName("DRETouchGPIO22");
    {
        uint32_t vector=0; /* One local vector explicitly maps to GPIO22. */
        OSArray *controllers=OSArray::withCapacity(1),*specs=OSArray::withCapacity(1);
        OSData *spec=OSData::withBytes(&vector,sizeof(vector));
        bool good=controllers && specs && spec && controllers->setObject(name) && specs->setObject(spec) &&
            child->setProperty(gIOInterruptControllersKey,controllers) &&
            child->setProperty(gIOInterruptSpecifiersKey,specs);
        OSSafeReleaseNULL(controllers); OSSafeReleaseNULL(specs);
        if (good) {
            good=dre_irq_source_cache_allocate(child,0)==kIOReturnSuccess &&
                dre_irq_namespace_bind_cached_source(name,this,child,0,spec)==kIOReturnSuccess;
        }
        OSSafeReleaseNULL(spec);
        if (!good) { r=DRE_TN_PROVIDER; goto done; }
    }
    parentAttempted=true;
    if (parentNub->registerInterrupt((int)parentSource,this,parentAction)!=kIOReturnSuccess) {
        __atomic_store_n(&quarantined,1,__ATOMIC_RELEASE); r=DRE_TN_OWNERSHIP; goto done;
    }
    parentRegistered=true;
    if (dre_spi_route_check_handler(parentRoute,this)) {
        __atomic_store_n(&quarantined,1,__ATOMIC_RELEASE); r=DRE_TN_OWNERSHIP; goto done;
    }
    setProperty("TouchIRQPhase","BOUND_MASKED_UNVALIDATED");
    r=DRE_TN_OK;
done:
    IOLockUnlock(lifecycle);
    leaveOwner(entered);
    return r;
}

bool DRETouchTLMMController::exact(IOService *n,int source)
{
    return n && n==child && source==0 && n->_numInterruptSources==1 && n->_interruptSources &&
        n->_interruptSources[0].interruptController==this && n->_interruptSources[0].vectorData &&
        n->_interruptSources[0].vectorData->getLength()==sizeof(uint32_t) &&
        n->_interruptSources[0].vectorData->getBytesNoCopy() &&
        *(const uint32_t *)n->_interruptSources[0].vectorData->getBytesNoCopy()==0;
}
IOReturn DRETouchTLMMController::getInterruptType(IOService *n,int source,int *type)
{
    unsigned level=0;
    if (!type || !exact(n,source) || !fastOwned() || dre_tn_tlmm_sample(&pin,&level))
        return kIOReturnNotReady;
    *type=kIOInterruptTypeEdge; /* Exact programmed falling-edge pin; parent is level-high. */
    return kIOReturnSuccess;
}
IOReturn DRETouchTLMMController::registerInterrupt(IOService *n,int source,void *target,
                                                  IOInterruptHandler handler,void *ref)
{
    int entered=0;
    if (!target || !handler || enterOwner(&entered)) return kIOReturnNotPermitted;
    IOLockLock(lifecycle);
    IOReturn result=kIOReturnNotReady;
    if (exact(n,source) && owned() && parentRegistered && !stopping() &&
        !dre_spi_route_check_handler(parentRoute,this)) {
        IOInterruptState state=IOSimpleLockLockDisableInterrupt(controllerLock);
        if (!childRegistered && !childInFlight) {
            childTarget=(OSObject *)target; childTarget->retain();
            childHandler=handler; childRef=ref; childEnabled=false; childRegistered=true;
            result=kIOReturnSuccess;
        } else result=kIOReturnExclusiveAccess;
        IOSimpleLockUnlockEnableInterrupt(controllerLock,state);
    }
    IOLockUnlock(lifecycle); leaveOwner(entered);
    return result;
}
int DRETouchTLMMController::maskJoin()
{
    if (!owned() || !parentRoute || !parentRegistered || dre_tn_tlmm_mask(&pin,0)) return DRE_TN_OWNERSHIP;
    IOInterruptState state=IOSimpleLockLockDisableInterrupt(controllerLock);
    childEnabled=false;
    IOSimpleLockUnlockEnableInterrupt(controllerLock,state);
    if (dre_spi_route_mask_join(parentRoute) || dre_spi_route_check_handler(parentRoute,this) ||
        __atomic_load_n(&childInFlight,__ATOMIC_ACQUIRE)) return DRE_TN_OWNERSHIP;
    return DRE_TN_OK;
}
IOReturn DRETouchTLMMController::disableInterrupt(IOService *n,int source)
{
    int entered=0;
    if (enterOwner(&entered)) return kIOReturnNotPermitted;
    IOLockLock(lifecycle);
    IOReturn result=exact(n,source) && childRegistered && !maskJoin()?kIOReturnSuccess:kIOReturnNotReady;
    IOLockUnlock(lifecycle); leaveOwner(entered);
    return result;
}
IOReturn DRETouchTLMMController::enableInterrupt(IOService *n,int source)
{
    int entered=0;
    if (enterOwner(&entered)) return kIOReturnNotPermitted;
    IOLockLock(lifecycle);
    IOReturn result=kIOReturnNotReady;
    if (exact(n,source) && childRegistered && owned() && parentRegistered && !stopping() &&
        !dre_spi_route_check_handler(parentRoute,this)) {
        IOInterruptState state=IOSimpleLockLockDisableInterrupt(controllerLock);
        /* Adapter has already armed/rechecked this same pin engine. Do not
         * ACK again here or invent a pin edge while enabling its parent. */
        childEnabled=true;
        IOSimpleLockUnlockEnableInterrupt(controllerLock,state);
        if (!dre_spi_route_unmask(parentRoute)) result=kIOReturnSuccess;
        else {
            state=IOSimpleLockLockDisableInterrupt(controllerLock); childEnabled=false;
            IOSimpleLockUnlockEnableInterrupt(controllerLock,state);
            __atomic_store_n(&quarantined,1,__ATOMIC_RELEASE);
        }
    }
    IOLockUnlock(lifecycle); leaveOwner(entered);
    return result;
}
IOReturn DRETouchTLMMController::unregisterInterrupt(IOService *n,int source)
{
    int entered=0;
    if (enterOwner(&entered)) return kIOReturnNotPermitted;
    IOLockLock(lifecycle);
    IOReturn result=kIOReturnNotReady;
    OSObject *target=NULL;
    if (exact(n,source) && !__atomic_load_n(&quarantined,__ATOMIC_ACQUIRE)) {
        if (!childRegistered) result=kIOReturnSuccess;
        else if (!maskJoin()) {
            IOInterruptState state=IOSimpleLockLockDisableInterrupt(controllerLock);
            childRegistered=childEnabled=false; target=childTarget;
            childTarget=NULL; childHandler=NULL; childRef=NULL;
            IOSimpleLockUnlockEnableInterrupt(controllerLock,state);
            result=kIOReturnSuccess;
        }
    }
    IOLockUnlock(lifecycle); leaveOwner(entered);
    OSSafeReleaseNULL(target);
    return result;
}
IOReturn DRETouchTLMMController::causeInterrupt(IOService *,int)
{ return kIOReturnUnsupported; } /* Never fabricate a touch IRQ. */
IOInterruptAction DRETouchTLMMController::getInterruptHandlerAddress()
{ return parentAction; }
void DRETouchTLMMController::parentAction(OSObject *owner,void *ref,IOService *n,int source)
{ (void)((DRETouchTLMMController *)owner)->handleInterrupt(ref,n,source); }
IOReturn DRETouchTLMMController::handleInterrupt(void *,IOService *n,int source)
{
    if (n!=parentNub || source<0 || (unsigned)source!=parentSource || !fastOwned() ||
        !parentRoute || dre_spi_route_check_fast(parentRoute,parentToken)) return kIOReturnNotReady;
    unsigned pending=0;
    if (dre_tn_tlmm_pending(&pin,&pending)) {
        __atomic_store_n(&quarantined,1,__ATOMIC_RELEASE);
        /* Only the still-owned parent vector may be masked. Foreign pins,
         * MPM/PDC, pending clears and GIC EOI are never written here. */
        if (fastOwned() && !dre_spi_route_check_fast(parentRoute,parentToken))
            (void)dre_spi_route_mask_fast(parentRoute);
        return kIOReturnNotReady;
    }
    if (!pending) { __atomic_fetch_add(&emptyParentIRQs,1,__ATOMIC_RELAXED); return kIOReturnSuccess; }
    IOInterruptState state=IOSimpleLockLockDisableInterrupt(controllerLock);
    OSObject *target=childTarget; IOInterruptHandler handler=childHandler; void *ref=childRef;
    bool deliver=childRegistered && childEnabled && target && handler && !stopping();
    if (deliver) __atomic_fetch_add(&childInFlight,1,__ATOMIC_ACQ_REL);
    IOSimpleLockUnlockEnableInterrupt(controllerLock,state);
    if (deliver) {
        __atomic_fetch_add(&realPinIRQs,1,__ATOMIC_RELAXED);
        handler(target,ref,child,0);
        __atomic_fetch_sub(&childInFlight,1,__ATOMIC_RELEASE);
    }
    /* The selected SLEH dispatcher performs the sole parent GIC EOI. */
    return kIOReturnSuccess;
}

int DRETouchTLMMController::closeNative()
{
    int entered=0;
    int r=enterOwner(&entered);
    if (r) return r;
    IOLockLock(lifecycle);
    __atomic_store_n(&closing,1,__ATOMIC_RELEASE);
    if (__atomic_load_n(&quarantined,__ATOMIC_ACQUIRE) || childRegistered ||
        __atomic_load_n(&childInFlight,__ATOMIC_ACQUIRE) || (parentAttempted && !parentRegistered)) {
        r=DRE_TN_POISONED; goto done;
    }
    if (parentRoute) {
        if (pin.configured && dre_tn_tlmm_mask(&pin,0)) { r=DRE_TN_OWNERSHIP; goto done; }
        if (dre_spi_route_mask_join(parentRoute)) { r=DRE_TN_OWNERSHIP; goto done; }
        if (parentRegistered) {
            if (dre_spi_route_check_handler(parentRoute,this) ||
                parentNub->unregisterInterrupt((int)parentSource)!=kIOReturnSuccess) {
                __atomic_store_n(&quarantined,1,__ATOMIC_RELEASE); r=DRE_TN_OWNERSHIP; goto done;
            }
            parentRegistered=parentAttempted=false;
        }
        if (dre_spi_route_check_handler(parentRoute,NULL)) { r=DRE_TN_OWNERSHIP; goto done; }
    }
    if (dre_tn_tlmm_end(&pin)) { r=DRE_TN_POISONED; goto done; }
    if (parentRoute && dre_spi_route_release(&parentRoute)) { r=DRE_TN_OWNERSHIP; goto done; }
    if (namespaceRegistered) {
        if (dre_irq_namespace_unregister_same(name,this)!=kIOReturnSuccess) { r=DRE_TN_OWNERSHIP; goto done; }
        namespaceRegistered=false;
    }
    if (child) {
        if (childStarted) { child->stop(this); childStarted=false; }
        if (child->getProvider()==this) child->detach(this);
        OSSafeReleaseNULL(child);
    }
    OSSafeReleaseNULL(parentNub); OSSafeReleaseNULL(parentController); parentToken=0;
    if (pinLease || sourceOwner) {
        if (policy.release(policy.cookie,pinLease,sourceOwner)) { r=DRE_TN_OWNERSHIP; goto done; }
        pinLease=NULL; sourceOwner=0;
    }
    if (pinMap) { ml_io_unmap(pinMap,0x4000); pinMap=0; }
    if (started) { IOInterruptController::stop(brokerOwner); started=false; }
    if (attached) { detach(brokerOwner); attached=false; }
    r=DRE_TN_OK;
done:
    IOLockUnlock(lifecycle); leaveOwner(entered);
    /* General IOService client/cache termination has not been proved for this
     * new controller. Once published, retain the actual closed object until
     * reboot so no late cached controller pointer can target freed memory.
     * Hardware/pin/parent leases above are still released after real joins.
     * Failed unpublished acquisitions may drop their extra owner normally. */
    if (!r && publishedEver) setProperty("TouchIRQPhase","RETIRED_RESIDENT_UNVALIDATED");
    if (!r && hardwareHold && !publishedEver) { hardwareHold=false; release(); }
    return r;
}
void DRETouchTLMMController::stop(IOService *provider)
{
    __atomic_store_n(&closing,1,__ATOMIC_RELEASE);
    if (parentRoute && fastOwned() && !dre_spi_route_check_fast(parentRoute,parentToken))
        (void)dre_spi_route_mask_fast(parentRoute);
    /* Ordinary stop does not release a possibly referenced GPIO/parent owner. */
    IOInterruptController::stop(provider);
}
void DRETouchTLMMController::free()
{
    if (pinLease || sourceOwner || parentRoute || pin.active || pin.mutated || pin.poisoned ||
        childRegistered || childInFlight || parentAttempted || parentRegistered || namespaceRegistered || publishedEver) return;
    OSSafeReleaseNULL(child); OSSafeReleaseNULL(parentNub); OSSafeReleaseNULL(parentController);
    OSSafeReleaseNULL(name); OSSafeReleaseNULL(brokerOwner);
    if (pinMap) ml_io_unmap(pinMap,0x4000);
    if (lifecycle) IOLockFree(lifecycle);
    if (controllerLock) { IOSimpleLockFree(controllerLock); controllerLock=NULL; }
    IOInterruptController::free();
}

extern "C" int dre_touch_tlmm_publish(const dre_touch_tlmm_policy *policy,void **owner)
{
    if (!owner || *owner || !policy_valid(policy)) return DRE_TN_ARGUMENT;
    DRETouchTLMMController *s=new DRETouchTLMMController;
    if (!s) return DRE_TN_PROVIDER;
    if (!s->initNative(policy)) { s->release(); return DRE_TN_PROVIDER; }
    int r=s->openNative();
    if (r && !s->hardwareHold && !s->pinLease && !s->sourceOwner && !s->parentRoute) { s->release(); return r; }
    *owner=s; /* A failed partially acquired owner must remain reachable. */
    return r;
}
extern "C" int dre_touch_tlmm_nub(void *owner,void **nub,unsigned *source)
{
    DRETouchTLMMController *s=owner?OSDynamicCast(DRETouchTLMMController,(OSObject *)owner):NULL;
    if (!s || !nub || !source) return DRE_TN_PROVIDER;
    int entered=0;
    int result=s->enterOwner(&entered);
    if (result) return result;
    IOLockLock(s->lifecycle);
    if (!s->child || !s->owned() || s->stopping() || !s->parentRegistered ||
        !s->namespaceRegistered || !s->exact(s->child,0)) result=DRE_TN_PROVIDER;
    else { *nub=s->child; *source=0; result=DRE_TN_OK; }
    IOLockUnlock(s->lifecycle); s->leaveOwner(entered);
    return result;
}
extern "C" int dre_touch_tlmm_pin_for_nub(void *nub,unsigned source,dre_tn_tlmm_pin **pin)
{
    IOService *n=nub?OSDynamicCast(IOService,(OSObject *)nub):NULL;
    DRETouchTLMMController *s=n?OSDynamicCast(DRETouchTLMMController,n->getProvider()):NULL;
    if (!s || !pin || source || !s->exact(n,0) || !s->fastOwned() || s->stopping() ||
        !s->parentRegistered || !s->pin.configured || !s->namespaceRegistered) return DRE_TN_PROVIDER;
    *pin=&s->pin;
    return DRE_TN_OK;
}
extern "C" int dre_touch_tlmm_shutdown(void **owner)
{
    if (!owner || !*owner) return DRE_TN_ARGUMENT;
    DRETouchTLMMController *s=OSDynamicCast(DRETouchTLMMController,(OSObject *)*owner);
    if (!s) return DRE_TN_ARGUMENT;
    int r=s->closeNative();
    if (!r) { *owner=NULL; s->release(); }
    return r;
}
#endif
