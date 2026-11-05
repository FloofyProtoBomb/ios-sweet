/* SPDX-License-Identifier: MIT; synthetic MMIO model, no hardware claim. */
#include "holi_power.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ON (1u<<31)
static unsigned cases;
/* Independent hardware address decoder: never derive the synthetic register
 * map from the production accessor being checked. */
static const uint64_t hardware[HOLI_REGISTER_COUNT]={
    0x01436004,0x01471154,0x0143600c,0x01479004,
    0x01400000,0x01400004,0x01400008,0x0140000c,0x0140002c,0x01400038,
    0x05990000,0x05990004,0x05990008,0x0599000c,0x05990010,
    0x05990014,0x05990018,0x0599001c,0x05990020,0x0599002c,0x05990038,
    0x0599101c,0x05991020,0x05991120,0x05991124,
    0x0599106c,0x05991540,0x0599100c,0x05991008,0x05991160,0x05998004,0x05991508,
    0x05991078,0x05991060,0x0599109c,0x05991098,0x05991054};
struct fixture {
    struct dre_holi_power state;
    struct dre_rpm_votes rpm;
    struct dre_holi_power_config cfg;
    uint32_t reg[HOLI_REGISTER_COUNT],initial[HOLI_REGISTER_COUNT];
    unsigned reads,writes,leases,barriers,fail_read,fail_write,fail_lease;
    unsigned fail_readback,lost_source,lost_ack,lost_config,domain_timeout,branch_timeout;
    uint64_t epoch,ticks;
    unsigned trace_reg[32];uint32_t trace_value[32];uint64_t trace_tick[32];
    int stopped_clock;
};
static unsigned index_of(uint64_t address)
{
    for(unsigned i=0;i<HOLI_REGISTER_COUNT;i++)if(hardware[i]==address)return i;
    assert(!"unexpected MMIO address");return 0;
}
static uint64_t lease(void *cookie)
{ struct fixture *f=cookie;f->leases++;return f->leases==f->fail_lease?0:f->epoch; }
static int read32(void *cookie,uint64_t address,uint32_t *value)
{
    struct fixture *f=cookie;f->reads++;unsigned i=index_of(address);
    if(i>=HOLI_PLL_MODE)assert(f->reg[HOLI_GCC_CFG]&1); /* never unclocked GPUCC access */
    if(f->reads==f->fail_read)return 0;
    *value=f->reg[i];return 1;
}
static int writable(unsigned i)
{ return i==HOLI_CX_GDSCR || i==HOLI_GX_GDSCR || (i>=HOLI_GX_SW_RESET && i<=HOLI_GX_DOMAIN) || i>=HOLI_AHB; }
static int write32(void *cookie,uint64_t address,uint32_t value)
{
    struct fixture *f=cookie;unsigned i=index_of(address);assert(writable(i) && f->writes<32);
    f->trace_reg[f->writes]=i;f->trace_value[f->writes]=value;f->trace_tick[f->writes]=f->ticks;f->writes++;
    uint32_t changed=f->reg[i]^value;
    if(i==HOLI_CX_GDSCR)assert(!(changed&~0xf001u));
    else if(i==HOLI_GX_DOMAIN)assert(!(changed&~17u));
    else assert(!(changed&~1u));
    f->reg[i]=value;
    if(i==HOLI_CX_GDSCR && !(value&1) && f->domain_timeout!=1) { f->reg[HOLI_CX_GDSCR]|=ON;f->reg[HOLI_CX_STATUS]|=ON; }
    if(i==HOLI_GX_GDSCR && !(value&1) && f->domain_timeout!=2)f->reg[i]|=ON;
    if(i>=HOLI_AHB && (value&1) && f->branch_timeout!=i) {
        f->reg[i]&=~ON;
        if(i==HOLI_CX_GMU)f->reg[HOLI_GMU_CMD]&=~ON;
        if(i==HOLI_GX_GFX)f->reg[HOLI_GFX_CMD]&=~ON;
    }
    if(f->writes==f->fail_readback)f->reg[i]^=changed&(~changed+1u);
    if(f->writes==f->lost_source)f->reg[HOLI_PLL_FRAC]=0x5aac;
    if(f->writes==f->lost_ack)f->rpm.acked_id[0]++;
    if(f->writes==f->lost_config)f->reg[HOLI_GCC_CFG]=0;
    return f->writes!=f->fail_write; /* possible device effect before failure */
}
static void barrier(void *cookie) { ((struct fixture *)cookie)->barriers++; }
static uint64_t now(void *cookie)
{ struct fixture *f=cookie;if(!f->stopped_clock)f->ticks+=20;return f->ticks; }
static struct dre_rpm_mmio_ops io(struct fixture *f)
{ return (struct dre_rpm_mmio_ops){lease,read32,write32,barrier,now,f}; }
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));f->epoch=91;
    f->cfg=(struct dre_holi_power_config){DRE_HOLI_CHIP,19200000,19200000,253000000,200000000,
        {384,384,64},3,500,100,8,DRE_HOLI_GPUCC,DRE_HOLI_GCC,0x599106c,0x5991540,0x599100c,
        0x5991008,0x5991160,0x5998004,0x5991508};
    f->rpm.link.open=1;f->rpm.link.lease=f->epoch;f->rpm.next_id=5;
    const uint32_t types[]={0x78637772,0x786d7772,0x78677772};
    for(unsigned i=0;i<3;i++) {
        f->rpm.acked_id[i]=2+i;f->rpm.held_level[i]=i==2?64:384;
        f->rpm.resource[i]=(struct dre_rpm_resource){types[i],0,i==2?0:16,512,i==2?16u:512u};
    }
    const uint32_t values[HOLI_REGISTER_COUNT]={
        3,3,3,1u<<16,0xc0100000,31,0x3f,0x107,1,0x4000,
        0x80800005,26,0x3f,7,0x4805,0x20485699,0x2067,0x40000000,2,1,0x5aaa,
        ON,0x103,ON,0x602,0x5801,0,0x801,0x100,0x200,0x400,0x10000001,
        0x80002000,0x80002000,0x80002000,0x80002000,0x80002000};
    memcpy(f->reg,values,sizeof(values));memcpy(f->initial,values,sizeof(values));
}
static const char *run(struct fixture *f)
{ struct dre_rpm_mmio_ops o=io(f);return dre_holi_power_acquire(&f->state,&o,&f->rpm,&f->cfg); }
static void assert_preserved(const struct fixture *f)
{
    for(unsigned i=0;i<HOLI_REGISTER_COUNT;i++) {
        uint32_t mask=0;
        if(i==HOLI_CX_GDSCR)mask=ON|0xf001;
        if(i==HOLI_CX_STATUS || i==HOLI_GFX_CMD || i==HOLI_GMU_CMD)mask=ON;
        if(i==HOLI_GX_GDSCR || i>=HOLI_AHB)mask=ON|1;
        if(i>=HOLI_GX_SW_RESET && i<=HOLI_GX_MISC_RESET)mask=1;
        if(i==HOLI_GX_DOMAIN)mask=17;
        assert((f->reg[i]&~mask)==(f->initial[i]&~mask));
    }
}
int main(void)
{
    struct fixture f;init(&f);assert(!strcmp(run(&f),"HOLI_DOMAINS_BRANCHES_ACQUIRED"));cases++;
    assert(f.state.active && !f.state.unsafe && f.writes==17 && f.state.observed_gpu_hz==252999902 && f.state.observed_gmu_hz==200000000);
    const unsigned order[]={HOLI_CX_GDSCR,HOLI_CX_GDSCR,HOLI_GX_SW_RESET,HOLI_GX_ACD_RESET,HOLI_GX_MISC_RESET,
        HOLI_GX_SW_RESET,HOLI_GX_ACD_RESET,HOLI_GX_MISC_RESET,HOLI_GX_DOMAIN,HOLI_GX_DOMAIN,HOLI_GX_DOMAIN,
        HOLI_GX_GDSCR,HOLI_AHB,HOLI_GX_CXO,HOLI_CXO,HOLI_CX_GMU,HOLI_GX_GFX};
    assert(!memcmp(f.trace_reg,order,sizeof(order)));assert(f.trace_tick[5]-f.trace_tick[4]>=1920 && f.trace_tick[9]-f.trace_tick[8]>=20);assert_preserved(&f);cases++;
    unsigned reads=f.reads,writes=f.writes,leases=f.leases;
    struct dre_rpm_mmio_ops o=io(&f);unsigned before=f.writes;
    assert(!strcmp(dre_holi_power_validate(&f.state,&o,&f.rpm),"HOLI_DOMAINS_BRANCHES_ACQUIRED") && f.writes==before);cases++;
    for(unsigned kind=0;kind<4;kind++)for(unsigned n=1;n<=(kind==0?reads:kind==1?writes:kind==2?leases:writes);n++) {
        init(&f);if(kind==0)f.fail_read=n;if(kind==1)f.fail_write=n;if(kind==2)f.fail_lease=n;if(kind==3)f.fail_readback=n;
        const char *result=run(&f);assert(strcmp(result,"HOLI_DOMAINS_BRANCHES_ACQUIRED") && !f.state.active);
        if(f.writes)assert(f.state.unsafe);
        if(kind==1)assert(f.state.failed_index==f.trace_reg[f.writes-1] && f.state.last_write_index==f.state.failed_index);
        cases++;
    }
    for(unsigned kind=0;kind<20;kind++) {
        init(&f);switch(kind) {
        case 0:f.reg[HOLI_PLL_FRAC]=0x5aac;break;case 1:f.reg[HOLI_PLL_MODE]&=~ON;break;
        case 2:f.reg[HOLI_GFX_CFG]=0;break;case 3:f.reg[HOLI_GMU_CFG]=0;break;
        case 4:f.reg[HOLI_GCC_CFG]=0;break;case 5:f.reg[HOLI_GCC_AXI]=0;break;
        case 6:f.reg[HOLI_GCC_GPU_PLL_VOTE]=0;break;case 7:f.reg[HOLI_GPLL_FRAC]=0;break;
        case 8:f.reg[HOLI_CX_GDSCR]|=2;break;case 9:f.reg[HOLI_GX_GDSCR]|=4;break;
        case 10:f.reg[HOLI_GX_SW_RESET]|=1;break;case 11:f.rpm.pending=1;break;
        case 12:f.rpm.acked_id[0]=0;break;case 13:f.rpm.held_level[0]=64;break;
        case 14:f.rpm.resource[0].type=0x65706d73;break;case 15:f.cfg.timer_hz=24000000;break;
        case 16:f.cfg.variant_features=0;break;case 17:f.cfg.aggregate_floor[0]=64;break;
        case 18:f.reg[HOLI_GCC_CFG]|=1u<<22;break;case 19:f.reg[HOLI_AHB]|=1u<<22;break;
        }
        assert(strcmp(run(&f),"HOLI_DOMAINS_BRANCHES_ACQUIRED") && !f.writes);cases++;
    }
    for(unsigned kind=0;kind<8;kind++) {
        init(&f);
        if(kind==0)f.domain_timeout=1;
        if(kind==1)f.domain_timeout=2;
        if(kind==2)f.branch_timeout=HOLI_GX_GFX;
        if(kind==3)f.stopped_clock=1;
        if(kind==4)f.lost_source=1;
        if(kind==5)f.lost_ack=1;
        if(kind==6)f.lost_config=1;
        if(kind==7)f.branch_timeout=HOLI_CX_GMU;
        assert(strcmp(run(&f),"HOLI_DOMAINS_BRANCHES_ACQUIRED") && f.state.unsafe && !f.state.active);
        assert(f.rpm.held_level[0]==384 && f.rpm.held_level[1]==384 && f.rpm.held_level[2]==64);cases++;
    }
    init(&f);f.reg[HOLI_CX_GDSCR]=0x80008800;f.reg[HOLI_CX_STATUS]=ON;f.reg[HOLI_GX_GDSCR]=ON|0x800;f.reg[HOLI_GX_DOMAIN]&=~1u;
    for(unsigned i=HOLI_AHB;i<HOLI_REGISTER_COUNT;i++)f.reg[i]=0x2001;
    f.reg[HOLI_GFX_CMD]=f.reg[HOLI_GMU_CMD]=0;memcpy(f.initial,f.reg,sizeof(f.reg));
    assert(!strcmp(run(&f),"HOLI_DOMAINS_BRANCHES_ACQUIRED") && !f.writes && f.state.borrowed_domains==3 && f.state.borrowed_branches==31);assert_preserved(&f);cases++;
    f.reg[HOLI_GCC_CFG]=0;o=io(&f);assert(strcmp(dre_holi_power_validate(&f.state,&o,&f.rpm),"HOLI_DOMAINS_BRANCHES_ACQUIRED") && !f.writes);cases++;
    init(&f);f.reg[HOLI_PLL_FRAC]=0x5aab;assert(!strcmp(run(&f),"HOLI_DOMAINS_BRANCHES_ACQUIRED") && f.state.observed_gpu_hz==253000048);cases++;
    for(unsigned domains=0;domains<4;domains++)for(unsigned branches=0;branches<32;branches++) {
        init(&f);
        if(domains&1) { f.reg[HOLI_CX_GDSCR]=0x80008800;f.reg[HOLI_CX_STATUS]=ON; }
        if(domains&2) { f.reg[HOLI_GX_GDSCR]=ON|0x800;f.reg[HOLI_GX_DOMAIN]&=~1u; }
        for(unsigned i=0;i<5;i++)if(branches&(1u<<i))f.reg[HOLI_AHB+i]=0x2001;
        if(branches&(1u<<(HOLI_CX_GMU-HOLI_AHB)))f.reg[HOLI_GMU_CMD]=0;
        if(branches&(1u<<(HOLI_GX_GFX-HOLI_AHB)))f.reg[HOLI_GFX_CMD]=0;
        memcpy(f.initial,f.reg,sizeof(f.reg));
        assert(!strcmp(run(&f),"HOLI_DOMAINS_BRANCHES_ACQUIRED") && f.state.borrowed_domains==domains && f.state.borrowed_branches==branches);
        for(unsigned n=0;n<f.writes;n++) {
            unsigned reg=f.trace_reg[n];
            if(domains&1)assert(reg!=HOLI_CX_GDSCR);
            if(domains&2)assert(reg!=HOLI_GX_GDSCR && !(reg>=HOLI_GX_SW_RESET && reg<=HOLI_GX_DOMAIN));
            if(reg>=HOLI_AHB)assert(!(branches&(1u<<(reg-HOLI_AHB))));
        }
        assert_preserved(&f);cases++;
    }
    assert(!dre_holi_register_address(HOLI_REGISTER_COUNT) && !strcmp(dre_holi_register_name(HOLI_REGISTER_COUNT),"UNKNOWN"));cases++;
    printf("PASS %u synthetic Holi power/clock cases; %u read/%u write/%u lease fault positions; physical rails/source/domain state unproven\n",cases,reads,writes,leases);
    return 0;
}
