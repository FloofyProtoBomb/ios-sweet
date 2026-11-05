/* SPDX-License-Identifier: MIT; UNVALIDATED source-only draft. */
#pragma once
#include "dre_gpu_uapi.h"
#if defined(DRE)
#include <kern/task.h>
#include <IOKit/IOService.h>
struct dre_surface_bridge {
    uint32_t version,struct_bytes;
    /* Actual SweetSurface Mach-right resolver and retained/prepare lifecycle.
     * A failure may still return a retained pin: caller must preserve it and
     * close only through this bridge. No CPU snapshot/host pointer import. */
    int (*retain_from_port)(task_t,uint32_t,uint32_t,void **,dre_gpu_surface_description *);
    int (*describe)(void *,dre_gpu_surface_description *);
    int (*close)(void *); /* actual descriptor complete before final release */
};
/* Install once from the selected real IOSurface root. Owner is retained;
 * no unload/replacement can invalidate outstanding backing objects. */
int dre_surface_install_bridge(IOService *,const dre_surface_bridge *);
int dre_surface_retain_from_port(task_t,uint32_t,uint32_t,void **,dre_gpu_surface_description *);
int dre_surface_describe(void *,dre_gpu_surface_description *);
int dre_surface_close(void *);
#endif
