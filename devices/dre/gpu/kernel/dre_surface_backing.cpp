/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Real registered backing callbacks only, no default success.
 * No GPU cache conversion, SMMU map, import BO, queue or capability grant.
 */
#include "dre_surface_backing.h"
#if defined(DRE)
#include <IOKit/IOLib.h>
static const dre_surface_bridge *bridge;
static IOService *bridgeOwner;
int dre_surface_install_bridge(IOService *owner,const dre_surface_bridge *ops)
{
    if(!owner||!ops||ops->version!=1||ops->struct_bytes!=sizeof(*ops)||
       !ops->retain_from_port||!ops->describe||!ops->close)return DRE_GPU_INVALID;
    const dre_surface_bridge *current=__atomic_load_n(&bridge,__ATOMIC_ACQUIRE);
    if(current)return current==ops&&bridgeOwner==owner?DRE_GPU_OK:DRE_GPU_BUSY;
    owner->retain();
    /* Root install is single-threaded, but a competing installer still cannot
     * replace callbacks for live pins. Publish owner before release pointer. */
    IOService *expected=NULL;
    if(!__atomic_compare_exchange_n(&bridgeOwner,&expected,owner,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        owner->release();return DRE_GPU_BUSY;
    }
    __atomic_store_n(&bridge,ops,__ATOMIC_RELEASE);
    return DRE_GPU_OK;
}
int dre_surface_retain_from_port(task_t task,uint32_t port,uint32_t access,
    void **pin,dre_gpu_surface_description *out)
{
    if(!pin||!out)return DRE_GPU_INVALID;
    *pin=NULL;bzero(out,sizeof(*out));
    const dre_surface_bridge *ops=__atomic_load_n(&bridge,__ATOMIC_ACQUIRE);
    if(!ops)return DRE_GPU_UNSUPPORTED;
    if(!task||task==kernel_task||!port||!access||(access&~3u))return DRE_GPU_INVALID;
    return ops->retain_from_port(task,port,access,pin,out);
}
int dre_surface_describe(void *pin,dre_gpu_surface_description *out)
{
    if(!pin||!out)return DRE_GPU_INVALID;
    bzero(out,sizeof(*out));
    const dre_surface_bridge *ops=__atomic_load_n(&bridge,__ATOMIC_ACQUIRE);
    return ops?ops->describe(pin,out):DRE_GPU_UNSUPPORTED;
}
int dre_surface_close(void *pin)
{
    const dre_surface_bridge *ops=__atomic_load_n(&bridge,__ATOMIC_ACQUIRE);
    return pin&&ops?ops->close(pin):DRE_GPU_UNSUPPORTED;
}
#endif
