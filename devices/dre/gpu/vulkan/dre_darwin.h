/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source draft. No compile, test or device execution.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "dre_gpu_uapi.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Transport errors stay independent of Vulkan so no VK_SUCCESS can be
 * inferred from service existence, an IOKit call alone, or marker execution. */
enum dre_darwin_result {
    DRE_DARWIN_OK = 0,
    DRE_DARWIN_NO_PROVIDER,
    DRE_DARWIN_UNSUPPORTED,
    DRE_DARWIN_INVALID,
    DRE_DARWIN_NO_MEMORY,
    DRE_DARWIN_BUSY,
    DRE_DARWIN_TIMEOUT,
    DRE_DARWIN_LOST,
    DRE_DARWIN_PROTOCOL
};
struct dre_darwin;
struct dre_darwin_bo;
struct dre_darwin_surface;

/* required_caps is compared with the real provider reply. The Turnip adapter
 * requires raw validated PM4, persistent task mapping, complete resource pins, explicit
 * cache ownership and hardware fences. A marker-only provider is insufficient.
 * required_caps=0 opens only the versioned control connection; missing GPU
 * geometry/capabilities remain missing. It grants no BO, Vulkan or Metal use.
 * No configurable service names, selector probes or AGX identity are used. */
enum dre_darwin_result dre_darwin_open(uint64_t required_caps,
    struct dre_darwin **out);
const struct dre_gpu_info_reply *dre_darwin_info(const struct dre_darwin *client);
/* Close invalidates the task's handles; the native provider must quarantine
 * any uncertain in-flight mappings/pages, not free them on userclient death. */
enum dre_darwin_result dre_darwin_close(struct dre_darwin *client);

enum dre_darwin_result dre_darwin_bo_create(struct dre_darwin *client,
    uint64_t bytes, uint32_t flags, struct dre_darwin_bo **out);
enum dre_darwin_result dre_darwin_bo_map(struct dre_darwin *client,
    struct dre_darwin_bo *bo, void **address);
/* New ABI1 opt-in selectors keep map lifetime separate from access epochs.
 * Kernel currently withholds the persistent capability; no driver may infer
 * coherent access from a non-NULL pointer. These calls preserve that refusal. */
enum dre_darwin_result dre_darwin_bo_cpu_acquire(struct dre_darwin *client,
    struct dre_darwin_bo *bo);
enum dre_darwin_result dre_darwin_bo_cpu_release(struct dre_darwin *client,
    struct dre_darwin_bo *bo);
int dre_darwin_bo_cpu_owned(const struct dre_darwin_bo *bo);
enum dre_darwin_result dre_darwin_bo_unmap(struct dre_darwin *client,
    struct dre_darwin_bo *bo);
/* The caller discards its BO pointer once close starts. OK can mean the
 * user's handle was closed while native asynchronous retirement still holds
 * GPU pins/VA. Neither OK nor BUSY authorizes caller physical/IOVA reuse. */
enum dre_darwin_result dre_darwin_bo_close(struct dre_darwin *client,
    struct dre_darwin_bo *bo);
uint64_t dre_darwin_bo_handle(const struct dre_darwin_bo *bo);
uint64_t dre_darwin_bo_iova(const struct dre_darwin_bo *bo);
uint64_t dre_darwin_bo_size(const struct dre_darwin_bo *bo);
uint32_t dre_darwin_bo_flags(const struct dre_darwin_bo *bo);

/* Actual IOSurface Mach send right in this task, not a global surface ID,
 * client pixel pointer, snapshot or physical address. Kernel retains its real
 * backing independently of this temporary send right. A typed partial-error
 * reply may still return *out: the caller must close that pin even on failure.
 * These operations grant no GPU import or Vulkan capability.
 */
enum dre_darwin_result dre_darwin_surface_retain_port(struct dre_darwin *client,
    uint32_t mach_port_name, uint32_t access, struct dre_darwin_surface **out);
enum dre_darwin_result dre_darwin_surface_describe(struct dre_darwin *client,
    struct dre_darwin_surface *surface, struct dre_gpu_surface_description *out);
/* The caller discards its pointer when close starts, including BUSY/error.
 * Retired userspace metadata and failed kernel descriptor completion remain
 * in their connection's cleanup ownership; neither permits backing reuse. */
enum dre_darwin_result dre_darwin_surface_close(struct dre_darwin *client,
    struct dre_darwin_surface *surface);
/* This draft's real import attempt returns Unsupported and *bo=NULL. Metadata
 * retention/page inventory is never converted to a GPU VA, coherent host import
 * or a successful texture. Inspect description.gpu_import_refusals for blockers.
 */
enum dre_darwin_result dre_darwin_surface_import_bo(struct dre_darwin *client,
    struct dre_darwin_surface *surface, uint32_t plane, uint32_t access,
    struct dre_darwin_bo **bo);

/* Requests use only kernel-returned BO handles and offsets, never a caller PA
 * or arbitrary IOVA. A request is submitted atomically; capacity overflow is an
 * explicit failure and is never silently split into independent jobs. If an
 * ANY valid typed error reply carries a nonzero fence, publication already occurred: that
 * returned fence still owns one user reference and must be released/closed.
 * Further work is poisoned; the result is LOST even if the original kernel
 * status was BUSY/TIMEOUT/NO_MEMORY. An error does not imply native BO pins or
 * translations have retired. Mach/malformed replies retain uncertain kernel
 * ownership through connection teardown, never a manufactured fence handle. */
/* Resource access uses READ/WRITE only; each IB additionally requires an
 * EXECUTE-capable BO created without WRITE. These permissions are distinct. */
enum dre_darwin_result dre_darwin_submit(struct dre_darwin *client,
    const struct dre_gpu_submit_resource *resources, uint32_t resource_count,
    const struct dre_gpu_submit_ib *ibs, uint32_t ib_count,
    const uint64_t *wait_fences, uint32_t wait_count, uint64_t *fence);
/* Submit returns one user reference. Copies into sync/queue objects retain;
 * move transfers; final release closes the kernel handle only when safe. */
enum dre_darwin_result dre_darwin_fence_retain(struct dre_darwin *client,
    uint64_t fence);
/* A recognized caller reference is consumed even when collection afterwards
 * fails. Never release the same caller reference twice on an error retry.
 * The zero-reference retirement record remains owned by the connection. */
enum dre_darwin_result dre_darwin_fence_release(struct dre_darwin *client,
    uint64_t fence);
enum dre_darwin_result dre_darwin_fence_query(struct dre_darwin *client,
    uint64_t fence, int *complete);
enum dre_darwin_result dre_darwin_fence_wait(struct dre_darwin *client,
    uint64_t fence, uint64_t timeout_ns);
enum dre_darwin_result dre_darwin_cache(struct dre_darwin *client,
    struct dre_darwin_bo *bo, uint64_t offset, uint64_t bytes, uint32_t operation);
const char *dre_darwin_result_name(enum dre_darwin_result result);

#ifdef __cplusplus
}
#endif
