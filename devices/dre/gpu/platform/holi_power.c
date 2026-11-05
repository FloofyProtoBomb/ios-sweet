/* SPDX-License-Identifier: MIT
 * Original restricted provider; exact primary ABI is pinned in sources.json.
 * No PLL/RCG programming, regulator request, IRQ/reset-controller takeover,
 * interconnect vote, firmware or GPU command is hidden behind a ready flag. */
#include "holi_power.h"
#include <string.h>
#define ON (1u<<31)
#define COLLAPSE 1u
#define UNSUPPORTED_GDSC 6u
#define POLLS 1000000u
static const uint64_t addresses[HOLI_REGISTER_COUNT]={
    0x01436004,0x01471154,0x0143600c,0x01479004,
    0x01400000,0x01400004,0x01400008,0x0140000c,0x0140002c,0x01400038,
    0x05990000,0x05990004,0x05990008,0x0599000c,0x05990010,
    0x05990014,0x05990018,0x0599001c,0x05990020,0x0599002c,0x05990038,
    0x0599101c,0x05991020,0x05991120,0x05991124,
    0x0599106c,0x05991540,0x0599100c,0x05991008,0x05991160,0x05998004,0x05991508,
    0x05991078,0x05991060,0x0599109c,0x05991098,0x05991054
};
static const char *names[HOLI_REGISTER_COUNT]={
    "GCC_GPU_CFG_AHB","GCC_BIMC_GPU_AXI","GCC_GPU_MEMNOC","GCC_GPU_GPLL0_VOTES",
    "GCC_GPLL0_MODE","GCC_GPLL0_L","GCC_GPLL0_CAL","GCC_GPLL0_USER","GCC_GPLL0_OPMODE","GCC_GPLL0_FRAC",
    "GPU_PLL0_MODE","GPU_PLL0_L","GPU_PLL0_CAL","GPU_PLL0_USER","GPU_PLL0_USER_HI",
    "GPU_PLL0_CONFIG","GPU_PLL0_CONFIG_HI","GPU_PLL0_TEST","GPU_PLL0_TEST_HI","GPU_PLL0_OPMODE","GPU_PLL0_FRAC",
    "GFX_CMD","GFX_CFG","GMU_CMD","GMU_CFG","CX_GDSCR","CX_HW_STATUS","GX_GDSCR",
    "GX_SW_RESET","GX_ACD_RESET","GX_ACD_MISC_RESET","GX_DOMAIN","GPU_AHB","GPU_GX_CXO","GPU_CXO","GPU_CX_GMU","GPU_GX_GFX"
};
uint64_t dre_holi_register_address(unsigned i) { return i<HOLI_REGISTER_COUNT?addresses[i]:0; }
const char *dre_holi_register_name(unsigned i) { return i<HOLI_REGISTER_COUNT?names[i]:"UNKNOWN"; }
static int fail(struct dre_holi_power *s,const char *error)
{ s->error=error;s->active=0;if(s->changed)s->unsafe=1;return 0; }
static int args(const struct dre_rpm_mmio_ops *o)
{ return o && o->lease && o->read32 && o->write32 && o->barrier && o->now; }
static int branch_parent(uint32_t value)
{ return !(value&(1u<<22)) && (value&1) && ((value&2) || !(value&ON)); }
static int holds(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,const struct dre_rpm_votes *r)
{
    if(s->unsafe || !s->lease || o->lease(o->cookie)!=s->lease)return fail(s,"HOLI_OWNERSHIP_LOST");
    if(!r || !r->link.open || r->link.unsafe || r->unsafe || r->pending || r->link.lease!=s->lease)return fail(s,"HOLI_RPM_HOLDS_LOST");
    const uint32_t types[]={0x78637772,0x786d7772,0x78677772};
    for(unsigned i=0;i<3;i++)if(r->resource[i].type!=types[i] || r->resource[i].id ||
        r->acked_id[i]!=s->acked_id[i] || r->held_level[i]!=s->held_level[i] ||
        r->acked_id[i]<2 || r->acked_id[i]>=r->next_id || !dre_rpm_level_mask(r->held_level[i]) ||
        !(r->resource[i].allowed_levels&dre_rpm_level_mask(r->held_level[i])))return fail(s,"HOLI_RAIL_ACK_CHANGED");
    if(r->acked_id[0]==r->acked_id[1] || r->acked_id[0]==r->acked_id[2] || r->acked_id[1]==r->acked_id[2])return fail(s,"HOLI_RAIL_ACK_REUSED");
    return 1;
}
static int read_reg(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,const struct dre_rpm_votes *r,unsigned index,uint32_t *value)
{
    if(index>=HOLI_REGISTER_COUNT)return fail(s,"HOLI_REGISTER_RANGE");
    s->last_read_index=index;
    if(!holds(s,o,r)) { s->failed_index=index;return 0; }
    if(index>=HOLI_PLL_MODE) {
        uint32_t gate;
        if(!o->read32(o->cookie,addresses[HOLI_GCC_CFG],&gate)) { s->failed_index=HOLI_GCC_CFG;return fail(s,"HOLI_CONFIG_GATE_READ_FAILED"); }
        o->barrier(o->cookie);
        if(!holds(s,o,r) || !branch_parent(gate))return fail(s,"HOLI_CONFIG_BUS_CLOCK_LOST");
    }
    if(!o->read32(o->cookie,addresses[index],value)) { s->failed_index=index;return fail(s,"HOLI_REGISTER_READ_FAILED"); }
    o->barrier(o->cookie);
    if(!(s->valid_reads&(UINT64_C(1)<<index)))s->before[index]=*value;
    s->valid_reads|=UINT64_C(1)<<index;s->last[index]=*value;
    return holds(s,o,r);
}
static int delay(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,const struct dre_rpm_votes *r,uint32_t us)
{
    uint64_t start=o->now(o->cookie),ticks=((uint64_t)us*192+9)/10;
    for(unsigned n=0;n<POLLS;n++) {
        if(!holds(s,o,r))return 0;
        uint64_t now=o->now(o->cookie);
        if(now<start)return fail(s,"HOLI_COUNTER_REVERSED");
        if(now-start>=ticks)return 1;
    }
    return fail(s,"HOLI_COUNTER_STOPPED");
}
static int store_mask(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,const struct dre_rpm_votes *r,
    unsigned index,uint32_t mask,uint32_t value)
{
    uint32_t old,current;
    if(!read_reg(s,o,r,index,&old))return 0;
    uint32_t wanted=(old&~mask)|(value&mask);
    if(wanted==old)return 1;
    if(!holds(s,o,r))return 0;
    s->changed=1;s->writes++;s->last_write_index=index;
    int ok=o->write32(o->cookie,addresses[index],wanted);o->barrier(o->cookie);
    if(!ok) { s->failed_index=index;return fail(s,"HOLI_STORE_UNCERTAIN"); }
    if(!read_reg(s,o,r,index,&current))return 0;
    if((current&mask)!=(wanted&mask)) { s->failed_index=index;return fail(s,"HOLI_STORE_READBACK_FAILED"); }
    return 1;
}
static int poll_on(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,const struct dre_rpm_votes *r,
    unsigned index,unsigned us,int branch)
{
    uint64_t start=o->now(o->cookie),budget=((uint64_t)us*192+9)/10;
    for(unsigned n=0;n<POLLS;n++) {
        uint32_t value;if(!read_reg(s,o,r,index,&value))return 0;
        uint64_t now=o->now(o->cookie);
        if(now<start || now-start>=budget)return fail(s,branch?"HOLI_BRANCH_STATUS_TIMEOUT":"HOLI_GDSC_STATUS_TIMEOUT");
        if(branch?(!(value&ON) || (value&0xf0000000u)==0x20000000u):!!(value&ON))return 1;
        if(!delay(s,o,r,1))return 0;
    }
    return fail(s,"HOLI_STATUS_POLL_LIMIT");
}
static int config_valid(const struct dre_holi_power_config *c)
{
    if(!c || c->chip_id!=DRE_HOLI_CHIP || c->timer_hz!=DRE_RPM_TIMER_HZ || c->xo_hz!=19200000 ||
        c->gfx_nominal_hz!=253000000 || c->gmu_nominal_hz!=200000000 || c->variant_features!=3 ||
        c->cx_timeout_us!=500 || c->gx_timeout_us!=100 || c->cx_clk_dis_wait!=8 ||
        c->gpucc_base!=DRE_HOLI_GPUCC || c->gcc_base!=DRE_HOLI_GCC || c->cx_gdscr!=0x599106c ||
        c->cx_status!=0x5991540 || c->gx_gdscr!=0x599100c || c->gx_sw_reset!=0x5991008 ||
        c->gx_acd_reset!=0x5991160 || c->gx_misc_reset!=0x5998004 || c->gx_domain!=0x5991508)return 0;
    for(unsigned i=0;i<3;i++)if(c->aggregate_floor[i]<(i==2?64u:384u) || !dre_rpm_level_mask(c->aggregate_floor[i]))return 0;
    return 1;
}
static int sources(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,const struct dre_rpm_votes *r)
{
    uint32_t value[HOLI_GMU_CFG+1];
    for(unsigned i=0;i<=HOLI_GMU_CFG;i++)if(!read_reg(s,o,r,i,&value[i]))return 0;
    if(!branch_parent(value[HOLI_GCC_CFG]) || !branch_parent(value[HOLI_GCC_AXI]) || !branch_parent(value[HOLI_GCC_MEM]) ||
        !(value[HOLI_GCC_GPU_PLL_VOTE]&(1u<<16)))return fail(s,"HOLI_PARENT_CLOCK_HOLD_REQUIRED");
    if((value[HOLI_GPLL_MODE]&0xc0100000u)!=0xc0100000u || (value[HOLI_GPLL_MODE]&((1u<<22)|(1u<<7))) ||
        value[HOLI_GPLL_L]!=31 || value[HOLI_GPLL_FRAC]!=0x4000 || !value[HOLI_GPLL_CAL] ||
        (value[HOLI_GPLL_USER]&0xf07)!=0x107 || value[HOLI_GPLL_OPMODE]!=1)return fail(s,"HOLI_GPLL0_600MHZ_UNQUALIFIED");
    if((value[HOLI_PLL_MODE]&0x80800005u)!=0x80800005u ||
        (value[HOLI_PLL_MODE]&((1u<<20)|(1u<<22)|(1u<<7))) || value[HOLI_PLL_L]!=26 ||
        (value[HOLI_PLL_FRAC]!=0x5aaa && value[HOLI_PLL_FRAC]!=0x5aab) || value[HOLI_PLL_CAL]!=0x3f ||
        value[HOLI_PLL_USER]!=7 || value[HOLI_PLL_USER_HI]!=0x4805 || value[HOLI_PLL_CONFIG]!=0x20485699 ||
        value[HOLI_PLL_CONFIG_HI]!=0x2067 || value[HOLI_PLL_TEST]!=0x40000000 || value[HOLI_PLL_TEST_HI]!=2 ||
        value[HOLI_PLL_OPMODE]!=1)return fail(s,"HOLI_GPU_PLL0_506MHZ_UNQUALIFIED");
    if(value[HOLI_GFX_CFG]!=0x103 || value[HOLI_GMU_CFG]!=0x602 ||
        (value[HOLI_GFX_CMD]&~0x80000002u) || (value[HOLI_GMU_CMD]&~0x80000002u))return fail(s,"HOLI_RCG_RATE_UNQUALIFIED");
    s->observed_gpu_hz=(UINT64_C(19200000)*26+((UINT64_C(19200000)*value[HOLI_PLL_FRAC])>>16))/2;
    s->observed_gmu_hz=UINT64_C(200000000);
    return 1;
}
static int enable_domain(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,const struct dre_rpm_votes *r,unsigned gx)
{
    unsigned reg=gx?HOLI_GX_GDSCR:HOLI_CX_GDSCR,status=gx?HOLI_GX_GDSCR:HOLI_CX_STATUS;
    uint32_t value,status_value;
    if(!read_reg(s,o,r,reg,&value) || !read_reg(s,o,r,status,&status_value))return 0;
    if((value&UNSUPPORTED_GDSC) || (!!(value&COLLAPSE)==!!(status_value&ON)))return fail(s,"HOLI_DOMAIN_OWNER_CHANGED");
    if(!(value&COLLAPSE)) { s->borrowed_domains|=1u<<gx;return 1; }
    s->phase=gx?3:2;
    if(gx) {
        const unsigned resets[]={HOLI_GX_SW_RESET,HOLI_GX_ACD_RESET,HOLI_GX_MISC_RESET};
        for(unsigned i=0;i<3;i++) {
            if(!read_reg(s,o,r,resets[i],&value))return 0;
            if(value&1)return fail(s,"HOLI_RESET_OWNER_CHANGED");
        }
        for(unsigned i=0;i<3;i++)if(!store_mask(s,o,r,resets[i],1,1))return 0;
        if(!delay(s,o,r,100))return 0;
        for(unsigned i=0;i<3;i++)if(!store_mask(s,o,r,resets[i],1,0))return 0;
        if(!store_mask(s,o,r,HOLI_GX_DOMAIN,16,16) || !delay(s,o,r,1) ||
            !store_mask(s,o,r,HOLI_GX_DOMAIN,16,0) || !store_mask(s,o,r,HOLI_GX_DOMAIN,1,0))return 0;
    } else if(!store_mask(s,o,r,reg,0xf000,0x8000))return 0;
    if(!store_mask(s,o,r,reg,COLLAPSE,0) || !delay(s,o,r,1) ||
        !poll_on(s,o,r,status,gx?100:1000,0) || !delay(s,o,r,2))return 0;
    return 1;
}
const char *dre_holi_power_acquire(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,
    const struct dre_rpm_votes *r,const struct dre_holi_power_config *cfg)
{
    if(!s || !args(o) || !r)return "HOLI_INVALID_ARGUMENT";
    if(s->unsafe || s->changed || s->active)return "HOLI_FRESH_OWNER_REQUIRED";
    if(!config_valid(cfg))return "HOLI_PROFILE_OR_AGGREGATE_UNQUALIFIED";
    memset(s,0,sizeof(*s));s->lease=o->lease(o->cookie);s->phase=1;
    s->last_read_index=s->last_write_index=s->failed_index=UINT32_MAX;
    for(unsigned i=0;i<3;i++) {
        s->acked_id[i]=r->acked_id[i];s->held_level[i]=r->held_level[i];
        if(r->held_level[i]<cfg->aggregate_floor[i])return s->error="HOLI_AGGREGATE_ACK_HOLD_REQUIRED";
    }
    if(!holds(s,o,r))return s->error;
    uint32_t value;
    if(!read_reg(s,o,r,HOLI_GCC_CFG,&value))return s->error;
    if(!branch_parent(value))return s->error="HOLI_CONFIG_BUS_CLOCK_REQUIRED";
    if(!sources(s,o,r))return s->error;
    for(unsigned i=HOLI_CX_GDSCR;i<HOLI_REGISTER_COUNT;i++)if(!read_reg(s,o,r,i,&value))return s->error;
    if((s->last[HOLI_CX_GDSCR]|s->last[HOLI_GX_GDSCR])&UNSUPPORTED_GDSC)return s->error="HOLI_GDSC_HW_OR_OVERRIDE_OWNER";
    for(unsigned gx=0;gx<2;gx++) {
        unsigned reg=gx?HOLI_GX_GDSCR:HOLI_CX_GDSCR,status=gx?HOLI_GX_GDSCR:HOLI_CX_STATUS;
        int collapse=!!(s->last[reg]&COLLAPSE),on=!!(s->last[status]&ON);
        if(collapse==on)return s->error="HOLI_GDSC_TRANSITIONING";
    }
    if((s->last[HOLI_GX_SW_RESET]|s->last[HOLI_GX_ACD_RESET]|s->last[HOLI_GX_MISC_RESET])&1 ||
        (s->last[HOLI_GX_DOMAIN]&16))return s->error="HOLI_INHERITED_RESET_ASSERTED";
    if(!(s->last[HOLI_CX_GDSCR]&1) && (s->last[HOLI_CX_GDSCR]&0xf000)!=0x8000)return s->error="HOLI_INHERITED_CX_WAIT_UNQUALIFIED";
    if(!(s->last[HOLI_GX_GDSCR]&1) && (s->last[HOLI_GX_DOMAIN]&1))return s->error="HOLI_INHERITED_GX_CLAMPED";
    for(unsigned i=HOLI_AHB;i<HOLI_REGISTER_COUNT;i++)if(s->last[i]&(1u<<22))return s->error="HOLI_BRANCH_FORCE_OFF";
    if(!enable_domain(s,o,r,0) || !sources(s,o,r) || !enable_domain(s,o,r,1))return s->error;
    s->phase=4;
    for(unsigned i=HOLI_AHB;i<HOLI_REGISTER_COUNT;i++) {
        if(!sources(s,o,r))return s->error;
        if(s->last[i]&1)s->borrowed_branches|=1u<<(i-HOLI_AHB);
        if(!store_mask(s,o,r,i,1,1))return s->error;
        /* Published delay branches require10us; HALT branches poll<=200us.
         * This restricted provider additionally observes branch2 ON for every
         * acquired branch, including DELAY branches. No bit-only success. */
        if((i==HOLI_AHB || i==HOLI_GX_CXO || i==HOLI_GX_GFX) && !delay(s,o,r,10))return s->error;
        if(!poll_on(s,o,r,i,200,1))return s->error;
    }
    s->active=1;s->phase=5;
    return dre_holi_power_validate(s,o,r);
}
const char *dre_holi_power_validate(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,const struct dre_rpm_votes *r)
{
    if(!s || !args(o) || !s->active)return "HOLI_NOT_ACQUIRED";
    if(!holds(s,o,r) || !sources(s,o,r))return s->error;
    uint32_t value;
    if((s->last[HOLI_GFX_CMD]|s->last[HOLI_GMU_CMD])&ON)return fail(s,"HOLI_RCG_ROOT_NOT_OBSERVED"),s->error;
    if(!read_reg(s,o,r,HOLI_CX_STATUS,&value) || !(value&ON) ||
        !read_reg(s,o,r,HOLI_GX_GDSCR,&value) || !(value&ON))return fail(s,"HOLI_DOMAIN_HOLD_LOST"),s->error;
    if(!read_reg(s,o,r,HOLI_CX_GDSCR,&value) || (value&7) ||
        !read_reg(s,o,r,HOLI_GX_GDSCR,&value) || (value&7))return fail(s,"HOLI_DOMAIN_CONTROL_LOST"),s->error;
    if(!read_reg(s,o,r,HOLI_GX_DOMAIN,&value))return s->error;
    if(value&17)return fail(s,"HOLI_DOMAIN_CLAMP_OR_RESET_LOST"),s->error;
    for(unsigned i=HOLI_GX_SW_RESET;i<=HOLI_GX_MISC_RESET;i++) {
        if(!read_reg(s,o,r,i,&value))return s->error;
        if(value&1)return fail(s,"HOLI_RESET_HOLD_LOST"),s->error;
    }
    for(unsigned i=HOLI_AHB;i<HOLI_REGISTER_COUNT;i++) {
        if(!read_reg(s,o,r,i,&value))return s->error;
        if(!(value&1) || (value&(1u<<22)) || ((value&ON) && (value&0xf0000000u)!=0x20000000u))return fail(s,"HOLI_BRANCH_HOLD_LOST"),s->error;
    }
    return s->error="HOLI_DOMAINS_BRANCHES_ACQUIRED";
}
