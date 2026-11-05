/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source. Built-in kernel callers only; no user UAPI.
 */
#ifndef DRE_TOUCH_XNU_PROVIDER_H
#define DRE_TOUCH_XNU_PROVIDER_H
#ifdef __cplusplus
extern "C" {
#endif
#include "native.h"
#include "smmu.h"
#include "qup.h"

#define DRE_TOUCH_XNU_PROVIDER_VERSION 3u
enum { DRE_TOUCH_DEVICE_READ = 1u, DRE_TOUCH_DEVICE_WRITE = 2u };
typedef struct dre_tn_smmu_page dre_touch_xnu_page;

/* All callbacks return0 only after the described actual operation/proof.
 * The provider remains resident for the complete acquired lease lifetime.
 * No default provider is installed; absent policy/route/map callbacks refuse.
 */
struct dre_touch_xnu_provider {
    uint32_t version, struct_bytes;
    void *cookie;
    /* Optional expected image of the boot-held original FDT. Actual authority
     * comes from dre_native_board_snapshot, never from this caller pointer.
     * A supplied expectation must match that snapshot byte-for-byte. */
    const void *fdt;
    uint64_t fdt_bytes;
    /* Atomic shared-orchestrator acquisition: QUP SE0, a free AP GPII pair,
     * event ring, GPIO0..3/21/22, GPI/GENI/TLMM IRQs, held wrapper/SE/source
     * clocks and actual incell panel/reset/power dependencies. Must refuse
     * foreign/inherited DMA, unqualified policy and legacy Sweet touch use.
     * Return a nonnull lease even on failure if ANY partial ownership remains.
     * Failed partial acquisitions may only be dropped by a proven release. */
    int (*acquire)(void *, const struct dre_tn_profile *, void **lease, unsigned *gpii);
    /* Covers actual shared power/MMIO access and immutable exclusive pins,
     * GPI/SE/IRQ ownership for the transaction, including foreign firmware.
     * The adapter's registered-owner ledger does not prove this global policy.
     * Qualification may not race a shared clock-parent/rate transition. */
    int (*check_lease)(void *, void *lease);
    /* Hard-IRQ-safe lease generation/owner check. No sleeping, locks which
     * recurse into the adapter, allocating, policy acquisition or rail MMIO.
     * This checks an already held owner; it cannot establish power readiness. */
    int (*check_irq_lease_fast)(void *, void *lease);
    /* Return the actual already-held XO reference frequency, not CNTFRQ or an
     * unheld FDT property. GCC/GENI/Fabia decoding is performed by the adapter.
     * No default 19.2MHz ready-value provider is supplied. */
    int (*clock_reference)(void *, void *lease, uint64_t *xo_hz);
    /* Validate this exact readback against the real native clock/rail owner's
     * acquired parent/source and aggregate voltage/bandwidth requirements.
     * Hold that existing tree and rate unchanged until release. A derived
     * frequency is not a qualified shared rail floor or panel policy.
     * No guessed PLL/source/divider writes are permitted by this interface. */
    int (*check_observed_clock)(void *, void *lease,const struct dre_tn_qup_snapshot *,
        const struct dre_tn_qup_clock *);
    int (*mask_gpi_geni_sync)(void *, void *lease, unsigned gpii);
    /* Return the actual retained-by-caller IOService nub/source for logical
     * GPIO22 after proving the live TLMM/PDC/GIC cascade, polarity and exclusive
     * native interrupt ownership. The nub must implement a real interrupt
     * controller path. GPIO22 must NOT be used as a GIC INTID or guessed route.
     * The leased route must forward this pin exclusively and coordinate local
     * TLMM mask/ack ownership with the adapter; no second controller may race
     * the adapter by changing GPIO22 config/status behind its pin lock.
     * Hold this actual IOService and its registered native IOInterruptController
     * resident and freeze the source's controller/specifier/cache mapping until
     * release. Qualify the actual controller vector range and CPU-handler/
     * summary/PDC delivery path; a matching metadata array alone cannot do so.
     * The adapter captures and retains those exact IOKit objects and cache,
     * refuses missing/changed/shared mappings, and uses the captured controller
     * for real registration/join/unregistration. Its actual TLMM mask remains
     * necessary because generic IOKit disable can be only a software disable.
     * Resolve the actual source cache only after its real controller has been
     * registered. The adapter refuses missing caches without calling XNU's
     * blocking missing-name controller lookup; it does not wait for a promise.
     * For this DE2118 path the nub must be the actual DRETouchTLMMController
     * child returned by dre_touch_tlmm_nub. Adapter obtains its real pin engine
     * through dre_touch_tlmm_pin_for_nub; both use ONE controller-owned native
     * IOSimpleLock. The adapter does not race another local GPIO register lock.
     * This route still needs actual global awake TLMM/summary/pin policy and
     * the leased native GIC259 provider; there is no default publisher.
     * This callback still supplies the missing physical/native route policy. */
    int (*gpio_irq_nub)(void *, void *lease, void **io_service, unsigned *source);
    /* Prove all physical aliases of these descriptor-owned pages are Normal-NC
     * and mutually exclusive with every other allocation in this lease. The
     * adapter independently checks actual PAR attributes of its own CPU alias.
     * A cache option passed to IOBufferMemoryDescriptor alone is insufficient. */
    int (*physical_aliases_nc)(void *, void *lease, const void *cpu, uint64_t bytes,
        const struct dre_tn_smmu_page *, unsigned pages);
    /* Actual native shared-SMMU resource policy, NOT map/walk/unmap stubs.
     * Reserve an unused NS CB/SMR/ASID/context-fault IRQ on apps QSMMU-v500,
     * and a real held initialized TCU/anoc_1 TBU/global-owner transaction.
     * Evidence contains actual register readbacks and that owner's epoch.
     * All SID0x16 producers must be quiescent before domain construction.
     * A partial failure returns its retained nonnull domain lease as usual.
     * The concrete component now owns table construction, exact SID routing,
     * page walks, RO/RW/NX mappings, TLBIASID/TLBSYNC and bounded restoration.
     * Never fill evidence/epoch from DT, or substitute GPU SID0 ownership. */
    int (*reserve_domain)(void *, void *lease, const struct dre_tn_smmu_profile *,
        void **domain_lease, struct dre_tn_smmu_reservation *evidence);
    int (*check_domain)(void *, void *lease, void *domain_lease,
        const struct dre_tn_smmu_reservation *, int require_quiet);
    int (*release_domain)(void *, void *lease, void *domain_lease);
    /* Drop only this owner's policy refs, preserving shared requirements. */
    int (*release)(void *, void *lease);
};

/* hid_service is the real registered SweetHIDTouch IOService, not a readiness
 * flag or replacement HID implementation. Adapter retains it and uses the
 * existing SweetHID batch fragment. out_owner receives a retained kernel owner
 * even on error if any native lease or uncertain resource remains. Caller must
 * preserve that owner; never release it because publish returned an error.
 * No automatic publish/start hook exists in the default kernel. */
int dre_touch_xnu_publish(const struct dre_touch_xnu_provider *, void *hid_service,
                           void **out_owner);
/* External task context, outside the touch work loop. Joins native IRQ and
 * worker dispatch, submits cancellation, retires GPI, then actual DMA mappings.
 * Nonzero leaves the owner retained. A zero return consumes *owner and sets it
 * NULL. Ordinary IOService stop cannot discard quarantined hardware resources. */
int dre_touch_xnu_shutdown(void **owner);
#ifdef __cplusplus
}
#endif
#endif
