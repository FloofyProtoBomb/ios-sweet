/* SPDX-License-Identifier: MIT
 * UNVALIDATED. EOF include in the selected IOService.cpp, paired with the
 * DRE-only `friend class DREIRQSourceCacheAccess;` in actual IOService.h.
 * No private-to-public macro, layout/vtable change or fake source allocation.
 */
#include "platform_namespace.h"
#if defined(ARM64_BOARD_CONFIG_DRE)
class DREIRQSourceCacheAccess {
public:
    static IOReturn allocate(IOService *nub,unsigned source)
    {
        if(!nub||source||nub->_interruptSources||nub->_numInterruptSources)
            return kIOReturnBadArgument;
        OSArray *names=OSDynamicCast(OSArray,nub->getProperty(gIOInterruptControllersKey));
        OSArray *specs=OSDynamicCast(OSArray,nub->getProperty(gIOInterruptSpecifiersKey));
        if(!names||!specs||names->getCount()!=1||specs->getCount()!=1||
           !OSDynamicCast(OSSymbol,names->getObject(0))||
           !OSDynamicCast(OSData,specs->getObject(0)))return kIOReturnBadArgument;
        IOInterruptController *unused=NULL;
        /* Actual private method performs its existing paired source/private
         * array allocation, then false returns without missing-name lookup. */
        IOReturn status=nub->lookupInterrupt(0,false,&unused);
        if(status!=kIOReturnNoInterrupt)return status;
        if(unused||nub->_numInterruptSources!=1||!nub->_interruptSources||
           nub->_interruptSources[0].interruptController||
           nub->_interruptSources[0].vectorData)return kIOReturnError;
        return kIOReturnSuccess;
    }
};
extern "C" IOReturn dre_irq_source_cache_allocate(IOService *nub,unsigned source)
{ return DREIRQSourceCacheAccess::allocate(nub,source); }
#endif
