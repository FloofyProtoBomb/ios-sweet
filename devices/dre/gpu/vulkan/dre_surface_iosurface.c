/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Optional real IOSurface framework wrapper, not yet built or
 * linked into the runtime/Turnip adapter. Source-only overnight work.
 */
#include "dre_surface_iosurface.h"
#if defined(__APPLE__)
#include <mach/mach.h>
enum dre_darwin_result dre_darwin_surface_retain_iosurface(struct dre_darwin *c,
    IOSurfaceRef surface, uint32_t access, struct dre_darwin_surface **out)
{
    if(!out)return DRE_DARWIN_INVALID;
    *out=NULL;
    if(!c||!surface||!(access&DRE_GPU_ACCESS_READ)||(access&~3u))return DRE_DARWIN_INVALID;
    mach_port_t port=IOSurfaceCreateMachPort(surface);
    if(!MACH_PORT_VALID(port))return DRE_DARWIN_INVALID;
    enum dre_darwin_result status=dre_darwin_surface_retain_port(c,(uint32_t)port,access,out);
    /* The kernel has an independent retained backing object on a typed reply.
     * Releasing this newly created send right never releases the caller's
     * IOSurfaceRef. Even on failure, *out can still own a partial kernel pin. */
    kern_return_t dropped=mach_port_deallocate(mach_task_self(),port);
    if(dropped!=KERN_SUCCESS&&status==DRE_DARWIN_OK)return DRE_DARWIN_LOST;
    return status;
}
#endif
