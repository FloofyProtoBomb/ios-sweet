/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Independent lifetime/route capture for pinned XNU8796 IOKit.
 * Uses public IOInterruptSource fields intended for controller implementors.
 * No tests, compilation or native execution performed for this source.
 */
#include "xnu_irq_route.h"
#if defined(DRE)
#include "../irq/platform_namespace.h"
#include <IOKit/IOLib.h>
#include <string.h>

int DRETouchIRQRoute::capture(IOService *n,unsigned s,IOPlatformExpert *p,
                              void *context,int (*held)(void *))
{
    if (nub || controller || platform || controllerList || specifierList || captured ||
        attempted || registered || uncertain || !n || !p || !held || s>255 ||
        held(context) || n->isInactive() || p->isInactive()) return DRE_TN_PROVIDER;
    cookie=context; guard=held;
    nub=n; nub->retain(); platform=p; platform->retain(); source=s;
    OSObject *a=nub->copyProperty(gIOInterruptControllersKey);
    OSObject *b=nub->copyProperty(gIOInterruptSpecifiersKey);
    controllerList=OSDynamicCast(OSArray,a);
    specifierList=OSDynamicCast(OSArray,b);
    if (!controllerList) OSSafeReleaseNULL(a);
    if (!specifierList) OSSafeReleaseNULL(b);
    if (!controllerList || !specifierList) return DRE_TN_PROVIDER;
    sources=controllerList->getCount();
    if (!sources || sources>256 || sources!=specifierList->getCount() || s>=sources)
        return DRE_TN_PROVIDER;
    controllerName=OSDynamicCast(OSSymbol,controllerList->getObject(s));
    specifier=OSDynamicCast(OSData,specifierList->getObject(s));
    if (controllerName) controllerName->retain();
    if (specifier) specifier->retain();
    if (!controllerName || !specifier) return DRE_TN_PROVIDER;
    bytes=specifier->getLength();
    if (bytes<sizeof(int32_t) || bytes>sizeof(words) || (bytes&3) ||
        !specifier->getBytesNoCopy()) return DRE_TN_PROVIDER;
    bzero(words,sizeof(words));
    memcpy(words,specifier->getBytesNoCopy(),bytes);
    int32_t vector=0;
    memcpy(&vector,words,sizeof(vector));
    /* This is only a bounded metadata sanity check. The real native owner
     * must additionally qualify its controller's actual vector range. */
    if (vector<0 || vector>=4096 || guard(cookie)) return DRE_TN_PROVIDER;
    /* Pinned lookUpInterruptController sleeps indefinitely if a name is not
     * registered. Do not use it as an existence probe, or trigger IOService's
     * resolver on an unqualified/missing route. The actual native publisher
     * must already have resolved this source after registering its own real
     * controller. Inspect that concrete cache without any wait here. */
    if (nub->_numInterruptSources!=(int)sources || !nub->_interruptSources ||
        nub->_interruptSources[s].vectorData!=specifier) return DRE_TN_PROVIDER;
    controller=nub->_interruptSources[s].interruptController;
    if (controller) controller->retain();
    if (!controller || controller->isInactive() ||
        !(controller->getState()&kIOServiceRegisteredState) ||
        OSDynamicCast(IOSharedInterruptController,controller) ||
        !controller->getInterruptHandlerAddress()) return DRE_TN_PROVIDER;
    IOInterruptController *registered=dre_irq_namespace_copy_nowait(controllerName);
    bool same=registered==controller;
    OSSafeReleaseNULL(registered);
    if (!same) return DRE_TN_PROVIDER;
    /* Require the real cached controller's type, not an arbitrary nub override
     * which merely returns "edge" without an actual registered binding.
     * No IRQ disable/register/MMIO write has been authorized by this capture. */
    int type=0;
    if (controller->getInterruptType(nub,(int)s,&type)!=kIOReturnSuccess || type!=kIOInterruptTypeEdge)
        return DRE_TN_PROVIDER;
    captured=true;
    return check();
}

int DRETouchIRQRoute::check()
{
    if (!captured || uncertain || !guard || guard(cookie) || !nub || !controller || !platform ||
        nub->isInactive() || controller->isInactive() || platform->isInactive() ||
        IOService::getPlatform()!=platform || !(controller->getState()&kIOServiceRegisteredState) ||
        OSDynamicCast(IOSharedInterruptController,controller) ||
        !controller->getInterruptHandlerAddress() || !controllerList || !specifierList ||
        source>=sources || controllerList->getCount()!=sources || specifierList->getCount()!=sources ||
        controllerList->getObject(source)!=controllerName || specifierList->getObject(source)!=specifier ||
        !specifier || specifier->getLength()!=bytes || !specifier->getBytesNoCopy() ||
        memcmp(specifier->getBytesNoCopy(),words,bytes) ||
        nub->_numInterruptSources!=(int)sources || !nub->_interruptSources ||
        nub->_interruptSources[source].interruptController!=controller ||
        nub->_interruptSources[source].vectorData!=specifier) return DRE_TN_OWNERSHIP;
    IOInterruptController *registered=dre_irq_namespace_copy_nowait(controllerName);
    bool inNamespace=registered==controller;
    OSSafeReleaseNULL(registered);
    if (!inNamespace) return DRE_TN_OWNERSHIP;
    OSObject *a=nub->copyProperty(gIOInterruptControllersKey);
    OSObject *b=nub->copyProperty(gIOInterruptSpecifiersKey);
    bool same=a==controllerList && b==specifierList;
    OSSafeReleaseNULL(a); OSSafeReleaseNULL(b);
    return same?DRE_TN_OK:DRE_TN_OWNERSHIP;
}

int DRETouchIRQRoute::registerOwned(OSObject *target,IOInterruptAction action,void *ref)
{
    if (!target || !action || attempted || registered || check()) return DRE_TN_OWNERSHIP;
    attempted=true;
    IOReturn r=controller->registerInterrupt(nub,(int)source,target,(IOInterruptHandler)action,ref);
    if (r!=kIOReturnSuccess) {
        /* An unknown subclass's failed register can have partial side effects.
         * Do not unregister an earlier/foreign vector to "undo" that failure.
         * Keep all actual platform/resource references for explicit recovery. */
        __atomic_store_n(&uncertain,true,__ATOMIC_RELEASE);
        return DRE_TN_OWNERSHIP;
    }
    __atomic_store_n(&registered,true,__ATOMIC_RELEASE);
    if (check()) {
        /* Generic shared-vector conversion rewrites the cache. Even if a
         * subclass allowed it, this exclusive GPIO source does not accept it. */
        __atomic_store_n(&uncertain,true,__ATOMIC_RELEASE);
        return DRE_TN_OWNERSHIP;
    }
    return DRE_TN_OK;
}

int DRETouchIRQRoute::disableJoin()
{
    if (!registered || check()) return DRE_TN_OWNERSHIP;
    if (controller->disableInterrupt(nub,(int)source)!=kIOReturnSuccess) {
        __atomic_store_n(&uncertain,true,__ATOMIC_RELEASE);
        return DRE_TN_OWNERSHIP;
    }
    return check();
}
int DRETouchIRQRoute::enableOwned()
{
    if (!registered || check()) return DRE_TN_OWNERSHIP;
    if (controller->enableInterrupt(nub,(int)source)!=kIOReturnSuccess) {
        __atomic_store_n(&uncertain,true,__ATOMIC_RELEASE);
        return DRE_TN_OWNERSHIP;
    }
    if (check()) {
        __atomic_store_n(&uncertain,true,__ATOMIC_RELEASE);
        return DRE_TN_OWNERSHIP;
    }
    return DRE_TN_OK;
}
int DRETouchIRQRoute::unregisterOwned()
{
    if (uncertain || (attempted && !registered)) return DRE_TN_POISONED;
    if (!registered) return DRE_TN_OK;
    if (disableJoin() || controller->unregisterInterrupt(nub,(int)source)!=kIOReturnSuccess) {
        __atomic_store_n(&uncertain,true,__ATOMIC_RELEASE);
        return DRE_TN_OWNERSHIP;
    }
    __atomic_store_n(&registered,false,__ATOMIC_RELEASE);
    attempted=false;
    return DRE_TN_OK;
}
int DRETouchIRQRoute::releaseCapture()
{
    if (attempted || registered || uncertain) return DRE_TN_POISONED;
    OSSafeReleaseNULL(specifier); OSSafeReleaseNULL(controllerName);
    OSSafeReleaseNULL(specifierList); OSSafeReleaseNULL(controllerList);
    OSSafeReleaseNULL(controller); OSSafeReleaseNULL(platform); OSSafeReleaseNULL(nub);
    cookie=NULL; guard=NULL; source=sources=bytes=0; captured=false;
    bzero(words,sizeof(words));
    return DRE_TN_OK;
}
bool DRETouchIRQRoute::fastMatches(IOService *n,int s) const
{
    /* No property lookup, virtual controller method, allocation or sleep from
     * hard IRQ. Native check_irq_lease_fast separately checks the held epoch. */
    return n==nub && s>=0 && (unsigned)s==source &&
        __atomic_load_n(&registered,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&uncertain,__ATOMIC_ACQUIRE);
}
#endif
