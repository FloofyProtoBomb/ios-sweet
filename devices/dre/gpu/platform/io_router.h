/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only connection. */
#ifndef DRE_GPU_IO_ROUTER_H
#define DRE_GPU_IO_ROUTER_H
#include "native_owner.h"
#include "rpm_native.h"
#ifdef __cplusplus
extern "C" {
#endif
/* One resident kernel cookie for the startup plan's GPU/RPM/SMMU I/O views.
 * This owns NO provider, mapping, lease, allocator or rail. Both real provider
 * objects, their actual shared board mutex and this context remain resident
 * through every use and uncertain failure. All calls are serialized task
 * context with that SAME board mutex held; there is no IRQ API or new lock.
 * Providers below are obtained only through the actual native-owner/RPM APIs.
 * Matching numbers alone must never stand in for the native broker's genuine
 * common lease identity or shared owner-lock binding.
 */
struct dre_gpu_io_router {
    struct dre_gpu_native_owner *owner;
    struct dre_rpm_native *rpm;
    struct dre_gpu_native_io gpu_io;
    struct dre_rpm_mmio_ops rpm_io;
    struct dre_rpm_mmio_map rpm_map;
    struct dre_rpm_config rpm_protocol;
    void *context;
    uint64_t lease,attempted_writes;
    uint32_t bound,unsafe,guarding,accessing,predicate;
};
/* Fresh zeroed private context only. No MMIO access, GLINK negotiation, vote,
 * hardware acquisition or reference transfer. Real nonzero provider leases
 * must already be identical; IDs are never remapped/manufactured to match.
 * Status APIs return DRE_GPU_OK=0, callbacks retain exact boolean1 semantics.
 */
int dre_gpu_io_router_bind(struct dre_gpu_io_router *,struct dre_gpu_native_owner *,
                          struct dre_rpm_native *,void *resident_context);
/* Metadata only, not an ownership or readiness proof. The caller's actual
 * allocator/VM/PAS/firmware callbacks may retrieve their resident context here.
 * It remains available after a fault so resources can stay reachable/held. */
void *dre_gpu_io_router_context(void *router_cookie);
int dre_gpu_io_router_guard(void *);
int dre_gpu_io_router_cold(void *,uint64_t);
int dre_gpu_io_router_policy(void *,uint64_t,const struct dre_rpm_votes *,
                             const struct dre_holi_power_config *);
/* Real guarded callback views, all with cookie=router. Native I/O is GPU-only;
 * combined MMIO additionally exposes qualified RPM RAM and SEND_ID=0 writes.
 * The physical 19.2MHz counter is forwarded from the actual GPU Device owner;
 * the native RPM binding independently qualifies the same timer frequency.
 */
int dre_gpu_io_router_native_io(struct dre_gpu_io_router *,struct dre_gpu_native_io *);
int dre_gpu_io_router_mmio(struct dre_gpu_io_router *,struct dre_rpm_mmio_ops *);
/* Fill only known read/write/barrier/now/basic guard/cold/policy operations.
 * No existing nonmatching cookie/function is overwritten. All existing memory
 * or provider callbacks must already use this router cookie. Stronger SMMU
 * guard/owns_ram, RBBM providers_ready, CP platform/mapping/firmware/PAS and VM
 * callbacks stay caller-supplied; missing operations stay NULL and startup
 * continues to refuse. This does not call startup or set any ready mask.
 */
int dre_gpu_io_router_plan_io(struct dre_gpu_io_router *,struct dre_a619_start_plan *);
#ifdef __cplusplus
}
#endif
#endif
