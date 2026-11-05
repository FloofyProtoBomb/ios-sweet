/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source draft. No compile, test or native execution.
 * Direct ABI1 DREAdrenoGPU transport. No AGX tunnel, DRM fd, fake Vulkan
 * physical device, MMIO from EL0, caller physical address or guessed fence.
 */
#define _POSIX_C_SOURCE 200809L
#include "dre_darwin.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#endif

struct dre_darwin_bo {
    struct dre_darwin_bo *next;
    uint64_t handle, iova, bytes, cpu_lease, cpu_epoch, last_cpu_epoch;
    void *cpu;
    uint32_t access;
    int retired, persistent;
};
struct dre_local_fence {
    struct dre_local_fence *next;
    uint64_t value;
    uint32_t references;
};
struct dre_darwin_surface {
    struct dre_darwin_surface *next;
    uint64_t token;
    uint32_t access;
    int retired, ready;
    struct dre_gpu_surface_description description;
};
struct dre_darwin {
    pthread_mutex_t mutex;
    uint32_t connection;
    struct dre_gpu_info_reply info;
    struct dre_darwin_bo *bos;
    struct dre_local_fence *fences;
    struct dre_darwin_surface *surfaces;
    uint32_t fence_count,surface_count;
    uint64_t last_fence,last_surface;
    int poisoned,last_reply_typed;
};

static uint64_t monotonic_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000)
        return UINT64_MAX;
    if ((uint64_t)t.tv_sec > UINT64_MAX / 1000000000ull)
        return UINT64_MAX;
    uint64_t base = (uint64_t)t.tv_sec * 1000000000ull;
    return base > UINT64_MAX - (uint64_t)t.tv_nsec ? UINT64_MAX : base + (uint64_t)t.tv_nsec;
}
static void request(struct dre_darwin *c, struct dre_gpu_request_header *h, size_t bytes)
{
    h->abi_version = DRE_GPU_ABI_VERSION;
    h->struct_bytes = (uint32_t)bytes;
    h->session_generation = c->info.header.session_generation;
}
static enum dre_darwin_result protocol(struct dre_darwin *c)
{
    c->poisoned = 1;
    return DRE_DARWIN_PROTOCOL;
}
static enum dre_darwin_result status_code(int32_t code)
{
    switch (code) {
    case DRE_GPU_OK: return DRE_DARWIN_OK;
    case DRE_GPU_UNSUPPORTED: return DRE_DARWIN_UNSUPPORTED;
    case DRE_GPU_INVALID: return DRE_DARWIN_INVALID;
    case DRE_GPU_NOT_READY: return DRE_DARWIN_NO_PROVIDER;
    case DRE_GPU_NO_MEMORY: case DRE_GPU_LIMIT: return DRE_DARWIN_NO_MEMORY;
    case DRE_GPU_BUSY: return DRE_DARWIN_BUSY;
    case DRE_GPU_TIMEOUT: return DRE_DARWIN_TIMEOUT;
    case DRE_GPU_STALE: case DRE_GPU_FAULT: case DRE_GPU_CLOSED: return DRE_DARWIN_LOST;
    default: return DRE_DARWIN_PROTOCOL;
    }
}
static enum dre_darwin_result call(struct dre_darwin *c, uint32_t method,
    const void *in, size_t in_bytes, void *out, size_t out_bytes, int cleanup)
{
    if(c)c->last_reply_typed=0;
    if (!c || !c->connection || (c->poisoned && !cleanup))
        return DRE_DARWIN_LOST;
    memset(out, 0, out_bytes);
#if defined(__APPLE__)
    size_t returned = out_bytes;
    kern_return_t kr = IOConnectCallStructMethod((io_connect_t)c->connection,
        method, in, in_bytes, out, &returned);
    if (kr != KERN_SUCCESS) {
        /* The versioned native method carries errors in the typed reply. A
         * Mach/IOKit transport failure makes hardware outcome uncertain. */
        c->poisoned = 1;
        return DRE_DARWIN_LOST;
    }
    if (returned != out_bytes || out_bytes < sizeof(struct dre_gpu_reply_header))
        return protocol(c);
#else
    (void)method; (void)in; (void)in_bytes;
    return DRE_DARWIN_UNSUPPORTED;
#endif
    const struct dre_gpu_reply_header *h = (const struct dre_gpu_reply_header *)out;
    if (h->abi_version != DRE_GPU_ABI_VERSION || h->struct_bytes != out_bytes || h->flags ||
        !h->session_generation || (method != DRE_GPU_GET_INFO &&
        h->session_generation != c->info.header.session_generation))
        return protocol(c);
    enum dre_darwin_result r = status_code(h->status);
    if(r!=DRE_DARWIN_PROTOCOL)c->last_reply_typed=1;
    if (r == DRE_DARWIN_PROTOCOL || r == DRE_DARWIN_LOST)
        c->poisoned = 1;
    return r;
}
static struct dre_darwin_bo *owned_bo(struct dre_darwin *c, const struct dre_darwin_bo *bo)
{
    for (struct dre_darwin_bo *b = c->bos; b; b = b->next)
        if (b == bo) return b;
    return NULL;
}
static struct dre_darwin_bo *bo_handle(struct dre_darwin *c, uint64_t handle)
{
    for (struct dre_darwin_bo *b = c->bos; b; b = b->next)
        if (b->handle == handle && !b->retired) return b;
    return NULL;
}
static struct dre_local_fence *fence_handle(struct dre_darwin *c, uint64_t value)
{
    for (struct dre_local_fence *f = c->fences; f; f = f->next)
        if (f->value == value) return f;
    return NULL;
}
static enum dre_darwin_result handle_call(struct dre_darwin *c, uint32_t method,
    uint64_t handle, int cleanup)
{
    struct dre_gpu_handle_request req = {0};
    struct dre_gpu_status_reply reply;
    request(c, &req.header, sizeof(req)); req.handle = handle;
    return call(c, method, &req, sizeof(req), &reply, sizeof(reply), cleanup);
}
static enum dre_darwin_result cpu_access_locked(struct dre_darwin *c, struct dre_darwin_bo *b, int begin)
{
    if (!b->persistent || !b->cpu_lease ||
        !(c->info.capabilities & DRE_GPU_CAP_PERSISTENT_CPU_MAP)) return DRE_DARWIN_UNSUPPORTED;
    if (begin && c->poisoned) return DRE_DARWIN_LOST;
    if (begin && b->cpu_epoch) return DRE_DARWIN_OK; /* Retain this access epoch, not a cache no-op. */
    if (!begin && !b->cpu_epoch) return DRE_DARWIN_INVALID;
    struct dre_gpu_cpu_access_request req = {0};
    request(c, &req.header, sizeof(req)); req.bo = b->handle; req.map_lease = b->cpu_lease;
    req.epoch = begin ? 0 : b->cpu_epoch; req.access = begin ? DRE_GPU_CPU_READ | DRE_GPU_CPU_WRITE : 0;
    if (begin) {
        struct dre_gpu_cpu_access_reply reply = {0};
        enum dre_darwin_result r = call(c, DRE_GPU_CPU_ACCESS_BEGIN, &req, sizeof(req), &reply, sizeof(reply), 0);
        if (r == DRE_DARWIN_OK && !reply.epoch) return protocol(c);
        if (c->last_reply_typed && reply.epoch) {
            if(reply.epoch<=b->last_cpu_epoch)return protocol(c);
            b->cpu_epoch = reply.epoch; b->last_cpu_epoch = reply.epoch;
            if(r!=DRE_DARWIN_OK) {c->poisoned=1;r=DRE_DARWIN_LOST;}
        }
        return r;
    }
    struct dre_gpu_status_reply reply;
    enum dre_darwin_result r = call(c, DRE_GPU_CPU_ACCESS_END, &req, sizeof(req), &reply, sizeof(reply), 1);
    if (r == DRE_DARWIN_OK) b->cpu_epoch = 0;
    return r;
}
static enum dre_darwin_result unmap_locked(struct dre_darwin *c, struct dre_darwin_bo *b)
{
    if (!b->cpu_lease) return DRE_DARWIN_OK;
    if (b->persistent && b->cpu_epoch) {
        enum dre_darwin_result ended = cpu_access_locked(c, b, 0);
        if (ended != DRE_DARWIN_OK) return ended;
    }
    enum dre_darwin_result r = handle_call(c, DRE_GPU_CPU_UNMAP, b->cpu_lease, 1);
    if (r == DRE_DARWIN_OK) { b->cpu = NULL; b->cpu_lease = 0; }
    return r;
}
static enum dre_darwin_result fence_reply(struct dre_darwin *c, uint64_t fence,
    uint32_t method, uint64_t timeout, int *complete)
{
    struct dre_gpu_fence_request req = {0};
    struct dre_gpu_fence_reply reply;
    request(c, &req.header, sizeof(req)); req.fence = fence; req.timeout_ns = timeout;
    *complete = 0;
    /* A poisoned client may still observe/retire a known published fence.
     * This does not permit new allocations, maps, cache ownership or submits. */
    enum dre_darwin_result r = call(c, method, &req, sizeof(req), &reply, sizeof(reply), 1);
    if (r != DRE_DARWIN_OK && r != DRE_DARWIN_TIMEOUT) return r;
    if (reply.fence != fence || reply.reset_generation != c->info.reset_generation)
        return protocol(c);
    if (reply.state == DRE_GPU_FENCE_ERROR || reply.state == DRE_GPU_FENCE_RESET || reply.hardware_error) {
        c->poisoned = 1; return DRE_DARWIN_LOST;
    }
    if (reply.state == DRE_GPU_FENCE_PENDING)
        return method == DRE_GPU_WAIT_FENCE ? DRE_DARWIN_TIMEOUT : DRE_DARWIN_OK;
    if (reply.state != DRE_GPU_FENCE_COMPLETE || r == DRE_DARWIN_TIMEOUT)
        return protocol(c);
    *complete = 1;
    return DRE_DARWIN_OK;
}
/* Keep uncertain native objects in the connection's ownership list. No
 * failed close, pending fence or timeout permits address/page reuse. */
static enum dre_darwin_result collect_locked(struct dre_darwin *c)
{
    for (struct dre_darwin_surface **at = &c->surfaces; *at;) {
        struct dre_darwin_surface *s = *at;
        if (!s->retired) { at = &s->next; continue; }
        enum dre_darwin_result r = handle_call(c, DRE_GPU_SURFACE_CLOSE, s->token, 1);
        if (r == DRE_DARWIN_BUSY) { at = &s->next; continue; }
        if (r != DRE_DARWIN_OK) return r; /* backing remains retained by kernel */
        *at = s->next; free(s); --c->surface_count;
    }
    for (struct dre_darwin_bo **at = &c->bos; *at;) {
        struct dre_darwin_bo *b = *at;
        if (!b->retired) { at = &b->next; continue; }
        enum dre_darwin_result r = unmap_locked(c, b);
        if (r != DRE_DARWIN_OK) return r;
        r = handle_call(c, DRE_GPU_BO_CLOSE, b->handle, 1);
        if (r == DRE_DARWIN_BUSY) { at = &b->next; continue; }
        if (r != DRE_DARWIN_OK) return r;
        *at = b->next; free(b);
    }
    for (struct dre_local_fence **at = &c->fences; *at;) {
        struct dre_local_fence *f = *at;
        if (f->references) { at = &f->next; continue; }
        enum dre_darwin_result r = handle_call(c, DRE_GPU_FENCE_CLOSE, f->value, 1);
        if (r == DRE_DARWIN_BUSY) { at = &f->next; continue; }
        if (r != DRE_DARWIN_OK) return r;
        *at = f->next; free(f); --c->fence_count;
    }
    return DRE_DARWIN_OK;
}

enum dre_darwin_result dre_darwin_open(uint64_t required, struct dre_darwin **out)
{
    if (!out) return DRE_DARWIN_INVALID;
    *out = NULL;
#if !defined(__APPLE__)
    (void)required;
    return DRE_DARWIN_NO_PROVIDER;
#else
    struct dre_darwin *c = calloc(1, sizeof(*c));
    if (!c) return DRE_DARWIN_NO_MEMORY;
    if (pthread_mutex_init(&c->mutex, NULL)) { free(c); return DRE_DARWIN_NO_MEMORY; }
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(DRE_GPU_SERVICE));
    if (!service) { pthread_mutex_destroy(&c->mutex); free(c); return DRE_DARWIN_NO_PROVIDER; }
    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || !connection) {
        pthread_mutex_destroy(&c->mutex); free(c); return DRE_DARWIN_NO_PROVIDER;
    }
    c->connection = connection;
    struct dre_gpu_info_request req = {0};
    request(c, &req.header, sizeof(req));
    enum dre_darwin_result r = call(c, DRE_GPU_GET_INFO, &req, sizeof(req), &c->info, sizeof(c->info), 0);
    if (r == DRE_DARWIN_OK) {
        const struct dre_gpu_info_reply *p = &c->info;
        if ((p->capabilities & required) != required)
            r = DRE_DARWIN_UNSUPPORTED;
        else if ((required || p->capabilities) && (p->chip_id != 0x06010900u || p->cpu_page_bytes != DRE_GPU_CPU_PAGE_BYTES ||
            p->gpu_page_bytes != DRE_GPU_PAGE_BYTES || !p->gpu_va_bytes ||
            p->gpu_va_base > UINT64_MAX - p->gpu_va_bytes ||
            (p->gpu_va_base | p->gpu_va_bytes) % DRE_GPU_PAGE_BYTES ||
            !p->bo_max_bytes || p->bo_max_bytes > DRE_GPU_BO_MAX_BYTES ||
            !p->task_max_bytes || p->task_max_bytes > DRE_GPU_TASK_MAX_BYTES ||
            p->bo_max_bytes > p->task_max_bytes || !p->max_bos || p->max_bos > DRE_GPU_MAX_BOS ||
            !p->max_ibs || p->max_ibs > DRE_GPU_MAX_IBS || p->max_waits > DRE_GPU_MAX_WAITS ||
            !p->max_fences || p->max_fences > DRE_GPU_MAX_FENCES ||
            (p->cache_contract != DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE &&
             p->cache_contract != DRE_GPU_CACHE_OWNED_NORMAL_NC_PERSISTENT)))
            r = protocol(c);
        else if ((required & DRE_GPU_CAP_PERSISTENT_CPU_MAP) &&
                 p->cache_contract != DRE_GPU_CACHE_OWNED_NORMAL_NC_PERSISTENT)
            r = DRE_DARWIN_UNSUPPORTED;
    }
    if (r != DRE_DARWIN_OK) { dre_darwin_close(c); return r; }
    *out = c;
    return DRE_DARWIN_OK;
#endif
}
const struct dre_gpu_info_reply *dre_darwin_info(const struct dre_darwin *c)
{ return c ? &c->info : NULL; }

static struct dre_darwin_surface *owned_surface(struct dre_darwin *c,
    const struct dre_darwin_surface *surface)
{
    for (struct dre_darwin_surface *s = c->surfaces; s; s = s->next)
        if (s == surface) return s;
    return NULL;
}
static int zero_data(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; ++i) if (p[i]) return 0;
    return 1;
}
static int surface_description_valid(const struct dre_gpu_surface_description *d)
{
    /* Structural ABI checks only. These never qualify cache/VM ownership. */
    if (!d->allocation_bytes || d->allocation_bytes > DRE_GPU_BO_MAX_BYTES ||
        d->allocation_bytes % DRE_GPU_CPU_PAGE_BYTES || !d->surface_id ||
        !d->width || !d->height || d->width > 16384 || d->height > 16384 ||
        !d->plane_count || d->plane_count > DRE_GPU_MAX_SURFACE_PLANES ||
        (d->pin_flags & ~7u) || !(d->pin_flags & DRE_GPU_SURFACE_RETAINED) ||
        !d->gpu_import_refusals || (d->gpu_import_refusals & ~255ull)) return 0;
    if (d->descriptor_kind == 1) {
        if (d->pin_flags != 7u || d->page_count != d->allocation_bytes / DRE_GPU_PAGE_BYTES) return 0;
    } else if (d->descriptor_kind == 2) {
        if (d->pin_flags != DRE_GPU_SURFACE_RETAINED || d->page_count ||
            !(d->gpu_import_refusals & DRE_GPU_SURFACE_PHYSICAL_BACKING)) return 0;
    } else return 0;
    for (uint32_t i = 0; i < d->plane_count; ++i) {
        const struct dre_gpu_surface_plane *p = &d->planes[i];
        if (!p->width || !p->height || p->width > 16384 || p->height > 16384 ||
            !p->row_bytes || !p->bytes || !p->bytes_per_element || p->bytes_per_element > 64 ||
            !p->element_width || p->element_width > 255 || !p->element_height || p->element_height > 255 ||
            p->offset > d->allocation_bytes || p->bytes > d->allocation_bytes - p->offset) return 0;
        uint64_t columns = ((uint64_t)p->width + p->element_width - 1) / p->element_width;
        uint64_t rows = ((uint64_t)p->height + p->element_height - 1) / p->element_height;
        if (columns * p->bytes_per_element > p->row_bytes || rows * p->row_bytes > p->bytes) return 0;
    }
    return zero_data(&d->planes[d->plane_count],
        (DRE_GPU_MAX_SURFACE_PLANES - d->plane_count) * sizeof(d->planes[0]));
}
static int surface_reply_owned(struct dre_darwin *c, const struct dre_gpu_surface_reply *r)
{
    const struct dre_gpu_reply_header *h = &r->header;
    return c->last_reply_typed && h->abi_version == DRE_GPU_ABI_VERSION && h->struct_bytes == sizeof(*r) &&
        h->session_generation == c->info.header.session_generation && !h->flags &&
        status_code(h->status) != DRE_DARWIN_PROTOCOL && r->surface_token;
}
enum dre_darwin_result dre_darwin_surface_retain_port(struct dre_darwin *c,
    uint32_t port, uint32_t access, struct dre_darwin_surface **out)
{
    if (!c || !out || !port || !(access & DRE_GPU_ACCESS_READ) || (access & ~3u))
        return DRE_DARWIN_INVALID;
    *out = NULL;
    struct dre_darwin_surface *s = calloc(1, sizeof(*s));
    if (!s) return DRE_DARWIN_NO_MEMORY;
    pthread_mutex_lock(&c->mutex);
    enum dre_darwin_result r = collect_locked(c);
    struct dre_gpu_surface_retain_request req = {0};
    struct dre_gpu_surface_reply reply = {0};
    request(c, &req.header, sizeof(req)); req.mach_port_name = port; req.access = access;
    if (r == DRE_DARWIN_OK && c->surface_count >= DRE_GPU_MAX_SURFACES) r = DRE_DARWIN_NO_MEMORY;
    if (r == DRE_DARWIN_OK) r = call(c, DRE_GPU_SURFACE_RETAIN, &req, sizeof(req), &reply, sizeof(reply), 0);
    if (surface_reply_owned(c, &reply)) {
        int collision = 0;
        for (struct dre_darwin_surface *p = c->surfaces; p; p = p->next)
            if (p->token == reply.surface_token) collision = 1;
        uint32_t slot = (uint32_t)(reply.surface_token & 255u);
        if (collision || !slot || slot > DRE_GPU_MAX_SURFACES ||
            reply.surface_token <= c->last_surface) r = protocol(c);
        else {
            c->last_surface = reply.surface_token;
            s->token = reply.surface_token; s->access = access;
            s->description = reply.description;
            s->ready = r == DRE_DARWIN_OK && surface_description_valid(&reply.description);
            if (r == DRE_DARWIN_OK && !s->ready) r = protocol(c);
            s->next = c->surfaces; c->surfaces = s; ++c->surface_count; *out = s; s = NULL;
        }
    } else if (r == DRE_DARWIN_OK) r = protocol(c);
    free(s);
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_surface_describe(struct dre_darwin *c,
    struct dre_darwin_surface *surface, struct dre_gpu_surface_description *out)
{
    if (!c || !out) return DRE_DARWIN_INVALID;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&c->mutex);
    struct dre_darwin_surface *s = owned_surface(c, surface);
    enum dre_darwin_result r = DRE_DARWIN_INVALID;
    if (s && !s->retired) {
        struct dre_gpu_handle_request req = {0};
        struct dre_gpu_surface_reply reply = {0};
        request(c, &req.header, sizeof(req)); req.handle = s->token;
        r = call(c, DRE_GPU_SURFACE_DESCRIBE, &req, sizeof(req), &reply, sizeof(reply), 1);
        if (r == DRE_DARWIN_OK && (!surface_reply_owned(c, &reply) ||
            reply.surface_token != s->token || !surface_description_valid(&reply.description))) r = protocol(c);
        if (r == DRE_DARWIN_OK) { s->description = reply.description; s->ready = 1; *out = reply.description; }
    }
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_surface_close(struct dre_darwin *c, struct dre_darwin_surface *surface)
{
    if (!c) return DRE_DARWIN_INVALID;
    pthread_mutex_lock(&c->mutex);
    struct dre_darwin_surface *s = owned_surface(c, surface);
    enum dre_darwin_result r = DRE_DARWIN_INVALID;
    if (s) { s->retired = 1; r = collect_locked(c); }
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_surface_import_bo(struct dre_darwin *c,
    struct dre_darwin_surface *surface, uint32_t plane, uint32_t access, struct dre_darwin_bo **out)
{
    if (!c || !out || !(access & DRE_GPU_ACCESS_READ) || (access & ~3u)) return DRE_DARWIN_INVALID;
    *out = NULL;
    pthread_mutex_lock(&c->mutex);
    struct dre_darwin_surface *s = owned_surface(c, surface);
    enum dre_darwin_result r = DRE_DARWIN_INVALID;
    if (s && s->ready && !s->retired && !(access & ~s->access) && plane < s->description.plane_count) {
        struct dre_gpu_surface_import_request req = {0};
        struct dre_gpu_bo_create_reply reply = {0};
        request(c, &req.header, sizeof(req)); req.surface_token = s->token; req.plane = plane; req.access = access;
        r = call(c, DRE_GPU_SURFACE_IMPORT_BO, &req, sizeof(req), &reply, sizeof(reply), 0);
        /* There is deliberately no surface-to-BO adapter yet. A successful
         * reply or an error carrying a BO/VA would contradict this contract;
         * quarantine the connection instead of inventing a texture wrapper. */
        if (r == DRE_DARWIN_OK || reply.bo || reply.gpu_va || reply.bytes || reply.access || reply.cache_contract)
            r = protocol(c);
    }
    pthread_mutex_unlock(&c->mutex); return r;
}

enum dre_darwin_result dre_darwin_bo_create(struct dre_darwin *c,
    uint64_t bytes, uint32_t access, struct dre_darwin_bo **out)
{
    if (!c || !out || !bytes || !(access & DRE_GPU_ACCESS_READ) || (access & ~7u) ||
        ((access & DRE_GPU_ACCESS_WRITE) && (access & DRE_GPU_ACCESS_EXECUTE)))
        return DRE_DARWIN_INVALID;
    *out = NULL;
    if (!(c->info.capabilities & DRE_GPU_CAP_OWNED_TASK_BO)) return DRE_DARWIN_UNSUPPORTED;
    if (bytes > c->info.bo_max_bytes || bytes > UINT64_MAX - (DRE_GPU_CPU_PAGE_BYTES - 1))
        return DRE_DARWIN_NO_MEMORY;
    bytes = (bytes + DRE_GPU_CPU_PAGE_BYTES - 1) & ~(uint64_t)(DRE_GPU_CPU_PAGE_BYTES - 1);
    if (bytes > c->info.bo_max_bytes) return DRE_DARWIN_NO_MEMORY;
    struct dre_darwin_bo *b = calloc(1, sizeof(*b));
    if (!b) return DRE_DARWIN_NO_MEMORY;
    pthread_mutex_lock(&c->mutex);
    enum dre_darwin_result r = collect_locked(c);
    uint64_t allocated = 0; uint32_t count = 0;
    for (struct dre_darwin_bo *p = c->bos; p; p = p->next) { allocated += p->bytes; ++count; }
    if (r == DRE_DARWIN_OK && (count >= c->info.max_bos || allocated > c->info.task_max_bytes - bytes))
        r = DRE_DARWIN_NO_MEMORY;
    struct dre_gpu_bo_create_request req = {0};
    struct dre_gpu_bo_create_reply reply;
    request(c, &req.header, sizeof(req)); req.bytes = bytes; req.access = access;
    if (r == DRE_DARWIN_OK) r = call(c, DRE_GPU_BO_CREATE, &req, sizeof(req), &reply, sizeof(reply), 0);
    if (r == DRE_DARWIN_OK) {
        if (!reply.bo || reply.bytes != bytes || reply.access != access ||
            reply.cache_contract != c->info.cache_contract ||
            reply.gpu_va < c->info.gpu_va_base || (reply.gpu_va % DRE_GPU_PAGE_BYTES) ||
            reply.bytes > c->info.gpu_va_bytes ||
            reply.gpu_va - c->info.gpu_va_base > c->info.gpu_va_bytes - reply.bytes ||
            bo_handle(c, reply.bo))
            r = protocol(c);
        for (struct dre_darwin_bo *p = c->bos; r == DRE_DARWIN_OK && p; p = p->next)
            if (reply.bo == p->handle || (reply.gpu_va < p->iova + p->bytes && p->iova < reply.gpu_va + reply.bytes))
                r = protocol(c);
    }
    if (r == DRE_DARWIN_OK) {
        b->handle = reply.bo; b->iova = reply.gpu_va; b->bytes = reply.bytes; b->access = reply.access;
        b->next = c->bos; c->bos = b; *out = b;
    } else free(b);
    pthread_mutex_unlock(&c->mutex);
    return r;
}
enum dre_darwin_result dre_darwin_bo_map(struct dre_darwin *c, struct dre_darwin_bo *bo, void **address)
{
    if (!c || !address) return DRE_DARWIN_INVALID;
    *address = NULL;
    pthread_mutex_lock(&c->mutex);
    struct dre_darwin_bo *b = owned_bo(c, bo);
    enum dre_darwin_result r = DRE_DARWIN_INVALID;
    if (b && !b->retired && !c->poisoned) {
        if (b->cpu_lease) {
            r = b->persistent ? cpu_access_locked(c, b, 1) : DRE_DARWIN_OK;
            if (r == DRE_DARWIN_OK) *address = b->cpu;
        }
        else {
            struct dre_gpu_cpu_map_request req = {0}; struct dre_gpu_cpu_map_reply reply;
            request(c, &req.header, sizeof(req)); req.bo = b->handle;
            req.access = DRE_GPU_CPU_READ | DRE_GPU_CPU_WRITE;
            int persistent = !!(c->info.capabilities & DRE_GPU_CAP_PERSISTENT_CPU_MAP);
            r = call(c, persistent ? DRE_GPU_CPU_MAP_PERSISTENT : DRE_GPU_CPU_MAP,
                &req, sizeof(req), &reply, sizeof(reply), 0);
            if (r == DRE_DARWIN_OK) {
                if (!reply.lease || !reply.task_address || reply.task_address >= (1ull << 47) ||
                    reply.bytes != b->bytes || reply.task_address > (1ull << 47) - reply.bytes ||
                    reply.task_address % DRE_GPU_CPU_PAGE_BYTES || reply.reserved ||
                    reply.cache_contract != c->info.cache_contract)
                    r = protocol(c);
                for (struct dre_darwin_bo *p = c->bos; r == DRE_DARWIN_OK && p; p = p->next)
                    if (p->cpu_lease && (p->cpu_lease == reply.lease ||
                        (reply.task_address < (uint64_t)(uintptr_t)p->cpu + p->bytes &&
                         (uint64_t)(uintptr_t)p->cpu < reply.task_address + reply.bytes)))
                        r = protocol(c);
                if (r == DRE_DARWIN_OK) {
                    b->cpu_lease = reply.lease; b->cpu = (void *)(uintptr_t)reply.task_address; b->persistent = persistent;
                    if (persistent) r = cpu_access_locked(c, b, 1);
                    if (r == DRE_DARWIN_OK) *address = b->cpu;
                }
            }
        }
    }
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_bo_cpu_acquire(struct dre_darwin *c, struct dre_darwin_bo *bo)
{
    if (!c) return DRE_DARWIN_INVALID;
    pthread_mutex_lock(&c->mutex); struct dre_darwin_bo *b = owned_bo(c, bo);
    enum dre_darwin_result r = b && !b->retired ? cpu_access_locked(c, b, 1) : DRE_DARWIN_INVALID;
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_bo_cpu_release(struct dre_darwin *c, struct dre_darwin_bo *bo)
{
    if (!c) return DRE_DARWIN_INVALID;
    pthread_mutex_lock(&c->mutex); struct dre_darwin_bo *b = owned_bo(c, bo);
    enum dre_darwin_result r = b ? cpu_access_locked(c, b, 0) : DRE_DARWIN_INVALID;
    pthread_mutex_unlock(&c->mutex); return r;
}
int dre_darwin_bo_cpu_owned(const struct dre_darwin_bo *b)
{ return b && b->cpu_lease && (!b->persistent || b->cpu_epoch); }
enum dre_darwin_result dre_darwin_bo_unmap(struct dre_darwin *c, struct dre_darwin_bo *bo)
{
    if (!c) return DRE_DARWIN_INVALID;
    pthread_mutex_lock(&c->mutex);
    struct dre_darwin_bo *b = owned_bo(c, bo);
    enum dre_darwin_result r = b ? unmap_locked(c, b) : DRE_DARWIN_INVALID;
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_bo_close(struct dre_darwin *c, struct dre_darwin_bo *bo)
{
    if (!c) return DRE_DARWIN_INVALID;
    pthread_mutex_lock(&c->mutex);
    struct dre_darwin_bo *b = owned_bo(c, bo);
    if (!b || b->retired) { pthread_mutex_unlock(&c->mutex); return DRE_DARWIN_INVALID; }
    b->retired = 1;
    enum dre_darwin_result r = collect_locked(c);
    /* BUSY retirement is retained by the connection, not handed back as a
     * live reusable BO. The caller must discard its pointer on OK or BUSY. */
    if (r == DRE_DARWIN_OK && owned_bo(c, bo)) r = DRE_DARWIN_BUSY;
    pthread_mutex_unlock(&c->mutex); return r;
}
uint64_t dre_darwin_bo_handle(const struct dre_darwin_bo *b) { return b ? b->handle : 0; }
uint64_t dre_darwin_bo_iova(const struct dre_darwin_bo *b) { return b ? b->iova : 0; }
uint64_t dre_darwin_bo_size(const struct dre_darwin_bo *b) { return b ? b->bytes : 0; }
uint32_t dre_darwin_bo_flags(const struct dre_darwin_bo *b) { return b ? b->access : 0; }

enum dre_darwin_result dre_darwin_submit(struct dre_darwin *c,
    const struct dre_gpu_submit_resource *resources, uint32_t resource_count,
    const struct dre_gpu_submit_ib *ibs, uint32_t ib_count,
    const uint64_t *waits, uint32_t wait_count, uint64_t *fence)
{
    if (!c || !fence || !resources || !ibs || !resource_count || !ib_count ||
        (wait_count && !waits) || resource_count > c->info.max_bos ||
        ib_count > c->info.max_ibs || wait_count > c->info.max_waits)
        return DRE_DARWIN_INVALID;
    *fence = 0;
    if (!(c->info.capabilities & DRE_GPU_CAP_RAW_IB_SUBMIT)) return DRE_DARWIN_UNSUPPORTED;
    struct dre_local_fence *record = calloc(1, sizeof(*record));
    if (!record) return DRE_DARWIN_NO_MEMORY;
    struct dre_gpu_submit_request req = {0}; struct dre_gpu_submit_reply reply = {0};
    pthread_mutex_lock(&c->mutex);
    enum dre_darwin_result r = collect_locked(c);
    if (r == DRE_DARWIN_OK && c->fence_count >= c->info.max_fences) r = DRE_DARWIN_BUSY;
    request(c, &req.header, sizeof(req));
    req.resource_count = resource_count; req.ib_count = ib_count; req.wait_count = wait_count;
    for (uint32_t i = 0; r == DRE_DARWIN_OK && i < resource_count; ++i) {
        struct dre_darwin_bo *b = bo_handle(c, resources[i].bo);
        if (!b || resources[i].reserved || !resources[i].access || (resources[i].access & ~3u) ||
            (resources[i].access & ~b->access)) {
            r = DRE_DARWIN_INVALID; break;
        }
        if (b->cpu_lease && !(c->info.capabilities & DRE_GPU_CAP_PERSISTENT_CPU_MAP)) {
            r = DRE_DARWIN_BUSY; break;
        }
        if (b->persistent && b->cpu_epoch) { r = DRE_DARWIN_BUSY; break; }
        for (uint32_t j = 0; j < i; ++j)
            if (resources[j].bo == resources[i].bo) r = DRE_DARWIN_INVALID;
        req.resources[i] = resources[i];
    }
    for (uint32_t i = 0; r == DRE_DARWIN_OK && i < ib_count; ++i) {
        struct dre_darwin_bo *b = bo_handle(c, ibs[i].bo); int listed = 0;
        if (!b || !(b->access & DRE_GPU_ACCESS_EXECUTE) || !ibs[i].bytes || (ibs[i].offset | ibs[i].bytes) % 4 ||
            ibs[i].offset > b->bytes || ibs[i].bytes > b->bytes - ibs[i].offset) {
            r = DRE_DARWIN_INVALID; break;
        }
        for (uint32_t j = 0; j < resource_count; ++j)
            if (resources[j].bo == ibs[i].bo && (resources[j].access & DRE_GPU_ACCESS_READ)) listed = 1;
        if (!listed) { r = DRE_DARWIN_INVALID; break; }
        req.ibs[i] = ibs[i];
    }
    for (uint32_t i = 0; r == DRE_DARWIN_OK && i < wait_count; ++i) {
        struct dre_local_fence *f = fence_handle(c, waits[i]);
        if (!f || !f->references) { r = DRE_DARWIN_INVALID; break; }
        for (uint32_t j = 0; j < i; ++j)
            if (waits[j] == waits[i]) r = DRE_DARWIN_INVALID;
        req.wait_fences[i] = waits[i];
    }
    if (r == DRE_DARWIN_OK) r = call(c, DRE_GPU_SUBMIT, &req, sizeof(req), &reply, sizeof(reply), 0);
    /* The native provider can publish a ticket and subsequently return ANY
     * typed error. TIMEOUT/BUSY/LIMIT/NO_MEMORY are not non-publication proofs.
     * A Mach failure/malformed reply has no usable typed handle; connection
     * teardown leaves that uncertain ownership with the kernel orphan owner. */
    int published = c->last_reply_typed && reply.fence;
    if ((r == DRE_DARWIN_OK && !reply.fence) || (published &&
        (reply.fence <= c->last_fence || reply.reset_generation != c->info.reset_generation ||
         fence_handle(c, reply.fence)))) {
        r = protocol(c); published = 0;
    }
    if (published) {
        record->value = reply.fence; record->references = 1; record->next = c->fences;
        c->fences = record; ++c->fence_count; c->last_fence = reply.fence; *fence = reply.fence;
        if(r!=DRE_DARWIN_OK) {c->poisoned=1;r=DRE_DARWIN_LOST;}
    } else free(record);
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_fence_retain(struct dre_darwin *c, uint64_t fence)
{
    if (!c || !fence) return DRE_DARWIN_INVALID;
    pthread_mutex_lock(&c->mutex);
    struct dre_local_fence *f = fence_handle(c, fence);
    enum dre_darwin_result r = DRE_DARWIN_INVALID;
    if (f && f->references && f->references != UINT32_MAX && !c->poisoned) { ++f->references; r = DRE_DARWIN_OK; }
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_fence_release(struct dre_darwin *c, uint64_t fence)
{
    if (!c || !fence) return DRE_DARWIN_INVALID;
    pthread_mutex_lock(&c->mutex);
    struct dre_local_fence *f = fence_handle(c, fence);
    enum dre_darwin_result r = DRE_DARWIN_INVALID;
    if (f && f->references) { --f->references; r = collect_locked(c); }
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_fence_query(struct dre_darwin *c, uint64_t fence, int *complete)
{
    if (!c || !fence || !complete) return DRE_DARWIN_INVALID;
    *complete=0;
    pthread_mutex_lock(&c->mutex);
    struct dre_local_fence *f = fence_handle(c, fence);
    enum dre_darwin_result r = f && f->references ?
        fence_reply(c, fence, DRE_GPU_QUERY_FENCE, 0, complete) : DRE_DARWIN_INVALID;
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_fence_wait(struct dre_darwin *c, uint64_t fence, uint64_t timeout)
{
    if (!c || !fence) return DRE_DARWIN_INVALID;
    uint64_t start = monotonic_ns();
    if (start == UINT64_MAX) return DRE_DARWIN_LOST;
    uint64_t end = timeout > UINT64_MAX - start ? UINT64_MAX : start + timeout;
    for (;;) {
        uint64_t now = monotonic_ns(), slice = 100000000ull;
        if (now == UINT64_MAX) return DRE_DARWIN_LOST;
        if (end != UINT64_MAX && (now >= end || slice > end - now)) slice = now >= end ? 0 : end - now;
        pthread_mutex_lock(&c->mutex);
        struct dre_local_fence *f = fence_handle(c, fence); int complete = 0;
        enum dre_darwin_result r = f && f->references ?
            fence_reply(c, fence, DRE_GPU_WAIT_FENCE, slice, &complete) : DRE_DARWIN_INVALID;
        if (r == DRE_DARWIN_OK && complete) r = collect_locked(c);
        pthread_mutex_unlock(&c->mutex);
        if (r != DRE_DARWIN_TIMEOUT) return r;
        uint64_t observed=monotonic_ns();
        if(observed==UINT64_MAX)return DRE_DARWIN_LOST;
        if (!slice || (end != UINT64_MAX && observed >= end)) return DRE_DARWIN_TIMEOUT;
    }
}
enum dre_darwin_result dre_darwin_cache(struct dre_darwin *c, struct dre_darwin_bo *bo,
    uint64_t offset, uint64_t bytes, uint32_t op)
{
    if (!c || !bytes || (op != DRE_GPU_CACHE_TO_GPU && op != DRE_GPU_CACHE_FROM_GPU))
        return DRE_DARWIN_INVALID;
    pthread_mutex_lock(&c->mutex);
    struct dre_darwin_bo *b = owned_bo(c, bo);
    enum dre_darwin_result r = DRE_DARWIN_INVALID;
    if (b && b->persistent) { pthread_mutex_unlock(&c->mutex); return DRE_DARWIN_UNSUPPORTED; }
    if (b && !b->retired && offset <= b->bytes && bytes <= b->bytes - offset) {
        struct dre_gpu_cache_request req = {0}; struct dre_gpu_status_reply reply;
        request(c, &req.header, sizeof(req)); req.bo = b->handle;
        req.offset = offset; req.bytes = bytes; req.operation = op;
        r = call(c, DRE_GPU_CACHE_OP, &req, sizeof(req), &reply, sizeof(reply), 0);
    }
    pthread_mutex_unlock(&c->mutex); return r;
}
enum dre_darwin_result dre_darwin_close(struct dre_darwin *c)
{
    if (!c) return DRE_DARWIN_OK;
    pthread_mutex_lock(&c->mutex);
    enum dre_darwin_result result = DRE_DARWIN_OK;
    for (struct dre_darwin_bo *b = c->bos; b; b = b->next)
        if (unmap_locked(c, b) != DRE_DARWIN_OK) result = DRE_DARWIN_LOST;
    for (struct dre_darwin_surface *s = c->surfaces; s; s = s->next)
        if (handle_call(c, DRE_GPU_SURFACE_CLOSE, s->token, 1) != DRE_DARWIN_OK) result = DRE_DARWIN_LOST;
#if defined(__APPLE__)
    if (c->connection && IOServiceClose((io_connect_t)c->connection) != KERN_SUCCESS)
        result = DRE_DARWIN_LOST;
#endif
    c->connection = 0;
    /* Only userspace metadata is freed. Kernel-owned wired pages, mappings,
     * jobs and uncertain fences are the provider's retained/quarantine duty. */
    while (c->bos) { struct dre_darwin_bo *next = c->bos->next; free(c->bos); c->bos = next; }
    while (c->fences) { struct dre_local_fence *next = c->fences->next; free(c->fences); c->fences = next; }
    while (c->surfaces) { struct dre_darwin_surface *next = c->surfaces->next; free(c->surfaces); c->surfaces = next; }
    pthread_mutex_unlock(&c->mutex); pthread_mutex_destroy(&c->mutex); free(c);
    return result;
}
const char *dre_darwin_result_name(enum dre_darwin_result r)
{
    switch (r) {
    case DRE_DARWIN_OK: return "OK";
    case DRE_DARWIN_NO_PROVIDER: return "NO_NATIVE_PROVIDER";
    case DRE_DARWIN_UNSUPPORTED: return "UNSUPPORTED_NATIVE_CONTRACT";
    case DRE_DARWIN_INVALID: return "INVALID_REQUEST";
    case DRE_DARWIN_NO_MEMORY: return "NATIVE_MEMORY_LIMIT";
    case DRE_DARWIN_BUSY: return "NATIVE_BUSY";
    case DRE_DARWIN_TIMEOUT: return "HARDWARE_FENCE_TIMEOUT";
    case DRE_DARWIN_LOST: return "NATIVE_DEVICE_LOST";
    case DRE_DARWIN_PROTOCOL: return "NATIVE_PROTOCOL_MISMATCH";
    default: return "UNKNOWN_TRANSPORT_RESULT";
    }
}
