/*
 * SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source draft, not compiled or run.
 * Turnip KMD overlay for the pinned Mesa85fda Darwin recipe. It deliberately
 * replaces the reference A740/AGX tunnel backend rather than emulating DRM or
 * manufacturing a Vulkan device. DRE ABI1 provider capabilities are mandatory.
 */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dre_darwin.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "vk_device.h"
#include "vk_queue.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include "vk_util.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_knl.h"
#include "tu_queue.h"
#include "tu_rmv.h"
#ifdef TU_USE_WSI_PLATFORM
#error "DRE ABI1 has no IOSurface/Metal WSI transport; build with platforms=[]"
#endif

struct dre_bo_record {
   struct dre_bo_record *next;
   struct tu_bo *bo;
   struct dre_darwin_bo *native;
};
struct dre_device {
   struct dre_darwin *native;
   pthread_mutex_t lock;
   struct dre_bo_record *bos;
   uint32_t next_bo_id;
   uint32_t last_cookie;
   uint64_t last_fence;
   bool queue_open;
};
struct dre_submit {
   struct dre_gpu_submit_ib ibs[DRE_GPU_MAX_IBS];
   unsigned ib_count;
   bool overflow;
};
enum dre_sync_state { DRE_SYNC_UNSIGNALED, DRE_SYNC_HOST_SIGNALED, DRE_SYNC_FENCE };
struct dre_sync {
   struct vk_sync base;
   enum dre_sync_state state;
   uint64_t fence;
};

static struct dre_device *dre(struct tu_device *dev)
{ return (struct dre_device *)dev->darwin_retire; }
static VkResult refusal(struct tu_device *dev, enum dre_darwin_result r, const char *where)
{
   switch (r) {
   case DRE_DARWIN_OK: return VK_SUCCESS;
   case DRE_DARWIN_NO_MEMORY: return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   case DRE_DARWIN_TIMEOUT: return VK_TIMEOUT;
   case DRE_DARWIN_BUSY: return VK_NOT_READY;
   case DRE_DARWIN_NO_PROVIDER: case DRE_DARWIN_UNSUPPORTED:
      return vk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT, "%s: %s", where, dre_darwin_result_name(r));
   default:
      return vk_device_set_lost(&dev->vk, "%s: %s", where, dre_darwin_result_name(r));
   }
}
static struct dre_bo_record *find_bo(struct dre_device *d, struct tu_bo *bo)
{
   for (struct dre_bo_record *r = d->bos; r; r = r->next)
      if (r->bo == bo) return r;
   return NULL;
}
static VkResult wait_real(struct tu_device *dev, uint64_t fence, uint64_t abs_timeout)
{
   uint64_t now = os_time_get_nano();
   uint64_t relative = abs_timeout == UINT64_MAX ? UINT64_MAX : abs_timeout > now ? abs_timeout - now : 0;
   return refusal(dev, dre_darwin_fence_wait(dre(dev)->native, fence, relative), "hardware fence wait");
}

static VkResult device_init(struct tu_device *dev)
{
   struct dre_device *d = (struct dre_device *)calloc(1, sizeof(*d));
   if (!d) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   if (pthread_mutex_init(&d->lock, NULL)) { free(d); return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY); }
   enum dre_darwin_result r = dre_darwin_open(DRE_GPU_VULKAN_REQUIRED_CAPS, &d->native);
   if (r != DRE_DARWIN_OK) {
      pthread_mutex_destroy(&d->lock); free(d);
      return vk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED, "DRE native contract: %s", dre_darwin_result_name(r));
   }
   const struct dre_gpu_info_reply *info = dre_darwin_info(d->native);
   /* Enumeration and VkDevice creation are separate opens. Never silently
    * switch GPU generation or memory geometry between those two operations. */
   if (info->chip_id != dev->physical_device->dev_id.chip_id ||
       info->gpu_va_base != dev->physical_device->va_start ||
       info->gpu_va_bytes != dev->physical_device->va_size ||
       info->gmem_bytes != dev->physical_device->gmem_size ||
       info->reset_generation != dev->physical_device->dre_reset_generation) {
      dre_darwin_close(d->native); pthread_mutex_destroy(&d->lock); free(d);
      return vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);
   }
   dev->fd = -1;
   dev->darwin_retire = d;
   /* Required because unresolved Vulkan semaphore waits may block this KMD's
    * driver_submit. Application vkQueueSubmit must remain nonblocking. */
   vk_device_enable_threaded_submit(&dev->vk);
   return VK_SUCCESS;
}
static void device_finish(struct tu_device *dev)
{
   struct dre_device *d = dre(dev);
   if (!d) return;
   if (dre_darwin_close(d->native) != DRE_DARWIN_OK)
      mesa_loge("DRE: native connection teardown uncertain; provider must retain/quarantine DMA ownership");
   while (d->bos) { struct dre_bo_record *next = d->bos->next; free(d->bos); d->bos = next; }
   pthread_mutex_destroy(&d->lock); free(d); dev->darwin_retire = NULL;
}
static int timestamp(struct tu_device *, uint64_t *)
{ return -1; /* No calibrated native timestamp selector exists in ABI1. */ }
static int suspend_count(struct tu_device *, uint64_t *)
{ return -1; /* An absent counter is not a fabricated zero/success. */ }
static VkResult check_status(struct tu_device *dev)
{
   struct dre_device *d = dre(dev);
   pthread_mutex_lock(&d->lock);
   uint64_t fence = d->last_fence;
   if (fence) {
      int complete = 0;
      enum dre_darwin_result r = dre_darwin_fence_query(d->native, fence, &complete);
      pthread_mutex_unlock(&d->lock);
      return refusal(dev, r, "native status query");
   }
   pthread_mutex_unlock(&d->lock);
   return VK_SUCCESS; /* No jobs exist yet; no hardware completion asserted. */
}
static int queue_new(struct tu_device *dev, struct tu_queue *queue)
{
   if (queue->type != TU_QUEUE_GFX) return -1;
   struct dre_device *d = dre(dev);
   pthread_mutex_lock(&d->lock);
   if (d->queue_open) { pthread_mutex_unlock(&d->lock); return -1; }
   d->queue_open = true;
   pthread_mutex_unlock(&d->lock);
   if (vk_queue_enable_submit_thread(&queue->vk) != VK_SUCCESS) {
      pthread_mutex_lock(&d->lock); d->queue_open = false; pthread_mutex_unlock(&d->lock);
      return -1;
   }
   /* This is a real implicit per-session kernel FIFO reported by GetInfo,
    * not the A740 reference's unverified process-global queue assumption. */
   queue->msm_queue_id = 0;
   return 0;
}
static void queue_close(struct tu_device *dev, struct tu_queue *)
{
   struct dre_device *d = dre(dev);
   pthread_mutex_lock(&d->lock); d->queue_open = false; pthread_mutex_unlock(&d->lock);
}

static VkResult bo_init(struct tu_device *dev, struct vk_object_base *base,
   struct tu_bo **out, uint64_t bytes, uint64_t requested_iova,
   VkMemoryPropertyFlags properties, enum tu_bo_alloc_flags flags,
   struct tu_sparse_vma *lazy, const char *name)
{
   const unsigned allowed = TU_BO_ALLOC_ALLOW_DUMP | TU_BO_ALLOC_GPU_READ_ONLY | TU_BO_ALLOC_INTERNAL_RESOURCE;
   if (!out || requested_iova || lazy || ((unsigned)flags & ~allowed) ||
       (properties & (VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)))
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   *out = NULL;
   struct dre_bo_record *record = (struct dre_bo_record *)calloc(1, sizeof(*record));
   if (!record) return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   struct dre_device *d = dre(dev);
   pthread_mutex_lock(&d->lock);
   if (d->next_bo_id == UINT32_MAX) {
      pthread_mutex_unlock(&d->lock); free(record); return vk_error(dev, VK_ERROR_TOO_MANY_OBJECTS);
   }
   uint32_t access = DRE_GPU_ACCESS_READ |
      ((flags & TU_BO_ALLOC_GPU_READ_ONLY) ? DRE_GPU_ACCESS_EXECUTE : DRE_GPU_ACCESS_WRITE);
   enum dre_darwin_result r = dre_darwin_bo_create(d->native, bytes, access, &record->native);
   if (r != DRE_DARWIN_OK) { pthread_mutex_unlock(&d->lock); free(record); return refusal(dev, r, "BO create"); }
   /* A Mesa sparse-array index is local bookkeeping. Never cast/truncate the
    * opaque kernel64 handle into gem_handle. Local IDs are not reused. */
   uint32_t id = ++d->next_bo_id;
   struct tu_bo *bo = tu_device_lookup_bo(dev, id);
   if (!bo || bo->gem_handle) {
      dre_darwin_bo_close(d->native, record->native);
      pthread_mutex_unlock(&d->lock); free(record); return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   memset(bo, 0, sizeof(*bo));
   bo->gem_handle = id; bo->size = dre_darwin_bo_size(record->native);
   bo->iova = dre_darwin_bo_iova(record->native);
   bo->refcnt = 1; bo->base = base;
   bo->name = tu_debug_bos_add(dev, bo->size, name);
   bo->unique_id = dre_darwin_bo_handle(record->native);
   bo->darwin_handle = bo->unique_id;
   bo->never_unmap = true; /* CPU lease is ended explicitly by BO retirement. */
   record->bo = bo; record->next = d->bos; d->bos = record;
   tu_dump_bo_init(dev, bo); TU_RMV(bo_allocate, dev, bo);
   if (flags & TU_BO_ALLOC_INTERNAL_RESOURCE) {
      TU_RMV(internal_resource_create, dev, bo); TU_RMV(resource_name, dev, bo, name);
   }
   *out = bo;
   pthread_mutex_unlock(&d->lock); return VK_SUCCESS;
}
static VkResult bo_map(struct tu_device *dev, struct tu_bo *bo, void *placed)
{
   if (placed) return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   struct dre_device *d = dre(dev);
   pthread_mutex_lock(&d->lock);
   struct dre_bo_record *record = find_bo(d, bo);
   enum dre_darwin_result r = record ? dre_darwin_bo_map(d->native, record->native, &bo->map) : DRE_DARWIN_INVALID;
   if (r == DRE_DARWIN_OK) TU_RMV(bo_map, dev, bo);
   pthread_mutex_unlock(&d->lock); return refusal(dev, r, "task BO mapping");
}
static void bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   if (!p_atomic_dec_zero(&bo->refcnt)) return;
   struct dre_device *d = dre(dev);
   pthread_mutex_lock(&d->lock);
   struct dre_bo_record **at = &d->bos;
   while (*at && (*at)->bo != bo) at = &(*at)->next;
   if (!*at) { pthread_mutex_unlock(&d->lock); vk_device_set_lost(&dev->vk, "DRE unknown BO retirement"); return; }
   struct dre_bo_record *record = *at;
   enum dre_darwin_result r = dre_darwin_bo_close(d->native, record->native);
   if (r != DRE_DARWIN_OK && r != DRE_DARWIN_BUSY)
      vk_device_set_lost(&dev->vk, "DRE BO retirement: %s", dre_darwin_result_name(r));
   /* BUSY means the transport/native provider retains the allocation and its
    * GPU lease until hardware/TLB retirement, not that its IOVA can be reused. */
   tu_debug_bos_del(dev, bo); tu_dump_bo_del(dev, bo);
   if (bo->map) TU_RMV(bo_unmap, dev, bo);
   TU_RMV(bo_destroy, dev, bo);
   *at = record->next; free(record); memset(bo, 0, sizeof(*bo));
   pthread_mutex_unlock(&d->lock);
}
/* Called by the explicit source overlay in tu_bo_sync_cache. No EL0 DC
 * instruction, hardcoded cache line, successful no-op or Apple cache alias. */
void tu_dre_bo_sync_cache(struct tu_device *dev, struct tu_bo *bo,
   VkDeviceSize offset, VkDeviceSize bytes, enum tu_mem_sync_op op)
{
   struct dre_device *d = dre(dev);
   if(op!=TU_MEM_SYNC_CACHE_TO_GPU&&op!=TU_MEM_SYNC_CACHE_FROM_GPU) {
      vk_device_set_lost(&dev->vk,"DRE unknown cache operation");return;
   }
   if (bytes == VK_WHOLE_SIZE) {
      if (offset > bo->size) { vk_device_set_lost(&dev->vk, "DRE invalid cache range"); return; }
      bytes = bo->size - offset;
   }
   if (!bytes) return;
   pthread_mutex_lock(&d->lock);
   struct dre_bo_record *record = find_bo(d, bo);
   enum dre_darwin_result r = record ? dre_darwin_cache(d->native, record->native, offset, bytes,
      op == TU_MEM_SYNC_CACHE_TO_GPU ? DRE_GPU_CACHE_TO_GPU : DRE_GPU_CACHE_FROM_GPU) : DRE_DARWIN_INVALID;
   pthread_mutex_unlock(&d->lock);
   if (r != DRE_DARWIN_OK)
      vk_device_set_lost(&dev->vk, "DRE cache ownership operation: %s", dre_darwin_result_name(r));
}
static VkResult bo_import(struct tu_device *dev, struct tu_bo **, uint64_t, enum tu_bo_alloc_flags, int)
{ return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE); }
static VkResult bo_host(struct tu_device *dev, struct tu_bo **, void *, uint64_t)
{ return vk_error(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE); }
static int bo_export(struct tu_device *, struct tu_bo *) { return -1; }
static void bo_dump(struct tu_device *, struct tu_bo *) {}
static VkResult sparse_init(struct tu_device *dev, struct vk_object_base *, struct tu_sparse_vma *,
   uint64_t *, enum tu_sparse_vma_flags, uint64_t, uint64_t)
{ return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT); }
static void sparse_finish(struct tu_device *, struct tu_sparse_vma *) {}
static void *submit_create(struct tu_device *dev)
{ return vk_zalloc(&dev->vk.alloc, sizeof(struct dre_submit), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE); }
static void submit_finish(struct tu_device *dev, void *submit) { vk_free(&dev->vk.alloc, submit); }
static void submit_entries(struct tu_device *dev, void *data, struct tu_cs_entry *entries, unsigned count)
{
   struct dre_submit *submit = (struct dre_submit *)data;
   struct dre_device *d = dre(dev);
   pthread_mutex_lock(&d->lock);
   for (unsigned i = 0; i < count; ++i) {
      struct dre_bo_record *r = find_bo(d, entries[i].bo);
      if (!r || submit->ib_count == DRE_GPU_MAX_IBS || !entries[i].size ||
          ((entries[i].offset|entries[i].size)&3u) ||
          !(dre_darwin_bo_flags(r->native)&DRE_GPU_ACCESS_EXECUTE) ||
          entries[i].offset > entries[i].bo->size || entries[i].size > entries[i].bo->size - entries[i].offset) {
         submit->overflow = true; break;
      }
      struct dre_gpu_submit_ib *ib = &submit->ibs[submit->ib_count++];
      ib->bo = dre_darwin_bo_handle(r->native); ib->offset = entries[i].offset; ib->bytes = entries[i].size;
   }
   pthread_mutex_unlock(&d->lock);
}
static void submit_bind(struct tu_device *, void *data, struct tu_sparse_vma *, uint64_t,
   struct tu_bo *, uint64_t, uint64_t)
{ ((struct dre_submit *)data)->overflow = true; }

/* Sync fields are serialized by dev->submit_mutex, the same lock held by
 * tu_queue.cc while entering queue_submit. Condition waits release that lock.
 * Kernel fence references are retained across snapshots/moves and CPU waits. */
static VkResult sync_init(struct vk_device *, struct vk_sync *base, uint64_t initial)
{
   struct dre_sync *s = container_of(base, struct dre_sync, base);
   s->state = initial ? DRE_SYNC_HOST_SIGNALED : DRE_SYNC_UNSIGNALED; s->fence = 0;
   return VK_SUCCESS;
}
static void sync_finish(struct vk_device *vk, struct vk_sync *base)
{
   struct tu_device *dev = container_of(vk, struct tu_device, vk);
   struct dre_sync *s = container_of(base, struct dre_sync, base);
   if (s->fence && dre_darwin_fence_release(dre(dev)->native, s->fence)!=DRE_DARWIN_OK)
      vk_device_set_lost(vk,"DRE sync destruction could not collect native retirement");
   s->fence=0;s->state=DRE_SYNC_UNSIGNALED;
}
static VkResult sync_reset(struct vk_device *vk, struct vk_sync *base)
{
   struct tu_device *dev = container_of(vk, struct tu_device, vk);
   struct dre_sync *s = container_of(base, struct dre_sync, base);
   pthread_mutex_lock(&dev->submit_mutex);
   uint64_t old=s->fence;s->fence=0;s->state=DRE_SYNC_UNSIGNALED;
   enum dre_darwin_result r=old?dre_darwin_fence_release(dre(dev)->native,old):DRE_DARWIN_OK;
   pthread_mutex_unlock(&dev->submit_mutex);
   return r==DRE_DARWIN_OK?VK_SUCCESS:vk_device_set_lost(&dev->vk,
      "DRE reset native sync payload: %s",dre_darwin_result_name(r));
}
static VkResult sync_signal(struct vk_device *vk, struct vk_sync *base, uint64_t)
{
   struct tu_device *dev = container_of(vk, struct tu_device, vk);
   struct dre_sync *s = container_of(base, struct dre_sync, base);
   pthread_mutex_lock(&dev->submit_mutex);
   uint64_t old=s->fence;s->fence=0;s->state=DRE_SYNC_UNSIGNALED;
   enum dre_darwin_result r=old?dre_darwin_fence_release(dre(dev)->native,old):DRE_DARWIN_OK;
   if(r!=DRE_DARWIN_OK) {
      pthread_mutex_unlock(&dev->submit_mutex);return vk_device_set_lost(&dev->vk,
         "DRE replace native sync payload: %s",dre_darwin_result_name(r));
   }
   s->state = DRE_SYNC_HOST_SIGNALED;
   pthread_cond_broadcast(&dev->timeline_cond);
   pthread_mutex_unlock(&dev->submit_mutex); return VK_SUCCESS;
}
static VkResult sync_move(struct vk_device *vk, struct vk_sync *dst, struct vk_sync *src)
{
   struct tu_device *dev = container_of(vk, struct tu_device, vk);
   struct dre_sync *to = container_of(dst, struct dre_sync, base), *from = container_of(src, struct dre_sync, base);
   if (to == from) return VK_SUCCESS;
   pthread_mutex_lock(&dev->submit_mutex);
   uint64_t old=to->fence;to->fence=0;to->state=DRE_SYNC_UNSIGNALED;
   enum dre_darwin_result r=old?dre_darwin_fence_release(dre(dev)->native,old):DRE_DARWIN_OK;
   if(r!=DRE_DARWIN_OK) {
      pthread_mutex_unlock(&dev->submit_mutex);return vk_device_set_lost(&dev->vk,
         "DRE move native sync payload: %s",dre_darwin_result_name(r));
   }
   to->state = from->state; to->fence = from->fence;
   from->state = DRE_SYNC_UNSIGNALED; from->fence = 0;
   pthread_cond_broadcast(&dev->timeline_cond);
   pthread_mutex_unlock(&dev->submit_mutex); return VK_SUCCESS;
}
static int pending_wait(struct tu_device *dev, struct dre_sync *s, uint64_t deadline)
{
   while (s->state == DRE_SYNC_UNSIGNALED) {
      uint64_t now = os_time_get_nano();
      if (deadline != UINT64_MAX && now >= deadline) return ETIMEDOUT;
      struct timespec realtime;
      if (clock_gettime(CLOCK_REALTIME, &realtime)) return EIO;
      uint64_t relative = deadline == UINT64_MAX ? 100000000ull : MIN2(deadline - now, 100000000ull);
      uint64_t ns = (uint64_t)realtime.tv_nsec + relative;
      realtime.tv_sec += ns / 1000000000ull; realtime.tv_nsec = ns % 1000000000ull;
      int e = pthread_cond_timedwait(&dev->timeline_cond, &dev->submit_mutex, &realtime);
      if (e && e != ETIMEDOUT) return e;
      if (vk_device_is_lost(&dev->vk)) return EIO;
   }
   return 0;
}
static VkResult sync_wait(struct vk_device *vk, struct vk_sync *base, uint64_t,
   enum vk_sync_wait_flags flags, uint64_t deadline)
{
   struct tu_device *dev = container_of(vk, struct tu_device, vk);
   struct dre_sync *s = container_of(base, struct dre_sync, base);
   pthread_mutex_lock(&dev->submit_mutex);
   int e = pending_wait(dev, s, deadline);
   if (e) { pthread_mutex_unlock(&dev->submit_mutex); return e == ETIMEDOUT ? VK_TIMEOUT : VK_ERROR_DEVICE_LOST; }
   if ((flags & VK_SYNC_WAIT_PENDING) || s->state == DRE_SYNC_HOST_SIGNALED) {
      pthread_mutex_unlock(&dev->submit_mutex); return VK_SUCCESS;
   }
   uint64_t fence = s->fence;
   enum dre_darwin_result r = dre_darwin_fence_retain(dre(dev)->native, fence);
   pthread_mutex_unlock(&dev->submit_mutex);
   if (r != DRE_DARWIN_OK) return refusal(dev, r, "retain sync fence");
   VkResult result = wait_real(dev, fence, deadline);
   dre_darwin_fence_release(dre(dev)->native, fence);
   return result;
}
static const struct vk_sync_type sync_type = {
   .size = sizeof(struct dre_sync),
   .features = (enum vk_sync_features)(VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_GPU_WAIT |
      VK_SYNC_FEATURE_GPU_MULTI_WAIT | VK_SYNC_FEATURE_CPU_WAIT | VK_SYNC_FEATURE_CPU_RESET |
      VK_SYNC_FEATURE_CPU_SIGNAL | VK_SYNC_FEATURE_WAIT_PENDING),
   .init = sync_init, .finish = sync_finish, .signal = sync_signal,
   .reset = sync_reset, .move = sync_move, .wait = sync_wait,
};

/* pending_wait releases dev->submit_mutex. Earlier resolved waits must hold
 * their own native reference before another wait can sleep or a CPU move/reset
 * can replace a payload. Device teardown drains queue workers before KMD close.
 * No new GPU fence is synthesized and no kernel ABI extension is involved. */
struct dre_wait_refs {
   struct tu_device *dev;
   uint64_t values[DRE_GPU_MAX_WAITS];
   uint32_t count;
   explicit dre_wait_refs(struct tu_device *device):dev(device),values{},count(0) {}
   dre_wait_refs(const dre_wait_refs&)=delete;
   dre_wait_refs& operator=(const dre_wait_refs&)=delete;
   ~dre_wait_refs() {
      for(uint32_t i=0;i<count;i++)
         if(dre_darwin_fence_release(dre(dev)->native,values[i])!=DRE_DARWIN_OK)
            vk_device_set_lost(&dev->vk,"DRE native wait snapshot retirement failed");
   }
   enum dre_darwin_result append(uint64_t value) {
      for(uint32_t i=0;i<count;i++)if(values[i]==value)return DRE_DARWIN_OK;
      if(count==DRE_GPU_MAX_WAITS)return DRE_DARWIN_UNSUPPORTED;
      enum dre_darwin_result r=dre_darwin_fence_retain(dre(dev)->native,value);
      if(r==DRE_DARWIN_OK)values[count++]=value;
      return r;
   }
};

static VkResult queue_submit(struct tu_queue *queue, void *data,
   struct vk_sync_wait *waits, uint32_t wait_count, struct vk_sync_signal *signals,
   uint32_t signal_count, struct tu_u_trace_submission_data *trace)
{
   struct tu_device *dev = queue->device;
   struct dre_device *d = dre(dev);
   struct dre_submit *submit = (struct dre_submit *)data;
   if (submit->overflow || trace)
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   dre_wait_refs held_waits(dev);
   for (uint32_t i = 0; i < wait_count; ++i) {
      if (waits[i].sync->type != &sync_type)
         return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
      struct dre_sync *s = container_of(waits[i].sync, struct dre_sync, base);
      if (pending_wait(dev, s, UINT64_MAX)) return VK_ERROR_DEVICE_LOST;
      if (s->state == DRE_SYNC_FENCE) {
         enum dre_darwin_result r=held_waits.append(s->fence);
         if(r!=DRE_DARWIN_OK)return refusal(dev,r,"retain native semaphore wait snapshot");
      }
   }
   for (uint32_t i = 0; i < signal_count; ++i)
      if (signals[i].sync->type != &sync_type)
         return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   struct dre_gpu_submit_resource resources[DRE_GPU_MAX_BOS] = {};
   uint32_t resource_count = 0;
   pthread_mutex_lock(&d->lock);
   /* Conservatively pin every currently live session BO: Turnip command
    * streams reference shader/data/descriptor BOs indirectly. IB-only pins
    * would permit data resources to disappear during execution. This bounded
    * first transport refuses a >64-BO device instead of silently omitting any.
    * Per-submit precise residency is a later optimization, not a safety waiver. */
   for (struct dre_bo_record *r = d->bos; r; r = r->next) {
      if (resource_count == DRE_GPU_MAX_BOS) { pthread_mutex_unlock(&d->lock); return vk_error(dev, VK_ERROR_TOO_MANY_OBJECTS); }
      resources[resource_count].bo = dre_darwin_bo_handle(r->native);
      resources[resource_count++].access = dre_darwin_bo_flags(r->native) &
         (DRE_GPU_ACCESS_READ | DRE_GPU_ACCESS_WRITE);
   }
   uint64_t fence = 0;
   enum dre_darwin_result r = DRE_DARWIN_OK;
   if (submit->ib_count) {
      if (d->last_cookie == INT_MAX) { pthread_mutex_unlock(&d->lock); return vk_error(dev, VK_ERROR_TOO_MANY_OBJECTS); }
      r = dre_darwin_submit(d->native, resources, resource_count, submit->ibs, submit->ib_count,
         held_waits.values, held_waits.count, &fence);
   } else {
      /* An empty submit still obeys external semaphore dependencies. The
       * threaded worker can wait for those actual fences without blocking the
       * application. No new native fence or successful GPU work is fabricated. */
      for (uint32_t i = 0; r == DRE_DARWIN_OK && i < held_waits.count; ++i)
         r = dre_darwin_fence_wait(d->native, held_waits.values[i], UINT64_MAX);
      if (r == DRE_DARWIN_OK && d->last_fence) {
         fence = d->last_fence; r = dre_darwin_fence_retain(d->native, fence);
      }
   }
   if (r != DRE_DARWIN_OK) {
      pthread_mutex_unlock(&d->lock);
      /* A normal Vulkan submit cannot be half-published or reported successful
       * when the native FIFO/resource limit rejects it. Busy/LIMIT is visible;
       * a future bounded oldest-fence retry may be added after real tests. */
      /* A native error may occur after publication. Drop this user reference,
       * but retain the pending native retirement record and BO pins. */
      if (fence) dre_darwin_fence_release(d->native, fence);
      return r == DRE_DARWIN_BUSY ? vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY) : refusal(dev, r, "native atomic submit");
   }
   if (d->last_fence) dre_darwin_fence_release(d->native, d->last_fence);
   d->last_fence = fence;
   if (submit->ib_count) queue->fence = (int)++d->last_cookie;
   for (uint32_t i = 0; i < signal_count; ++i) {
      struct dre_sync *s = container_of(signals[i].sync, struct dre_sync, base);
      if (fence && dre_darwin_fence_retain(d->native, fence) != DRE_DARWIN_OK) {
         pthread_mutex_unlock(&d->lock); return vk_device_set_lost(&dev->vk, "DRE signal fence retain failed");
      }
      if (s->fence) dre_darwin_fence_release(d->native, s->fence);
      s->fence = fence;
      s->state = fence ? DRE_SYNC_FENCE : DRE_SYNC_HOST_SIGNALED;
   }
   pthread_mutex_unlock(&d->lock);
   return VK_SUCCESS;
}
static VkResult queue_wait(struct tu_queue *queue, uint32_t cookie, uint64_t timeout)
{
   struct tu_device *dev = queue->device; struct dre_device *d = dre(dev);
   pthread_mutex_lock(&d->lock);
   /* Generic Turnip has a 32-bit debug fence field. It holds a local cookie,
    * never the truncated native64 value. Only the latest debug wait is
    * supported; u_trace is explicitly refused above instead of aliasing an old
    * cookie to a different native job. Vulkan syncs retain full64 fences. */
   if (!cookie || cookie != d->last_cookie || !d->last_fence) {
      pthread_mutex_unlock(&d->lock); return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
   }
   uint64_t fence = d->last_fence;
   enum dre_darwin_result r = dre_darwin_fence_retain(d->native, fence);
   pthread_mutex_unlock(&d->lock);
   if (r != DRE_DARWIN_OK) return refusal(dev, r, "queue fence retain");
   r = dre_darwin_fence_wait(d->native, fence, timeout);
   dre_darwin_fence_release(d->native, fence);
   return refusal(dev, r, "queue fence wait");
}
static const struct tu_knl functions = {
   .name = "dre-darwin",
   .device_init = device_init, .device_finish = device_finish,
   .device_get_gpu_timestamp = timestamp, .device_get_suspend_count = suspend_count,
   .device_check_status = check_status, .submitqueue_new = queue_new, .submitqueue_close = queue_close,
   .bo_init = bo_init, .bo_init_dmabuf = bo_import, .bo_init_host = bo_host,
   .bo_export_dmabuf = bo_export, .bo_map = bo_map, .bo_allow_dump = bo_dump, .bo_finish = bo_finish,
   .submit_create = submit_create, .submit_finish = submit_finish, .submit_add_entries = submit_entries,
   .submit_add_bind = submit_bind, .queue_submit = queue_submit, .queue_wait_fence = queue_wait,
   .sparse_vma_init = sparse_init, .sparse_vma_finish = sparse_finish,
};

VkResult tu_knl_darwin_load(struct tu_instance *instance)
{
   /* The reference A740 trace uses private selector0x7102 and render-server
    * boot overrides, neither of which exists in DRE ABI1. Refuse a requested
    * trace instead of manufacturing a successful diagnostic submission. The
    * DRE source profile compiles out that reference-only diagnostic branch;
    * ordinary compute/render work still goes through real Turnip submits. */
   if (getenv("SHENG_GPU_TRACE") != NULL)
      return vk_startup_errorf(instance, VK_ERROR_FEATURE_NOT_PRESENT,
                               "DRE has no legacy A740 frame-trace transport");
   struct dre_darwin *native = NULL;
   enum dre_darwin_result r = dre_darwin_open(DRE_GPU_VULKAN_REQUIRED_CAPS, &native);
   if (r == DRE_DARWIN_NO_PROVIDER || r == DRE_DARWIN_UNSUPPORTED)
      return VK_ERROR_INCOMPATIBLE_DRIVER; /* No invented physical device. */
   if (r != DRE_DARWIN_OK)
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED, "DRE: %s", dre_darwin_result_name(r));
   struct dre_gpu_info_reply info = *dre_darwin_info(native);
   dre_darwin_close(native);
   if (!info.gmem_bytes || !info.gmem_base || !info.uche_trap_base ||
       !info.highest_bank_bit || info.macrotile_mode > 1)
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED, "DRE native GMEM/UBWC facts unavailable");
   struct tu_physical_device *p = (struct tu_physical_device *)
      vk_zalloc(&instance->vk.alloc, sizeof(*p), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!p) return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   p->instance = instance; p->local_fd = -1; p->master_fd = -1; p->kgsl_dma_fd = -1;
   snprintf(p->fd_path, sizeof(p->fd_path), "DREAdrenoGPU");
   p->dev_id.chip_id = info.chip_id; p->dev_id.gpu_id = 619;
   p->dre_reset_generation = info.reset_generation;
   p->gmem_size = info.gmem_bytes; p->gmem_base = info.gmem_base; p->uche_trap_base = info.uche_trap_base;
   p->va_start = info.gpu_va_base; p->va_size = info.gpu_va_bytes;
   p->has_set_iova = false; p->has_lazy_bos = false; p->has_vm_bind = false;
   p->has_sparse = false; p->has_sparse_prr = false; p->has_raytracing = false;
   p->has_preemption = false; p->is_perf_cntr_selectable = false;
   p->has_cached_coherent_memory = false; p->has_cached_non_coherent_memory = false;
   p->has_host_import = false; p->submitqueue_priority_count = 1;
   p->ubwc_config.highest_bank_bit = info.highest_bank_bit;
   p->ubwc_config.bank_swizzle_levels = info.ubwc_swizzle;
   p->ubwc_config.macrotile_mode = info.macrotile_mode ? FDL_MACROTILE_8_CHANNEL : FDL_MACROTILE_4_CHANNEL;
   p->timeline_type = vk_sync_timeline_get_type(&sync_type);
   p->sync_types[0] = &sync_type; p->sync_types[1] = &p->timeline_type.sync; p->sync_types[2] = NULL;
   p->heap.size = info.task_max_bytes; p->heap.used = 0; p->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
   instance->knl = &functions;
   VkResult result = tu_physical_device_init(p, instance);
   if (result != VK_SUCCESS) { vk_free(&instance->vk.alloc, p); return result; }
   list_addtail(&p->vk.link, &instance->vk.physical_devices.list);
   return VK_SUCCESS;
}
