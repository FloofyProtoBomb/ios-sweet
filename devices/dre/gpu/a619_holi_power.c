/* SPDX-License-Identifier: MIT
 * The hardware protocol/constants are from a6xx_holi_gmu_sptprac_enable /
 * disable and gpucc-holi.c at LineageOS commit
 * 98a38783f42f5588bf00cd933e299aedc589bc10. */
#include "a619_holi_power.h"
#define POWER_ON (1u << 31)
#define RETENTION (1u << 11)
#define SPT_ON 0x00778000u
#define SPT_OFF 0x00778001u
#define STATUS_ON (1u << 3)
#define STATUS_OFF (1u << 2)
#define SP_CLOCK_OFF (1u << 4)

static int valid(const struct dre_a619_holi_power *s, const struct dre_a619_ops *o)
{
    return s && o && o->guard && o->read32 && o->write32 && o->barrier && o->now;
}
static int branch(uint32_t value, int hwcg)
{ return (value & 1) && (!(value & POWER_ON) || (hwcg && (value & 2))); }
static int read_reg(const struct dre_a619_ops *o, uint64_t a, uint32_t *v)
{ return o->read32(o->cookie,a,v); }
static int store(const struct dre_a619_ops *o, uint64_t a, uint32_t v)
{
    if (!o->guard(o->cookie)) return 0;
    int ok=o->write32(o->cookie,a,v);
    o->barrier(o->cookie);
    return ok;
}
static int wait_status(const struct dre_a619_ops *o, uint32_t mask, uint32_t expect)
{
    uint64_t start=o->now(o->cookie);
    for (unsigned polls=0;polls<1000000;polls++) {
        uint32_t value;
        if (!o->guard(o->cookie) || !read_reg(o,DRE_A619_SPTPRAC_STATUS,&value)) return 0;
        if ((value & mask)==expect) return 1;
        if (o->now(o->cookie)-start >= 192000) return 0; /* 10ms at checked19.2MHz */
    }
    return 0;
}
static int power_off(struct dre_a619_holi_power *s, const struct dre_a619_ops *o)
{
    uint32_t value;
    if (!o->guard(o->cookie) || !read_reg(o,DRE_A619_GX_GDSCR,&value) ||
        !store(o,DRE_A619_GX_GDSCR,value|RETENTION) ||
        !store(o,DRE_A619_SPTPRAC_CONTROL,SPT_OFF) ||
        !wait_status(o,STATUS_OFF,STATUS_OFF)) return 0;
    /* Restore only the retention bit this transaction changed. Preserve every
     * other current GDSC bit; never replay a stale whole-register snapshot. */
    if (!read_reg(o,DRE_A619_GX_GDSCR,&value) ||
        !store(o,DRE_A619_GX_GDSCR,(value&~RETENTION)|(s->retention_before&RETENTION))) return 0;
    s->active=0; s->changed=0;
    return o->guard(o->cookie);
}
const char *dre_a619_holi_sptprac_enable(struct dre_a619_holi_power *s,
    const struct dre_a619_ops *o)
{
    if (!valid(s,o)) return "INVALID_ARGUMENT";
    if (s->unsafe) return "RESET_REQUIRED";
    if (s->chip_id != DRE_A619_CHIP_ID) return "WRONG_GPU";
    if (s->prerequisites != DRE_A619_POWER_PREREQUISITES) return "PLATFORM_POWER_UNAVAILABLE";
    if (s->tick_hz != 19200000) return "UNSUPPORTED_TIMER";
    if (s->active || s->changed) return "STATE_ALREADY_ACTIVE";
    if (!o->guard(o->cookie)) return "OWNERSHIP_REJECTED";
    uint32_t value;
    if (!read_reg(o,DRE_A619_GCC_CFG,&value)) return "CONFIG_READ_FAILED";
    if (!branch(value,1)) return "CONFIG_CLOCK_OFF";
    if (!read_reg(o,DRE_A619_CX_GDSCR,&value)) return "DOMAIN_READ_FAILED";
    if (!(value&POWER_ON)) return "CX_OFF";
    if (!read_reg(o,DRE_A619_GX_GDSCR,&s->retention_before)) return "DOMAIN_READ_FAILED";
    if (!(s->retention_before&POWER_ON)) return "GX_OFF";
    const uint64_t clocks[]={DRE_A619_AHB_CLK,DRE_A619_GFX_CLK,DRE_A619_GMU_CLK,DRE_A619_CXO_CLK};
    for (unsigned i=0;i<4;i++) {
        if (!read_reg(o,clocks[i],&value)) return "BRANCH_READ_FAILED";
        if (!branch(value,0)) return "BRANCH_CLOCK_OFF";
    }
    if (!read_reg(o,DRE_A619_SPTPRAC_STATUS,&s->status_before) ||
        !read_reg(o,DRE_A619_SPTPRAC_CONTROL,&s->control_before)) return "SPTPRAC_READ_FAILED";
    if (!(s->status_before&(STATUS_OFF|SP_CLOCK_OFF)) && (s->status_before&STATUS_ON)) {
        s->active=1; return "SPTPRAC_ALREADY_ON";
    }
    if (!(s->status_before&STATUS_OFF) || (s->status_before&STATUS_ON)) return "SPTPRAC_TRANSITIONING";
    if (!o->guard(o->cookie)) return "OWNERSHIP_CHANGED";
    s->changed=1; /* A failed write may still reach the device. */
    int written=store(o,DRE_A619_SPTPRAC_CONTROL,SPT_ON);
    if (written && wait_status(o,STATUS_ON|STATUS_OFF|SP_CLOCK_OFF,STATUS_ON) && o->guard(o->cookie)) {
        s->active=1; return "SPTPRAC_ENABLED";
    }
    if (!power_off(s,o)) { s->unsafe=1; return "POWER_STATE_UNSAFE"; }
    return written ? "POWER_TIMEOUT_RECOVERED" : "POWER_WRITE_FAILED_RECOVERED";
}
const char *dre_a619_holi_sptprac_release(struct dre_a619_holi_power *s,
    const struct dre_a619_ops *o)
{
    if (!valid(s,o)) return "INVALID_ARGUMENT";
    if (s->unsafe) return "RESET_REQUIRED";
    if (!s->changed) { s->active=0; return "NO_OWNED_POWER_CHANGE"; }
    if (s->chip_id != DRE_A619_CHIP_ID ||
        s->prerequisites != DRE_A619_POWER_PREREQUISITES ||
        s->tick_hz != 19200000) {
        s->unsafe=1; return "POWER_STATE_UNSAFE";
    }
    if (!power_off(s,o)) { s->unsafe=1; return "POWER_STATE_UNSAFE"; }
    return "SPTPRAC_POWERED_OFF";
}
