/* SPDX-License-Identifier: MIT -- raw-context/EL3 models only. This never
 * calls the privileged _early entry, including on ARM64 host machines. */
#include "scm_probe.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct fixture {
    struct dre_scm_probe_context context;
    uint64_t ticks,target,unavailable_target;
    unsigned calls,reads,busy,interrupts,unavailable,service_error,transport_error;
    unsigned fail_call,lose_context_call,fail_read;
    int always_busy,always_interrupt,clock_stuck,freeze_after_call;
};
static int read_context(void *cookie,struct dre_scm_probe_context *out)
{
    struct fixture *f=cookie;f->reads++;
    if(f->fail_read==f->reads)return 0;
    if(!f->clock_stuck&&!(f->freeze_after_call&&f->calls))f->ticks+=1000;
    *out=f->context;out->counter=f->ticks;
    if(f->lose_context_call&&f->calls>=f->lose_context_call)out->daif^=128;
    return 1;
}
static int smc(void *cookie,struct dre_a619_smc_regs *r)
{
    struct fixture *f=cookie;f->calls++;
    /* This hard assertion makes a production opcode regression fail the
     * fixture: NO PAS_INIT/MEM/AUTH/SHUTDOWN packet is accepted by the model. */
    assert((r->x[0]==UINT64_C(0xc2000601)||r->x[0]==1)&&r->x[1]==1&&
        !r->x[3]&&!r->x[4]&&!r->x[5]&&!r->x[7]);
    if(r->x[0]==1)assert(r->x[2]==f->target&&r->x[6]==0xa6);
    else {assert(!r->x[6]);f->target=r->x[2];}
    assert(f->target==UINT64_C(0x02000601)||f->target==UINT64_C(0x02000201)||
        f->target==UINT64_C(0x02000202)||f->target==UINT64_C(0x02000205)||
        f->target==UINT64_C(0x02000206)||f->target==UINT64_C(0x02000207));
    if(f->calls==f->fail_call)return 0;
    if(f->interrupts||f->always_interrupt) {
        if(f->interrupts)f->interrupts--;
        r->x[0]=1;r->x[1]=0xbad;r->x[6]=0xa6;return 1;
    }
    if(f->busy||f->always_busy) {
        if(f->busy)f->busy--;
        r->x[0]=(uint64_t)-12;r->x[1]=0xbad;return 1;
    }
    r->x[0]=(f->transport_error==(unsigned)(f->target&255))?(uint64_t)-1:0;
    r->x[1]=f->service_error==(unsigned)(f->target&255)?99u:
        f->unavailable==(unsigned)(f->target&255)||f->unavailable_target==f->target?0u:1u;
    return 1;
}
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));
    f->context=(struct dre_scm_probe_context){.current_el=4,.daif=0x3c0,
        .sctlr=0,.frequency=19200000,.mpidr=0x80000000};
}
static uint8_t dt[1024];static unsigned position;
static const char names[]="compatible\0oplus,dtsi_no\0clocks\0interconnects\0";
static void be(unsigned off,uint32_t v){for(unsigned i=0;i<4;i++)dt[off+i]=(uint8_t)(v>>(24-8*i));}
static void token(uint32_t n){be(position,n);position+=4;}
static void node(const char *name)
{token(1);size_t n=strlen(name)+1;memcpy(dt+position,name,n);position=(position+(unsigned)n+3)&~3u;}
static void prop(const char *name,const void *data,unsigned bytes)
{
    unsigned off=0;while(strcmp(names+off,name))off+=(unsigned)strlen(names+off)+1;
    token(3);token(bytes);token(off);if(bytes)memcpy(dt+position,data,bytes);
    position=(position+bytes+3)&~3u;
}
static size_t fdt(int bad_board,int extra_clock)
{
    memset(dt,0,sizeof(dt));position=56;node("");prop("compatible","qcom,holi",10);
    uint8_t project[]={0,2,8,24}; /* 133144, a published board-family ID */
    if(bad_board)project[3]^=1;
    prop("oplus,dtsi_no",project,4);node("firmware");node("scm");prop("compatible","qcom,scm",9);
    if(extra_clock)prop("clocks",NULL,0);
    token(2);token(2);node("soc");node("pil_scm_pas");prop("compatible","qcom,pil-tz-scm-pas",20);
    uint8_t interconnects[16]={0};prop("interconnects",interconnects,16);token(2);token(2);token(2);token(9);
    unsigned structure_bytes=position-56,strings=position;
    memcpy(dt+position,names,sizeof(names));position+=sizeof(names);
    uint32_t header[]={0xd00dfeed,position,56,strings,40,17,16,0,sizeof(names),structure_bytes};
    for(unsigned i=0;i<10;i++)be(i*4,header[i]);
    return position;
}
static enum dre_scm_probe_status run(struct fixture *f,size_t bytes,struct dre_scm_probe_result *r)
{
    const struct dre_scm_probe_io io={f,read_context,smc};
    return dre_gpu_scm_probe_with_io(dt,bytes,&io,r);
}
int main(int argc,char **argv)
{
    struct fixture f;struct dre_scm_probe_result r;unsigned cases=0;
    size_t bytes=fdt(0,0);init(&f);assert(run(&f,bytes,&r)==DRE_SCM_PROBE_OBSERVED);
    assert(f.calls==6&&r.available_mask==63&&!r.unavailable_mask&&!r.uncertain&&!r.boot_must_stop&&r.query_count==6);cases++;
    for(unsigned i=0;i<6;i++)assert(r.replies[i].function==UINT64_C(0xc2000601)&&r.replies[i].transport==0&&r.replies[i].service==1);
    bytes=fdt(1,0);init(&f);assert(run(&f,bytes,&r)==DRE_SCM_PROBE_BOARD&&!f.calls);cases++;
    bytes=fdt(0,1);init(&f);assert(run(&f,bytes,&r)==DRE_SCM_PROBE_PROFILE&&!f.calls);cases++;
    bytes=fdt(0,0);
    for(unsigned field=0;field<6;field++) {
        init(&f);
        if(field==0)f.context.current_el=8;
        if(field==1)f.context.daif=0;
        if(field==2)f.context.sctlr=1;
        if(field==3)f.context.sctlr=4;
        if(field==4)f.context.frequency=24000000;
        if(field==5)f.context.mpidr=0x80000001;
        assert(run(&f,bytes,&r)==DRE_SCM_PROBE_CONTEXT&&!f.calls);cases++;
    }
    init(&f);f.fail_read=3;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_CONTEXT_CHANGED&&!f.calls&&r.boot_must_stop&&!r.uncertain);cases++;
    init(&f);f.clock_stuck=1;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_CLOCK_STALLED&&!f.calls&&r.boot_must_stop);cases++;
    init(&f);f.interrupts=3;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_OBSERVED&&f.calls==9&&r.replies[0].interruptions==3);cases++;
    init(&f);f.busy=2;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_OBSERVED&&f.calls==8&&r.replies[0].busy==2);cases++;
    init(&f);f.always_busy=1;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_BUSY_LIMIT&&f.calls==21&&
        !r.uncertain&&!r.boot_must_stop&&r.query_count==1);cases++;
    init(&f);f.always_interrupt=1;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_CONTINUATION_LIMIT&&f.calls==64&&
        r.uncertain&&r.boot_must_stop&&r.query_count==1);cases++;
    init(&f);f.fail_call=1;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_TRANSPORT&&f.calls==1&&r.uncertain&&r.boot_must_stop);cases++;
    init(&f);f.fail_call=4;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_TRANSPORT&&f.calls==4&&
        r.query_count==4&&r.available_mask==7&&r.uncertain&&r.boot_must_stop);cases++;
    init(&f);f.lose_context_call=1;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_CONTEXT_CHANGED&&f.calls==1&&r.uncertain&&r.boot_must_stop);cases++;
    init(&f);f.always_busy=1;f.freeze_after_call=1;
    assert(run(&f,bytes,&r)==DRE_SCM_PROBE_CLOCK_STALLED&&f.calls==1&&r.boot_must_stop&&!r.uncertain);cases++;
    init(&f);f.unavailable=1;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_UNSUPPORTED&&f.calls==1&&
        r.unavailable_mask==1&&!r.uncertain&&!r.boot_must_stop);cases++;
    init(&f);f.transport_error=1;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_UNSUPPORTED&&f.calls==1&&
        r.replies[0].transport==-1&&!r.unavailable_mask&&!r.uncertain);cases++;
    init(&f);f.unavailable=2;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_PARTIAL&&f.calls==6&&
        r.unavailable_mask==4&&r.available_mask==59&&!r.boot_must_stop);cases++;
    init(&f);f.unavailable_target=UINT64_C(0x02000201);assert(run(&f,bytes,&r)==DRE_SCM_PROBE_PARTIAL&&f.calls==6&&
        r.unavailable_mask==2&&r.available_mask==61&&!r.boot_must_stop);cases++;
    init(&f);f.service_error=5;assert(run(&f,bytes,&r)==DRE_SCM_PROBE_PARTIAL&&f.calls==6&&
        r.replies[3].service==99&&r.replies[3].state==DRE_SCM_REPLY_ERROR&&!r.uncertain);cases++;
    printf("PASS %u read-only INFO probe context/packet/failure cases; synthetic raw replies only\n",cases);
    if(argc==2) {
        FILE *in=fopen(argv[1],"rb");assert(in);assert(!fseek(in,0,SEEK_END));long n=ftell(in);
        assert(n>=40&&n<=2*1024*1024);rewind(in);void *blob=malloc((size_t)n);assert(blob);
        assert(fread(blob,1,(size_t)n,in)==(size_t)n);assert(!fclose(in));init(&f);
        const struct dre_scm_probe_io io={&f,read_context,smc};
        assert(dre_gpu_scm_probe_with_io(blob,(size_t)n,&io,&r)==DRE_SCM_PROBE_OBSERVED);
        puts("PASS private original-FDT probe guard with SYNTHETIC transport; no actual SMC");free(blob);
    }else assert(argc==1);
    return 0;
}
