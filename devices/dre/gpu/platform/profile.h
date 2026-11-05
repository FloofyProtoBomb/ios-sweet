/* SPDX-License-Identifier: MIT -- UNVALIDATED read-only source draft. */
#pragma once
#include "holi_power.h"
#ifdef __cplusplus
extern "C" {
#endif
struct dre_holi_power_profile {
    struct dre_holi_power_config config;
    struct dre_rpm_resource resources[3];
    uint32_t init_level[3],proxy_minimum[3],proxy_maximum[3];
    /* Private original-FDT node identities. Never an IOUserClient payload. */
    int gpu,gpucc,rpm,cx_gdsc,gx_gdsc,regulator[3],resource[3];
};
/* Inspect the complete immutable kernel-owned original Holi FDT. This checks
 * actual supply identities, RPM resource bounds and the exact supported GDSC
 * variant. No allocation, MMIO, ACK, IRQ ownership or hardware action occurs.
 * config.aggregate_floor and resources[].allowed_levels ALWAYS remain zero.
 * Actual independently qualified aggregate policy is still mandatory before
 * these structures may be passed to the power/RPM acquisition routines.
 * A DT proxy/init value is only a source fact, never an inherited live vote. */
const char *dre_holi_power_profile_from_fdt(const void *,size_t,
    struct dre_holi_power_profile *);
#ifdef __cplusplus
}
#endif
