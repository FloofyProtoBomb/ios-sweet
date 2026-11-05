/* SPDX-License-Identifier: MIT; UNVALIDATED selected306 source overlay. */
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "bridge/NativeMetalObjects.h"
#include <stdint.h>

/* Data originates from the actual selected runtime Vulkan properties, not an
 * Apple family table or Android's separate driver. Version1 is source-only. */
struct DreMetalDeviceFacts {
    uint32_t version,bytes,vendor,chip,kind,flags;
    uint64_t max_buffer,max_allocation,threadgroup_memory,workgroup_invocations;
    uint32_t axes[3],color_attachments;
};
static_assert(sizeof(DreMetalDeviceFacts)==72,"DRE facts ABI draft");
extern "C" bool nmb_dre_device_facts(const NmbDevice *,DreMetalDeviceFacts *);

/* Call before objc_registerClassPair. Installs only explicit source-backed
 * queries and fences off inherited device APIs not implemented by this class.
 * This is not private base-table initialization or a Metal publication grant. */
bool dre_metal306_install_device_policy(Class,NmbDevice *);
bool dre_metal306_selector_refused(Class,SEL);
