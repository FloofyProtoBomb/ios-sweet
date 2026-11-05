/* SPDX-License-Identifier: MIT
 * UNVALIDATED source-only native lifecycle composition. Never auto-published.
 */
#ifndef DRE_GPU_PLATFORM_NATIVE_PROVIDER_H
#define DRE_GPU_PLATFORM_NATIVE_PROVIDER_H
#include "gic_route.h"
#include "rpm_native.h"
#include "gcc_hold.h"
#include "boot_binding.h"
#include "../firmware/native.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_GPU_PLATFORM_PROVIDER_CONFIG_VERSION 2u
struct dre_gpu_scm_request_policy;
struct dre_gpu_platform_provider;
struct dre_gpu_platform_provider_config {
    uint32_t version,struct_bytes;
    void *cookie;
    /* Real external board policy. Both brokers and all factories below must
     * use the exact service owner mutex received by bind_common_lock. */
    struct dre_gpu_native_broker gpu;
    struct dre_rpm_native_broker rpm;
    int (*bind_common_lock)(void *,const struct dre_gpu_owner_lock *,uint32_t *retained);
    /* Supply a genuinely created/held GIC controller after common lock bind.
     * It is not synthesized here and may be shared with the actual TLMM
     * provider only under its genuine common ownership/namespace policy. */
    int (*gic_policy)(void *,struct dre_gpu_gic_policy *);
    /* Factory is metadata-only: copy real acquired SCM/PAS/clock/retirement
     * callbacks using the exact actual GPU lease. Do not invoke SCM, retag
     * reserved RAM or start GPU clocks while constructing this table. */
    int (*firmware_policy)(void *,uint64_t lease,struct dre_gpu_fw_native_ops *);
    /* Independently qualified platform-wide active rail plan. DT init/proxy
     * values, GPU minimum frequency and ACK presence cannot supply it. Return
     * only genuine approved aggregate floors and allowed discrete masks. */
    int (*aggregate_policy)(void *,uint64_t lease,
        const struct dre_holi_power_profile *,uint32_t floors[3],uint32_t masks[3]);
    /* Genuine GPLL0/XO/interconnect/source references and global GCC update
     * serialization, AFTER actual PAS-buffer acceptance and real rail ACKs.
     * Metadata factory only; gcc_hold performs actual acquisition/readback and
     * strictly named OR writes. No default source policy or invented token. */
    int (*gcc_policy)(void *,uint64_t lease,const struct dre_rpm_votes *,
        const struct dre_holi_power_config *,struct dre_holi_gcc_policy *);
    /* Reserve actual CB/SMR/ASID/global ownership. On partial failure preserve
     * a nonzero domain_ops.reservation_lease whenever anything remains held.
     * Return only acquired CB_OWNED|ASID_OWNED prerequisites (36). NC table
     * proof comes from fresh allocation; powered/global/quiescent proof comes
     * later at the actual startup DOMAIN phase, never a future-state bit. */
    int (*domain_reservation)(void *,uint64_t lease,struct dre_a619_smmu *,
        struct dre_gpu_boot_domain_ops *);
    uint32_t window_pages,reserved; /* explicit power-of-two1024..65536 */
    uint64_t rpm_budget_ticks,cp_budget_ticks; /* checked19.2MHz, nonzero<=1s */
    /* Exclusive alternative to firmware_policy above. Metadata factory only;
     * supplies genuine AP PAS13 request exclusion, PIL interconnect/security
     * and loan lifetime references. Configure then performs actual serialized
     * SCM64 discovery and builds observed firmware ops. No future EL3 grant
     * or authentication may be asserted by this factory. No default policy. */
    int (*scm_request_policy)(void *,uint64_t lease,struct dre_gpu_scm_request_policy *);
};
/* Read-only original board observation and resident source table creation.
 * Does NOT publish an IOService, bind a mutex, acquire hardware or call SCM.
 * Caller must retain this state/config policy cookies for the boot lifetime.
 * Obtaining the table escapes its cookie, so no speculative unload is offered.
 */
int dre_gpu_platform_provider_create(const struct dre_gpu_platform_provider_config *,
    struct dre_gpu_platform_provider **out,struct dre_gpu_native_provider *table);
/* Optional explicit post-start fault IRQ arm. Requires actual startup marker
 * and live native provider checks via native_owner; not called by describe or
 * configure, and it enables no GPU RBBM interrupt mask. No hidden auto-arm. */
int dre_gpu_platform_provider_arm_fault_irq(struct dre_gpu_platform_provider *);
/* Private owner-lock-held diagnostics only, never a user ABI/readiness flag.
 * Partial lease metadata and constant failure reason stay available on error.
 */
int dre_gpu_platform_provider_retained(struct dre_gpu_platform_provider *,
    uint64_t *lease,uint64_t *reset_epoch,const char **reason);
#ifdef __cplusplus
}
#endif
#endif
