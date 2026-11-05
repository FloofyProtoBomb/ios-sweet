/* SPDX-License-Identifier: MIT */
#pragma once
#include "a619_backend.h"
#ifdef __cplusplus
extern "C" {
#endif

#define DRE_A619_GCC_CFG UINT64_C(0x01436004)
#define DRE_A619_CX_GDSCR UINT64_C(0x0599106c)
#define DRE_A619_CX_STATUS UINT64_C(0x05991540) /* live hw-ctrl-addr */
#define DRE_A619_GX_GDSCR UINT64_C(0x0599100c)
#define DRE_A619_AHB_CLK UINT64_C(0x05991078)
#define DRE_A619_GFX_CLK UINT64_C(0x05991054)
#define DRE_A619_GMU_CLK UINT64_C(0x05991098)
#define DRE_A619_CXO_CLK UINT64_C(0x0599109c)
#define DRE_A619_SPTPRAC_CONTROL UINT64_C(0x0596a204)
#define DRE_A619_SPTPRAC_STATUS UINT64_C(0x0597e340)

/* The platform must actually acquire/verify these before calling enable:
 * regulator corner votes, source clock rates, interconnect/memory clocks and
 * exclusive ownership with all GPU DMA stopped. They are not DT-presence bits. */
#define DRE_A619_RAIL_VOTES (1u << 0)
#define DRE_A619_SOURCE_RATES (1u << 1)
#define DRE_A619_BUS_VOTES (1u << 2)
#define DRE_A619_QUIESCENT_OWNER (1u << 3)
#define DRE_A619_POWER_PREREQUISITES 15u
struct dre_a619_holi_power {
    uint32_t chip_id, prerequisites, tick_hz;
    uint32_t status_before, control_before, retention_before;
    int active, changed, unsafe;
};
/* Implements Holi's published SPTPRAC sequence, not the regulator/clock/RPM
 * providers. Reads prove CX/GX and required branch clocks already on. A fault
 * after the first write attempts the published retention+power-off sequence.
 * Failure to establish that off state marks unsafe; caller must retain all
 * resources and reset the GPU/device. Releasing an already-on borrowed block
 * is a no-op. No SMMU, voltage, PLL, secure-state or media register is written. */
const char *dre_a619_holi_sptprac_enable(struct dre_a619_holi_power *state,
    const struct dre_a619_ops *ops);
const char *dre_a619_holi_sptprac_release(struct dre_a619_holi_power *state,
    const struct dre_a619_ops *ops);
/* UNVALIDATED overnight addition: read-only live-state guard, with no cold
 * CP-idle requirement and no recovery/off writes. Caller retains resources
 * after a failure. Existing enable/release behavior is unchanged. */
const char *dre_a619_holi_sptprac_check_live(const struct dre_a619_holi_power *state,
    const struct dre_a619_ops *ops);
#ifdef __cplusplus
}
#endif
