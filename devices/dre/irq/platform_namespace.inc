/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Include at EOF of selected iokit/Kernel/IOPlatformExpert.cpp.
 * The dictionary and lock below are its real file-static objects.
 */
#include "platform_namespace.h"
#if defined(ARM64_BOARD_CONFIG_DRE)
extern "C" IOReturn
dre_irq_namespace_register_absent(const OSSymbol *name,IOInterruptController *ic)
{
    if(!name||!ic)return kIOReturnBadArgument;
    if(!gIOInterruptControllers||!gIOInterruptControllersLock)return kIOReturnNotReady;
    IOLockLock(gIOInterruptControllersLock);
    IOReturn status=kIOReturnExclusiveAccess;
    if(!gIOInterruptControllers->getObject(name)) {
        if(gIOInterruptControllers->setObject(name,ic)) {
            IOLockWakeup(gIOInterruptControllersLock,gIOInterruptControllers,false);
            status=kIOReturnSuccess;
        } else status=kIOReturnNoMemory;
    }
    IOLockUnlock(gIOInterruptControllersLock);
    return status;
}
extern "C" IOReturn
dre_irq_namespace_unregister_same(const OSSymbol *name,IOInterruptController *ic)
{
    if(!name||!ic)return kIOReturnBadArgument;
    if(!gIOInterruptControllers||!gIOInterruptControllersLock)return kIOReturnNotReady;
    IOLockLock(gIOInterruptControllersLock);
    IOReturn status=kIOReturnExclusiveAccess;
    if(gIOInterruptControllers->getObject(name)==ic) {
        gIOInterruptControllers->removeObject(name);
        status=kIOReturnSuccess;
    }
    IOLockUnlock(gIOInterruptControllersLock);
    return status;
}
extern "C" IOInterruptController *
dre_irq_namespace_copy_nowait(const OSSymbol *name)
{
    if(!name||!gIOInterruptControllers||!gIOInterruptControllersLock)return NULL;
    IOLockLock(gIOInterruptControllersLock);
    IOInterruptController *ic=OSDynamicCast(IOInterruptController,
        gIOInterruptControllers->getObject(name));
    if(ic)ic->retain();
    IOLockUnlock(gIOInterruptControllersLock);
    return ic;
}
extern "C" IOReturn
dre_irq_namespace_bind_cached_source(const OSSymbol *name,IOInterruptController *ic,
                                     IOService *nub,unsigned source,OSData *specifier)
{
    if(!name||!ic||!nub||!specifier||!nub->_interruptSources||
       source>=(unsigned)nub->_numInterruptSources)return kIOReturnBadArgument;
    if(!gIOInterruptControllers||!gIOInterruptControllersLock)return kIOReturnNotReady;
    IOLockLock(gIOInterruptControllersLock);
    IOReturn status=kIOReturnExclusiveAccess;
    IOInterruptSource *cached=&nub->_interruptSources[source];
    if(gIOInterruptControllers->getObject(name)==ic&&
       !cached->interruptController&&!cached->vectorData) {
        cached->interruptController=ic;cached->vectorData=specifier;
        status=kIOReturnSuccess;
    }
    IOLockUnlock(gIOInterruptControllersLock);
    return status;
}
#endif
