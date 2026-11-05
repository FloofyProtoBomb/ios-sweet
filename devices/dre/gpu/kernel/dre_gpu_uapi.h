/* SPDX-License-Identifier: MIT */
/* UNVALIDATED overnight draft. No ABI or hardware validation has run.
 * Fixed-width task-scoped DREAdrenoGPU protocol; never AGX compatibility. */
#ifndef DRE_GPU_UAPI_H
#define DRE_GPU_UAPI_H
#include <stdint.h>

#define DRE_GPU_SERVICE "DREAdrenoGPU"
#define DRE_GPU_ABI_VERSION 1u
#define DRE_GPU_MAX_BOS 64u
#define DRE_GPU_MAX_IBS 16u
#define DRE_GPU_MAX_WAITS 16u
#define DRE_GPU_MAX_FENCES 128u
#define DRE_GPU_CPU_PAGE_BYTES 16384u
#define DRE_GPU_PAGE_BYTES 4096u
#define DRE_GPU_BO_MAX_BYTES (64ull << 20)
#define DRE_GPU_TASK_MAX_BYTES (128ull << 20)
#define DRE_GPU_WAIT_MAX_NS 5000000000ull

enum dre_gpu_selector {
    DRE_GPU_GET_INFO=0, DRE_GPU_BO_CREATE=1, DRE_GPU_BO_CLOSE=2,
    DRE_GPU_SUBMIT=3, DRE_GPU_WAIT_FENCE=4, DRE_GPU_QUERY_FENCE=5,
    DRE_GPU_CACHE_OP=6, DRE_GPU_CPU_MAP=7, DRE_GPU_CPU_UNMAP=8,
    DRE_GPU_FENCE_CLOSE=9,
    /* Append-only opt-in extension. Selectors0..9 and their ABI1 layouts stay
     * unchanged. These require the absent PERSISTENT_CPU_MAP capability. */
    DRE_GPU_CPU_MAP_PERSISTENT=10, DRE_GPU_CPU_ACCESS_BEGIN=11,
    DRE_GPU_CPU_ACCESS_END=12,
    /* CPU backing retention is not GPU import/capability. Original ABI1
     * selectors0..12/layouts are unchanged by this append-only extension. */
    DRE_GPU_SURFACE_RETAIN=13, DRE_GPU_SURFACE_DESCRIBE=14,
    DRE_GPU_SURFACE_CLOSE=15, DRE_GPU_SURFACE_IMPORT_BO=16,
    DRE_GPU_SELECTOR_COUNT=17
};
enum dre_gpu_status {
    DRE_GPU_OK=0, DRE_GPU_UNSUPPORTED=-1, DRE_GPU_INVALID=-2,
    DRE_GPU_NOT_READY=-3, DRE_GPU_NO_MEMORY=-4, DRE_GPU_BUSY=-5,
    DRE_GPU_STALE=-6, DRE_GPU_TIMEOUT=-7, DRE_GPU_FAULT=-8,
    DRE_GPU_CLOSED=-9, DRE_GPU_LIMIT=-10
};
enum dre_gpu_cap {
    DRE_GPU_CAP_OWNED_TASK_BO=1ull<<0,
    DRE_GPU_CAP_CPU_MAP_LEASE=1ull<<1,
    DRE_GPU_CAP_EXPLICIT_CACHE=1ull<<2,
    DRE_GPU_CAP_HARDWARE_FENCE=1ull<<3,
    DRE_GPU_CAP_SINGLE_FIFO=1ull<<4,
    /* Requires real untrusted packet validation AND task protection, not just
     * a ring buffer, marker helper or kernel-authored command submission. */
    DRE_GPU_CAP_RAW_IB_SUBMIT=1ull<<5,
    DRE_GPU_CAP_PERSISTENT_CPU_MAP=1ull<<6
};
#define DRE_GPU_VULKAN_REQUIRED_CAPS ((1ull<<7)-1)
enum dre_gpu_bo_access {
    /* READ is mandatory; WRITE and EXECUTE are mutually exclusive. The
     * A619 stage-1 AP encoding does not provide write-only/execute-only BOs. */
    DRE_GPU_ACCESS_READ=1u, DRE_GPU_ACCESS_WRITE=2u,
    DRE_GPU_ACCESS_EXECUTE=4u
};
enum dre_gpu_cpu_access { DRE_GPU_CPU_READ=1u, DRE_GPU_CPU_WRITE=2u };
enum dre_gpu_cache_contract {
    /* Owned wired RAM, Normal-NC/WriteCombine in both aliases; NOT Device.
     * CPU leases must end before GPU submission. GPU dirty lines still need
     * real backend clean/invalidate before remap/retirement. */
    DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE=1u,
    /* Reserved for a proven persistent mapping + explicit synchronization
     * backend with immutable command snapshots. Not supplied by this draft. */
    DRE_GPU_CACHE_OWNED_NORMAL_NC_PERSISTENT=2u
};
enum dre_gpu_cache_operation {
    DRE_GPU_CACHE_TO_GPU=1u, DRE_GPU_CACHE_FROM_GPU=2u
};
enum dre_gpu_fence_state {
    DRE_GPU_FENCE_PENDING=0u, DRE_GPU_FENCE_COMPLETE=1u,
    DRE_GPU_FENCE_ERROR=2u, DRE_GPU_FENCE_RESET=3u
};
/* reserved fields, unused array elements and all padding must be zero.
 * GetInfo accepts generation 0. Every other request must match its reply's
 * connection generation. No addresses here are physical or caller IOVA. */
struct dre_gpu_request_header {
    uint32_t abi_version, struct_bytes;
    uint64_t session_generation, reserved;
};
struct dre_gpu_reply_header {
    uint32_t abi_version, struct_bytes;
    int32_t status;
    uint32_t flags;
    uint64_t session_generation;
};
struct dre_gpu_info_request { struct dre_gpu_request_header header; };
struct dre_gpu_info_reply {
    struct dre_gpu_reply_header header;
    uint64_t capabilities, bo_max_bytes, task_max_bytes;
    uint64_t gpu_va_base, gpu_va_bytes, reset_generation;
    uint32_t chip_id, gmem_bytes, cpu_page_bytes, gpu_page_bytes;
    uint32_t cache_contract, max_bos, max_ibs, max_waits;
    /* Stable missing-prerequisite mask from the actual native provider. */
    uint64_t unavailable;
    /* Native queried/configured values, never copied from an AGX/A740 model.
     * Zero means unavailable, not a permission to guess a device profile. */
    uint64_t gmem_base, uche_trap_base, timestamp_frequency;
    uint32_t highest_bank_bit, ubwc_swizzle, macrotile_mode, max_fences;
};
struct dre_gpu_bo_create_request {
    struct dre_gpu_request_header header;
    uint64_t bytes;
    uint32_t access, reserved;
};
struct dre_gpu_bo_create_reply {
    struct dre_gpu_reply_header header;
    uint64_t bo, gpu_va, bytes;
    uint32_t access, cache_contract;
};
struct dre_gpu_handle_request {
    struct dre_gpu_request_header header;
    uint64_t handle;
};
struct dre_gpu_status_reply { struct dre_gpu_reply_header header; };
struct dre_gpu_cpu_map_request {
    struct dre_gpu_request_header header;
    uint64_t bo;
    uint32_t access, reserved;
};
struct dre_gpu_cpu_map_reply {
    struct dre_gpu_reply_header header;
    uint64_t lease, task_address, bytes;
    uint32_t cache_contract, reserved;
};
/* A persistent map lease keeps the pointer alive; it does not claim CPU/GPU
 * access ownership. BEGIN requires epoch0 and requested CPU access. END uses
 * the exact returned epoch, access0. Both must match BO and actual map lease.
 * Pointer stores outside an acquired epoch violate synchronization; the
 * mapping itself cannot revoke or validate arbitrary CPU memory accesses.
 * Immutable command snapshots/task protection remain independent requirements.
 */
struct dre_gpu_cpu_access_request {
    struct dre_gpu_request_header header;
    uint64_t bo,map_lease,epoch;
    uint32_t access,reserved;
};
struct dre_gpu_cpu_access_reply {
    struct dre_gpu_reply_header header;
    uint64_t epoch;
};
#define DRE_GPU_MAX_SURFACES 16u
#define DRE_GPU_MAX_SURFACE_PLANES 20u
enum dre_gpu_surface_flags {
    DRE_GPU_SURFACE_RETAINED=1u, DRE_GPU_SURFACE_DESCRIPTOR_PREPARED=2u,
    DRE_GPU_SURFACE_PAGE_INVENTORY=4u
};
enum dre_gpu_surface_refusal {
    DRE_GPU_SURFACE_LIVE_CACHE_ALIASES=1ull<<0,
    DRE_GPU_SURFACE_GPU_CACHE_OWNER_MISSING=1ull<<1,
    DRE_GPU_SURFACE_TASK_VM_IMPORT_MISSING=1ull<<2,
    DRE_GPU_SURFACE_SHARED_EVENT_MISSING=1ull<<3,
    DRE_GPU_SURFACE_FORMAT_UNSUPPORTED=1ull<<4,
    DRE_GPU_SURFACE_PHYSICAL_BACKING=1ull<<5,
    DRE_GPU_SURFACE_PLANE_LAYOUT_UNSUPPORTED=1ull<<6,
    DRE_GPU_SURFACE_GPU_OWNERSHIP_UNPROVEN=1ull<<7
};
struct dre_gpu_surface_plane {
    uint32_t width,height,row_bytes,offset,bytes;
    uint32_t bytes_per_element,element_width,element_height;
};
struct dre_gpu_surface_description {
    uint64_t allocation_bytes;
    uint32_t surface_id,width,height,pixel_format;
    uint32_t plane_count,descriptor_kind,pin_flags,page_count;
    uint64_t gpu_import_refusals;
    struct dre_gpu_surface_plane planes[DRE_GPU_MAX_SURFACE_PLANES];
};
struct dre_gpu_surface_retain_request {
    struct dre_gpu_request_header header;
    uint32_t mach_port_name,access; /* real send right in owning task's IPC space */
};
struct dre_gpu_surface_reply {
    struct dre_gpu_reply_header header;
    uint64_t surface_token;
    struct dre_gpu_surface_description description;
};
struct dre_gpu_surface_import_request {
    struct dre_gpu_request_header header;
    uint64_t surface_token;
    uint32_t plane,access;
};
/* CPUUnmap uses handle=lease. It revokes the actual mapping before returning.
 * A lease is not an IOConnectMapMemory token and must not be mapped twice. */
struct dre_gpu_cache_request {
    struct dre_gpu_request_header header;
    uint64_t bo, offset, bytes;
    uint32_t operation, reserved;
};
struct dre_gpu_submit_ib { uint64_t bo, offset, bytes; };
/* Resource usage is READ/WRITE only. EXECUTE is a BO mapping permission;
 * every IB additionally requires its BO to have that permission. */
struct dre_gpu_submit_resource { uint64_t bo; uint32_t access, reserved; };
struct dre_gpu_submit_request {
    struct dre_gpu_request_header header;
    uint32_t ib_count, resource_count, wait_count, reserved;
    struct dre_gpu_submit_ib ibs[DRE_GPU_MAX_IBS];
    struct dre_gpu_submit_resource resources[DRE_GPU_MAX_BOS];
    uint64_t wait_fences[DRE_GPU_MAX_WAITS];
};
struct dre_gpu_submit_reply {
    struct dre_gpu_reply_header header;
    uint64_t fence, reset_generation;
};
struct dre_gpu_fence_request {
    struct dre_gpu_request_header header;
    uint64_t fence, timeout_ns;
};
struct dre_gpu_fence_reply {
    struct dre_gpu_reply_header header;
    uint64_t fence, reset_generation;
    uint32_t state, hardware_error;
};
#if defined(__cplusplus)
static_assert(sizeof(dre_gpu_request_header)==24,"request header ABI");
static_assert(sizeof(dre_gpu_reply_header)==24,"reply header ABI");
static_assert(sizeof(dre_gpu_info_reply)==152,"info ABI");
static_assert(sizeof(dre_gpu_submit_request)==1576,"submit ABI");
static_assert(sizeof(dre_gpu_cpu_map_reply)==56,"mapping ABI");
static_assert(sizeof(dre_gpu_cpu_access_request)==56,"CPU access request ABI");
static_assert(sizeof(dre_gpu_cpu_access_reply)==32,"CPU access reply ABI");
static_assert(sizeof(dre_gpu_surface_plane)==32,"surface plane ABI");
static_assert(sizeof(dre_gpu_surface_reply)==720,"surface reply ABI");
#else
_Static_assert(sizeof(struct dre_gpu_request_header)==24,"request header ABI");
_Static_assert(sizeof(struct dre_gpu_reply_header)==24,"reply header ABI");
_Static_assert(sizeof(struct dre_gpu_info_reply)==152,"info ABI");
_Static_assert(sizeof(struct dre_gpu_submit_request)==1576,"submit ABI");
_Static_assert(sizeof(struct dre_gpu_cpu_map_reply)==56,"mapping ABI");
_Static_assert(sizeof(struct dre_gpu_cpu_access_request)==56,"CPU access request ABI");
_Static_assert(sizeof(struct dre_gpu_cpu_access_reply)==32,"CPU access reply ABI");
_Static_assert(sizeof(struct dre_gpu_surface_plane)==32,"surface plane ABI");
_Static_assert(sizeof(struct dre_gpu_surface_reply)==720,"surface reply ABI");
#endif
#endif
