/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only clock acquisition. */
#pragma once
#include "rpm_mmio.h"
#ifdef __cplusplus
extern "C" {
#endif
struct dre_holi_gcc_policy {
    void *cookie;
    /* Acquire real GPLL0/rate/XO and interconnect/aggregate-source references
     * for these exact named parents under the same native board mutex. The
     * actual RPM ACK holds already exist; DT levels cannot qualify them.
     * A partial retained source reference MUST return a genuine nonzero hold.
     * No token may be minted from a counter, register, DT or GPU lease here. */
    int (*acquire)(void *,uint64_t lease,const struct dre_rpm_votes *,
        const uint32_t observed_gpll0[6],uint64_t *hold);
    /* Status0: actual stable parent-rate/source/bus/vote owner remains held.
     * Nonrecursive, no register writes or new policy acquisition. */
    int (*check)(void *,uint64_t lease,uint64_t hold);
    /* Actual global APSS shared-register update serialization, in canonical
     * board-owner -> GCC-register order. It must exclude EVERY competing
     * writer, not just this helper. Success holds a sleepable native mutex
     * compatible with guarded IOKit callbacks; no IRQ/spinlock acquisition.
     * Failure must leave no register mutex held. This local code supplies no
     * fabricated controller or advisory lock standing in for that authority. */
    int (*lock_register)(void *,uint64_t lease,uint64_t hold,uint64_t address,uint32_t mask);
    void (*unlock_register)(void *,uint64_t lease,uint64_t hold,uint64_t address,uint32_t mask);
};
struct dre_holi_gcc_hold {
    struct dre_rpm_mmio_ops io;
    struct dre_holi_gcc_policy policy;
    const struct dre_rpm_votes *votes;
    uint64_t lease,source_hold,writes;
    uint32_t gpll0[6],before[4],last[4];
    uint32_t attempted,active,unsafe;
    const char *error;
};
/* Qualified existing GPLL0600MHz/div2 only. Actual parent/source policy and
 * RPM ACKs are mandatory. OR only three named GCC branch bit0 controls and
 * GPLL0-div APSS vote bit16; no PLL/RCG/GDSC/reset/voltage/rate programming.
 * Other register bits and foreign clocks remain preserved. Per-stage budget<=1s at the
 * actual19.2MHz counter. No automatic rollback/off/release API exists. */
const char *dre_holi_gcc_acquire(struct dre_holi_gcc_hold *,
    const struct dre_rpm_mmio_ops *,const struct dre_holi_gcc_policy *,
    const struct dre_rpm_votes *,uint64_t budget_ticks);
/* Read-only live requalification, never reacquires/toggles a clock. Failure
 * retains all source references and possibly effective enable stores. */
const char *dre_holi_gcc_validate(struct dre_holi_gcc_hold *);
#ifdef __cplusplus
}
#endif
