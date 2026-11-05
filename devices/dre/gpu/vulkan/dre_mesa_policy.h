/* SPDX-License-Identifier: MIT
 * UNVALIDATED source draft. This withholds known missing transport features;
 * it does not qualify the many remaining A619 compiler/Vulkan feature claims.
 * Included after tu_device.h by the pinned source-preparation recipe.
 */
#pragma once
#include <string.h>
#include "tu_knl.h"
#include "dre_gpu_uapi.h"
static inline bool tu_is_dre_darwin(const struct tu_instance *instance)
{
#ifdef TU_HAS_DARWIN
   return instance->knl && !strcmp(instance->knl->name, "dre-darwin");
#else
   (void)instance; return false;
#endif
}
static inline void tu_dre_filter_extensions(struct vk_device_extension_table *e)
{
   e->KHR_external_memory_fd = false;
   e->KHR_external_fence_fd = false;
   e->KHR_external_semaphore_fd = false;
   e->EXT_external_memory_dma_buf = false;
   e->EXT_external_memory_host = false;
   e->EXT_image_drm_format_modifier = false;
   e->EXT_physical_device_drm = false;
   e->EXT_queue_family_foreign = false;
   e->ANDROID_external_memory_android_hardware_buffer = false;
   e->ANDROID_native_buffer = false;
   e->KHR_calibrated_timestamps = false;
   e->EXT_calibrated_timestamps = false;
   e->KHR_performance_query = false;
   e->EXT_map_memory_placed = false;
   e->KHR_global_priority = false;
   e->EXT_global_priority = false;
   e->EXT_global_priority_query = false;
   /* IOSurface/CAMetalLayer presentation is not a Vulkan transport in ABI1.
    * The draft build must use platforms=[]; enabling Mesa's Metal WSI can
    * recursively enter this same Metal factory and is not a fallback. */
   e->KHR_swapchain = false;
   e->KHR_swapchain_maintenance1 = false;
   e->KHR_swapchain_mutable_format = false;
   e->KHR_incremental_present = false;
   e->EXT_swapchain_maintenance1 = false;
   e->EXT_present_timing = false;
   e->GOOGLE_display_timing = false;
   e->EXT_display_control = false;
   e->EXT_hdr_metadata = false;
}
static inline void tu_dre_filter_features(struct vk_features *f)
{
   f->bufferDeviceAddressCaptureReplay = false;
   f->bufferDeviceAddressMultiDevice = false;
   f->descriptorBufferCaptureReplay = false;
   f->accelerationStructureCaptureReplay = false;
   f->memoryMapPlaced = false;
   f->memoryMapRangePlaced = false;
   f->memoryUnmapReserve = false;
   f->performanceCounterQueryPools = false;
   f->performanceCounterMultipleQueryPools = false;
   f->protectedMemory = false;
}
static inline void tu_dre_filter_properties(struct vk_properties *p)
{
   p->maxMemoryAllocationCount = DRE_GPU_MAX_BOS;
   p->maxMemoryAllocationSize = DRE_GPU_BO_MAX_BYTES;
   p->minMemoryMapAlignment = DRE_GPU_CPU_PAGE_BYTES;
   p->timestampComputeAndGraphics = false;
   /* Required positive scalar when timestampValidBits is zero. This is NOT
    * a measured GPU period; no timestamp API/extension is exposed by the draft. */
   p->timestampPeriod = 1.0f;
}
