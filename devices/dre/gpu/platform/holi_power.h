/* SPDX-License-Identifier: MIT */
#pragma once
#include "rpm_mmio.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_HOLI_GPUCC UINT64_C(0x05990000)
#define DRE_HOLI_GCC UINT64_C(0x01400000)
#define DRE_HOLI_CHIP UINT32_C(0x06010900)
enum dre_holi_register {
    HOLI_GCC_CFG,HOLI_GCC_AXI,HOLI_GCC_MEM,HOLI_GCC_GPU_PLL_VOTE,
    HOLI_GPLL_MODE,HOLI_GPLL_L,HOLI_GPLL_CAL,HOLI_GPLL_USER,HOLI_GPLL_OPMODE,HOLI_GPLL_FRAC,
    HOLI_PLL_MODE,HOLI_PLL_L,HOLI_PLL_CAL,HOLI_PLL_USER,HOLI_PLL_USER_HI,
    HOLI_PLL_CONFIG,HOLI_PLL_CONFIG_HI,HOLI_PLL_TEST,HOLI_PLL_TEST_HI,HOLI_PLL_OPMODE,HOLI_PLL_FRAC,
    HOLI_GFX_CMD,HOLI_GFX_CFG,HOLI_GMU_CMD,HOLI_GMU_CFG,
    HOLI_CX_GDSCR,HOLI_CX_STATUS,HOLI_GX_GDSCR,HOLI_GX_SW_RESET,HOLI_GX_ACD_RESET,HOLI_GX_MISC_RESET,HOLI_GX_DOMAIN,
    HOLI_AHB,HOLI_GX_CXO,HOLI_CXO,HOLI_CX_GMU,HOLI_GX_GFX,HOLI_REGISTER_COUNT
};
uint64_t dre_holi_register_address(unsigned index);
const char *dre_holi_register_name(unsigned index);

/* Exact full live-FDT Holi variant, not a caller-controlled readiness mask.
 * variant_features=3 means GX reset-aon +CX no-status-check-on-disable;
 * all unlisted clock/reset/collapse/mailbox/retention variants must be absent.
 * Parent-supply graph must select the same RPM resource rows as the hold
 * object. The native binder must validate that graph before populating this.
 * aggregate_floor is an independently QUALIFIED platform-wide hold, not a
 * GPU-frequency inference. Floors below actual boot/proxy CX/MX384 or GX64
 * are outside this restricted acquisition profile. No default policy exists. */
struct dre_holi_power_config {
    uint32_t chip_id,timer_hz,xo_hz,gfx_nominal_hz,gmu_nominal_hz;
    uint32_t aggregate_floor[3],variant_features,cx_timeout_us,gx_timeout_us,cx_clk_dis_wait;
    uint64_t gpucc_base,gcc_base,cx_gdscr,cx_status,gx_gdscr;
    uint64_t gx_sw_reset,gx_acd_reset,gx_misc_reset,gx_domain;
};
struct dre_holi_power {
    uint64_t lease,valid_reads;
    uint32_t before[HOLI_REGISTER_COUNT],last[HOLI_REGISTER_COUNT];
    uint32_t acked_id[3],held_level[3],phase,writes,borrowed_domains,borrowed_branches;
    uint32_t last_read_index,last_write_index,failed_index;
    uint64_t observed_gpu_hz,observed_gmu_hz;
    int changed,unsafe,active;
    const char *error;
};
/* Own a quiescent GPU with no live/borrowed DMA work, the same serialized
 * exclusive lease as RPM, and all source/bus parents/mappings. Before any
 * mutation this reads/qualifies existing GPU PLL0/RCG
 * 253MHz and GMU 200MHz from existing GCC GPLL0 600MHz/2, plus actual parent
 * branch state. Missing or changing evidence refuses; it NEVER programs a
 * PLL, RCG, GCC vote/rate or bandwidth policy. Actual matched ACK holds for
 * all three aggregate rails are required, not a bit saying rails are ready.
 * Then acquire CX/GX by the published full reset/AON/clamp sequence and only
 * the named/critical GPUCC branch bits. Inherited-on blocks are borrowed and
 * never reset/toggled. Failure after an attempted store poisons the owner:
 * hold every rail/source/mapping; no automatic rollback or off request.
 * This is a partial provider, not complete GPU/SPTPRAC/CP/Metal readiness. */
const char *dre_holi_power_acquire(struct dre_holi_power *,const struct dre_rpm_mmio_ops *,
    const struct dre_rpm_votes *,const struct dre_holi_power_config *);
/* Revalidate still-held resources/status after acquisition; performs no store.
 * Suitable as one component of a later platform_ready callback. Do not free
 * this state, lower holds or resize mappings while any dependent work runs. */
const char *dre_holi_power_validate(struct dre_holi_power *,const struct dre_rpm_mmio_ops *,
    const struct dre_rpm_votes *);
#ifdef __cplusplus
}
#endif
