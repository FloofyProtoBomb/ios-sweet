/* SPDX-License-Identifier: MIT; synthetic power-controller model only. */
#include "a619_holi_power.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const uint64_t addresses[]={DRE_A619_GCC_CFG,DRE_A619_CX_GDSCR,DRE_A619_GX_GDSCR,
    DRE_A619_AHB_CLK,DRE_A619_GFX_CLK,DRE_A619_GMU_CLK,DRE_A619_CXO_CLK,
    DRE_A619_SPTPRAC_STATUS,DRE_A619_SPTPRAC_CONTROL};
struct fixture {
    struct dre_a619_holi_power state;
    uint32_t values[9];
    unsigned writes, reads, guards, barriers;
    uint64_t ticks, read_failure;
    unsigned write_failure;
    int on_timeout, off_timeout, ownership_lost, clock_stuck, lose_after_on;
};
static int guard(void *cookie)
{ struct fixture *f=cookie; f->guards++; return !f->ownership_lost; }
static int read32(void *cookie,uint64_t address,uint32_t *value)
{
    struct fixture *f=cookie; f->reads++;
    if (address==f->read_failure) { f->read_failure=0; return 0; }
    for (unsigned i=0;i<9;i++) if (address==addresses[i]) { *value=f->values[i]; return 1; }
    assert(!"unexpected MMIO read"); return 0;
}
static int write32(void *cookie,uint64_t address,uint32_t value)
{
    struct fixture *f=cookie; f->writes++;
    assert(address==DRE_A619_SPTPRAC_CONTROL || address==DRE_A619_GX_GDSCR);
    if (address==DRE_A619_GX_GDSCR) {
        /* The controller may change unrelated bits; driver preserves them. */
        assert((value&~(1u<<11))==(f->values[2]&~(1u<<11)));
        f->values[2]=value;
    } else {
        assert(value==0x00778000 || value==0x00778001);
        f->values[8]=value;
        if (value==0x00778000) {
            if (!f->on_timeout) f->values[7]=8;
            if (f->lose_after_on) f->ownership_lost=1;
        } else if (!f->off_timeout) f->values[7]=0x14;
    }
    return f->writes!=f->write_failure;
}
static void barrier(void *cookie) { ((struct fixture *)cookie)->barriers++; }
static uint64_t now(void *cookie)
{ struct fixture *f=cookie; if (!f->clock_stuck) f->ticks+=48000; return f->ticks; }
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));
    f->state=(struct dre_a619_holi_power){.chip_id=DRE_A619_CHIP_ID,
        .prerequisites=DRE_A619_POWER_PREREQUISITES,.tick_hz=19200000};
    const uint32_t values[]={1,0x80000001,0x80000201,1,1,1,1,0x14,0x00778001};
    memcpy(f->values,values,sizeof(values));
}
static struct dre_a619_ops ops(struct fixture *f)
{ return (struct dre_a619_ops){guard,read32,write32,barrier,now,f}; }
static const char *enable(struct fixture *f)
{ struct dre_a619_ops o=ops(f); return dre_a619_holi_sptprac_enable(&f->state,&o); }
static const char *release(struct fixture *f)
{ struct dre_a619_ops o=ops(f); return dre_a619_holi_sptprac_release(&f->state,&o); }
int main(void)
{
    struct fixture f; unsigned checks=0;
#define CHECK(s) assert(!strcmp(enable(&f),(s))); checks++
    init(&f); CHECK("SPTPRAC_ENABLED"); assert(f.writes==1 && f.state.active && f.state.changed);
    f.values[2]|=0x20000; /* unrelated hardware state changed since snapshot */
    assert(!strcmp(release(&f),"SPTPRAC_POWERED_OFF")); checks++;
    assert(f.values[2]==0x80020201 && f.values[7]==0x14 && !f.state.changed && !f.state.unsafe);
    init(&f); f.values[2]|=1u<<11; CHECK("SPTPRAC_ENABLED");
    assert(!strcmp(release(&f),"SPTPRAC_POWERED_OFF")); assert(f.values[2]&(1u<<11)); checks++;
    init(&f); f.values[7]=8; CHECK("SPTPRAC_ALREADY_ON");
    assert(!strcmp(release(&f),"NO_OWNED_POWER_CHANGE") && !f.writes); checks++;
    init(&f); f.state.chip_id=0; CHECK("WRONG_GPU"); assert(!f.writes && !f.reads);
    for (unsigned bit=0;bit<4;bit++) {
        init(&f); f.state.prerequisites&=~(1u<<bit); CHECK("PLATFORM_POWER_UNAVAILABLE"); assert(!f.writes && !f.reads);
    }
    init(&f); f.state.tick_hz=62500000; CHECK("UNSUPPORTED_TIMER"); assert(!f.reads);
    init(&f); f.ownership_lost=1; CHECK("OWNERSHIP_REJECTED"); assert(!f.writes && !f.reads);
    init(&f); f.values[0]=0; CHECK("CONFIG_CLOCK_OFF"); assert(!f.writes);
    init(&f); f.values[1]=0; CHECK("CX_OFF"); assert(!f.writes);
    init(&f); f.values[2]=0; CHECK("GX_OFF"); assert(!f.writes);
    for (unsigned i=3;i<7;i++) {
        init(&f); f.values[i]=0; CHECK("BRANCH_CLOCK_OFF"); assert(!f.writes);
    }
    init(&f); f.read_failure=addresses[0]; CHECK("CONFIG_READ_FAILED"); assert(!f.writes);
    init(&f); f.read_failure=addresses[1]; CHECK("DOMAIN_READ_FAILED"); assert(!f.writes);
    init(&f); f.read_failure=addresses[3]; CHECK("BRANCH_READ_FAILED"); assert(!f.writes);
    init(&f); f.read_failure=addresses[7]; CHECK("SPTPRAC_READ_FAILED"); assert(!f.writes);
    init(&f); f.values[7]=0; CHECK("SPTPRAC_TRANSITIONING"); assert(!f.writes);
    init(&f); f.values[7]=12; CHECK("SPTPRAC_TRANSITIONING"); assert(!f.writes);
    init(&f); f.on_timeout=1; CHECK("POWER_TIMEOUT_RECOVERED");
    assert(f.values[7]==0x14 && !f.state.active && !f.state.unsafe);
    init(&f); f.write_failure=1; CHECK("POWER_WRITE_FAILED_RECOVERED"); assert(!f.state.unsafe);
    init(&f); f.on_timeout=1; f.off_timeout=1; f.values[7]=0x10;
    CHECK("SPTPRAC_TRANSITIONING"); /* refuses ambiguous initial state before stores */
    init(&f); f.lose_after_on=1; CHECK("POWER_STATE_UNSAFE"); assert(f.writes==1 && f.state.unsafe);
    unsigned previous=f.writes; CHECK("RESET_REQUIRED"); assert(f.writes==previous);
    init(&f); CHECK("SPTPRAC_ENABLED"); f.off_timeout=1;
    assert(!strcmp(release(&f),"POWER_STATE_UNSAFE") && f.state.unsafe); checks++;
    init(&f); CHECK("SPTPRAC_ENABLED"); f.write_failure=2;
    assert(!strcmp(release(&f),"POWER_STATE_UNSAFE") && f.state.unsafe); checks++;
    init(&f); f.on_timeout=1; f.clock_stuck=1; CHECK("POWER_TIMEOUT_RECOVERED");
    init(&f); CHECK("SPTPRAC_ENABLED"); f.state.prerequisites=0;
    unsigned writes=f.writes;
    assert(!strcmp(release(&f),"POWER_STATE_UNSAFE") && f.state.unsafe && f.writes==writes); checks++;
    printf("PASS %u bounded Holi SPTPRAC ownership/power/recovery cases; synthetic hardware only\n",checks);
}
