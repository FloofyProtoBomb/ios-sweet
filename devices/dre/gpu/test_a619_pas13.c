/* SPDX-License-Identifier: MIT; synthetic SCM and firmware only. */
#include "a619_pas13.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(4096) unsigned char metadata[8192],reserved[8192];
static unsigned char file[512];
struct fixture {
    struct dre_a619_pas13 state;
    unsigned commands[128],calls,barriers,guards;
    unsigned current,interrupts,busy,fail_command,transport_fail_command;
    int supported,always_interrupt,always_busy,lose_on_call,ownership_lost;
    int shutdown_failure,clock_stuck;
    uint64_t tick;
};
static void w16(unsigned off,unsigned v) { file[off]=v; file[off+1]=v>>8; }
static void w32(unsigned off,unsigned v)
{ for (unsigned i=0;i<4;i++) file[off+i]=v>>(i*8); }
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f)); memset(file,0,sizeof(file));
    memset(metadata,0xcc,sizeof(metadata)); memset(reserved,0xa5,sizeof(reserved));
    memcpy(file,"\177ELF\1\1\1",7);
    w16(16,2); w16(18,164); w32(20,1); w32(28,52);
    w16(40,52); w16(42,32); w16(44,3);
    w32(52+16,148); w32(52+24,0x07000000);
    w32(84+4,160); w32(84+12,0x6000); w32(84+16,32);
    w32(84+20,4096); w32(84+24,0x02200000); w32(84+28,4096);
    w32(116,1); w32(116+4,256); w32(116+12,0x5000);
    w32(116+16,16); w32(116+20,64); w32(116+24,0x08000007); w32(116+28,0x100000);
    for (unsigned i=0;i<32;i++) file[160+i]=(unsigned char)(i+1);
    for (unsigned i=0;i<16;i++) file[256+i]=(unsigned char)(0x80+i);
    f->state=(struct dre_a619_pas13){.chip_id=DRE_A619_CHIP_ID,
        .prerequisites=DRE_A619_PAS13_PREREQUISITES,.tick_hz=19200000,
        .firmware=file,.firmware_bytes=sizeof(file),.metadata_cpu=metadata,
        .metadata_pa=0x91000000,.metadata_capacity=sizeof(metadata),
        .reserved_cpu=reserved,.reserved_pa=0x90000000,.reserved_bytes=sizeof(reserved)};
    f->supported=1;
}
static int guard(void *cookie)
{ struct fixture *f=cookie; f->guards++; return !f->ownership_lost; }
static void barrier(void *cookie) { ((struct fixture *)cookie)->barriers++; }
static uint64_t now(void *cookie)
{ struct fixture *f=cookie; if (!f->clock_stuck) f->tick+=96000; return f->tick; }
static int smc(void *cookie,struct dre_a619_smc_regs *r)
{
    struct fixture *f=cookie; f->calls++;
    unsigned cmd;
    if (r->x[0]==1) {
        assert(f->current && r->x[6]==0x12345678); cmd=f->current;
    } else {
        assert((r->x[0]&~UINT64_C(0xff))==UINT64_C(0x42000200));
        cmd=(unsigned)(r->x[0]&255); f->current=cmd;
        assert(r->x[6]==0);
    }
    assert(cmd==1 || cmd==2 || cmd==5 || cmd==6 || cmd==7);
    assert(r->x[2]==13 && r->x[5]==0 && r->x[7]==0);
    assert(r->x[1]==(cmd==1 ? 0x82u : cmd==2 ? 3u : 1u));
    assert(r->x[3]==(cmd==1 ? 0x91000000u : cmd==2 ? 0x90000000u : 0));
    assert(r->x[4]==(cmd==2 ? 4096u : 0));
    assert(f->calls<=128); f->commands[f->calls-1]=cmd;
    if (f->transport_fail_command==cmd) return 0;
    if (f->lose_on_call) f->ownership_lost=1;
    if (f->interrupts || f->always_interrupt) {
        if (f->interrupts) f->interrupts--;
        r->x[0]=1; r->x[1]=0xbadbad; r->x[6]=0x12345678; return 1;
    }
    if (f->busy || f->always_busy) {
        if (f->busy) f->busy--;
        r->x[0]=(uint64_t)-12; r->x[1]=0xbad; return 1;
    }
    r->x[0]=0; r->x[1]=(cmd==7 ? (unsigned)f->supported : f->fail_command==cmd ? 99u : 0u);
    if (cmd==6 && f->shutdown_failure) r->x[1]=99;
    if (cmd==1) {
        assert(f->barriers && !memcmp(metadata,file,148) && !memcmp(metadata+148,file+160,32));
        assert(reserved[0]==0xa5); /* copy is after init+memory-setup */
    }
    if (cmd==5) {
        assert(f->barriers>=2 && !memcmp(reserved,file+256,16));
        for (unsigned i=16;i<4096;i++) assert(!reserved[i]);
    }
    return 1;
}
static struct dre_a619_pas13_ops ops(struct fixture *f)
{ return (struct dre_a619_pas13_ops){guard,smc,barrier,now,f}; }
static const char *auth(struct fixture *f)
{ struct dre_a619_pas13_ops o=ops(f); return dre_a619_pas13_authenticate(&f->state,&o); }
static const char *shutdown(struct fixture *f)
{ struct dre_a619_pas13_ops o=ops(f); return dre_a619_pas13_shutdown(&f->state,&o); }
int main(void)
{
    unsigned checks=0; struct fixture f;
#define CHECK(s) do { assert(!strcmp(auth(&f),(s))); checks++; } while(0)
    init(&f); CHECK("PAS13_AUTHENTICATED"); assert(f.state.authenticated && f.state.initialized && f.calls==4);
    assert(f.commands[0]==7 && f.commands[1]==1 && f.commands[2]==2 && f.commands[3]==5);
    assert(!strcmp(shutdown(&f),"PAS13_SHUTDOWN")); checks++;
    assert(!f.state.initialized && !f.state.authenticated && f.commands[4]==6);
    assert(!strcmp(shutdown(&f),"PAS_NOT_OWNED") && f.calls==5); checks++;
    init(&f); f.state.chip_id=0; CHECK("WRONG_GPU"); assert(!f.calls);
    for (unsigned i=0;i<6;i++) { init(&f); f.state.prerequisites&=~(1u<<i); CHECK("PAS_PLATFORM_UNAVAILABLE"); assert(!f.calls); }
    init(&f); f.state.tick_hz=1; CHECK("UNSUPPORTED_TIMER"); assert(!f.calls);
    init(&f); f.state.authenticated=1; CHECK("PAS_ALREADY_ACTIVE"); assert(!f.calls);
    init(&f); f.ownership_lost=1; CHECK("PAS_OWNERSHIP_REJECTED"); assert(!f.calls);
    init(&f); f.state.metadata_pa++; CHECK("PAS_MEMORY_REJECTED"); assert(!f.calls);
    init(&f); f.state.metadata_cpu++; CHECK("PAS_MEMORY_REJECTED"); assert(!f.calls);
    init(&f); f.state.metadata_pa=f.state.reserved_pa; CHECK("PAS_MEMORY_REJECTED"); assert(!f.calls);
    init(&f); f.state.metadata_cpu=f.state.reserved_cpu; CHECK("PAS_MEMORY_REJECTED"); assert(!f.calls);
    init(&f); f.state.reserved_pa++; CHECK("INVALID_RESERVED_REGION"); assert(!f.calls);
    init(&f); file[0]=0; CHECK("UNSUPPORTED_ELF"); assert(!f.calls);
    init(&f); f.supported=0; CHECK("PAS13_UNSUPPORTED"); assert(f.calls==1 && metadata[0]==0xcc && reserved[0]==0xa5);
    const unsigned commands[]={1,2,5};
    const char *failures[]={"PAS_INIT_REJECTED","PAS_MEMORY_SETUP_REJECTED","PAS_AUTH_REJECTED"};
    for (unsigned i=0;i<3;i++) {
        init(&f); f.fail_command=commands[i]; CHECK(failures[i]);
        assert(!f.state.unsafe && !f.state.initialized && f.commands[f.calls-1]==6);
    }
    init(&f); f.fail_command=7; /* query uses boolean only */ f.supported=2; CHECK("PAS13_UNSUPPORTED");
    init(&f); f.interrupts=2; CHECK("PAS13_AUTHENTICATED"); assert(f.calls==6);
    init(&f); f.busy=2; CHECK("PAS13_AUTHENTICATED"); assert(f.calls==6);
    init(&f); f.always_busy=1; CHECK("PAS_QUERY_FAILED"); assert(f.calls==21 && !f.state.unsafe);
    init(&f); f.always_interrupt=1; CHECK("PAS_RESET_REQUIRED"); assert(f.calls==64 && f.state.unsafe);
    unsigned calls=f.calls; CHECK("PAS_RESET_REQUIRED"); assert(f.calls==calls);
    init(&f); f.transport_fail_command=1; CHECK("PAS_RESET_REQUIRED"); assert(f.calls==2 && f.state.unsafe);
    init(&f); f.lose_on_call=1; CHECK("PAS_RESET_REQUIRED"); assert(f.calls==1 && f.state.unsafe);
    init(&f); f.fail_command=5; f.shutdown_failure=1; CHECK("PAS_RESET_REQUIRED"); assert(f.state.unsafe);
    init(&f); CHECK("PAS13_AUTHENTICATED"); f.state.prerequisites=0; calls=f.calls;
    assert(!strcmp(shutdown(&f),"PAS_RESET_REQUIRED") && f.calls==calls && f.state.unsafe); checks++;
    init(&f); f.always_busy=1; f.clock_stuck=1; CHECK("PAS_QUERY_FAILED"); assert(f.calls==1);
    printf("PASS %u PAS-13 ABI/order/auth/continuation/busy/ownership checks; synthetic SCM only\n",checks);
}
