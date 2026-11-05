/* SPDX-License-Identifier: MIT -- synthetic EL3/registry/FDT, never a kernel
 * provider. Tests the actual discovery/packet/continuation/PAS state engine. */
#include "scm_native.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct fixture {
    struct dre_gpu_scm_channel channel;
    uint64_t ticks,function,target,missing_target;
    unsigned calls,reserves,finishes,busy,interrupts,fail_call,lose_call,missing;
    int owner,logical,quarantine,always_busy,always_interrupt,finish_fail,reserve_fail;
    unsigned service_fail;
};
static int guard(void *p){return ((struct fixture *)p)->owner;}
static int reserve(void *p)
{struct fixture *f=p;f->reserves++;if(f->reserve_fail)return f->reserve_fail;if(f->logical||f->quarantine)return 0;f->logical=1;return 1;}
static int held(void *p){struct fixture *f=p;return f->logical&&!f->quarantine?1:0;}
static int finish(void *p,int uncertain)
{
    struct fixture *f=p;f->finishes++;assert(f->logical);
    if(uncertain){f->quarantine=1;return 1;}
    if(f->finish_fail)return f->finish_fail;
    f->logical=0;return 1;
}
static void barrier(void *p){(void)p;}
static uint64_t now(void *p){struct fixture *f=p;return f->ticks+=96000;}
static int smc(void *p,struct dre_a619_smc_regs *r)
{
    struct fixture *f=p;assert(f->logical&&!f->quarantine);f->calls++;
    if(r->x[0]==1)assert(r->x[6]==0xa6);
    else {f->function=r->x[0];f->target=r->x[2];assert(!r->x[6]);}
    if(f->lose_call==f->calls)f->owner=0;
    if(f->fail_call==f->calls)return 0;
    if(f->interrupts||f->always_interrupt) {
        if(f->interrupts)f->interrupts--;
        r->x[0]=1;r->x[1]=0xbad;r->x[6]=0xa6;return 1;
    }
    if(f->busy||f->always_busy) {
        if(f->busy)f->busy--;
        r->x[0]=(uint64_t)-12;r->x[1]=0xbad;return 1;
    }
    if(f->function==UINT64_C(0xc2000601)||f->function==UINT64_C(0x42000601)) {
        assert(r->x[1]==1&&f->target>=UINT64_C(0x02000201));
        r->x[0]=0;r->x[1]=f->missing!=(unsigned)(f->target&255)&&f->missing_target!=f->target;return 1;
    }
    assert((f->function&~UINT64_C(255))==UINT64_C(0x42000200)&&f->target==13);
    unsigned cmd=(unsigned)(f->function&255);
    r->x[0]=0;r->x[1]=f->service_fail==cmd?99u:cmd==7?1u:0u;return 1;
}
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));f->owner=1;
    struct dre_gpu_scm_channel_ops ops={f,guard,reserve,held,finish,smc,barrier,now};
    struct dre_gpu_fw_relocation plan={.load_pa=0x90000000,.load_bytes=16384,
        .metadata_pa=0x90100000,.metadata_bytes=65536,.zap={.memory_bytes=4096}};
    assert(dre_gpu_scm_channel_init(&f->channel,&ops,&plan)==1);
}
static void discover(struct fixture *f)
{assert(!strcmp(dre_gpu_scm_channel_discover(&f->channel),"SCM64_PAS13_CALLS_OBSERVED_ONLY"));}
static int request(struct fixture *f,unsigned cmd,struct dre_a619_smc_regs *out)
{
    struct dre_a619_pas13_ops ops;assert(dre_gpu_scm_channel_pas_ops(&f->channel,&ops)==1);
    *out=(struct dre_a619_smc_regs){{UINT64_C(0x42000200)|cmd,cmd==1?0x82u:cmd==2?3u:1u,13,
        cmd==1?0x90100000u:cmd==2?0x90000000u:0,cmd==2?4096u:0,0,0,0}};
    return ops.smc64(ops.cookie,out);
}
/* Minimal original-shaped FDT; profile reads the actual libfdt implementation. */
static uint8_t dt[1024];static unsigned pos;
static const char names[]="compatible\0clocks\0interconnects\0status\0";
static void be(unsigned off,uint32_t v){for(unsigned i=0;i<4;i++)dt[off+i]=(uint8_t)(v>>(24-8*i));}
static void token(uint32_t n){be(pos,n);pos+=4;}
static void node(const char *name){token(1);size_t n=strlen(name)+1;memcpy(dt+pos,name,n);pos=(pos+(unsigned)n+3)&~3u;}
static void prop(const char *name,const void *p,unsigned n)
{
    unsigned off=0;while(strcmp(names+off,name))off+=(unsigned)strlen(names+off)+1;
    token(3);token(n);token(off);if(n)memcpy(dt+pos,p,n);pos=(pos+n+3)&~3u;
}
static size_t fdt(unsigned flags)
{
    memset(dt,0,sizeof(dt));pos=56;node("");prop("compatible","qcom,holi",10);
    node("firmware");node("scm");prop("compatible","qcom,scm",9);
    if(flags&1)prop("clocks",NULL,0);
    if(flags&2)prop("status","disabled",9);
    token(2);token(2);node("soc");node("pil_scm_pas");prop("compatible","qcom,pil-tz-scm-pas",20);
    uint8_t icc[16]={0};if(!(flags&4))prop("interconnects",icc,16);
    token(2);token(2);
    if(flags&8){node("other");prop("compatible","qcom,scm",9);token(2);}
    token(2);token(9);unsigned size=pos-56,strings=pos;memcpy(dt+pos,names,sizeof(names));pos+=sizeof(names);
    uint32_t header[]={0xd00dfeed,pos,56,strings,40,17,16,0,sizeof(names),size};
    for(unsigned i=0;i<10;i++)be(i*4,header[i]);
    return pos;
}
static _Alignas(16384) uint8_t metadata[65536],destination[16384];
static uint8_t firmware[512];
static void fw_word(unsigned off,unsigned n){for(unsigned i=0;i<4;i++)firmware[off+i]=(uint8_t)(n>>(i*8));}
static void fw_half(unsigned off,unsigned n){firmware[off]=(uint8_t)n;firmware[off+1]=(uint8_t)(n>>8);}
static void firmware_init(void)
{
    memset(firmware,0,sizeof(firmware));memcpy(firmware,"\177ELF\1\1\1",7);
    fw_half(16,2);fw_half(18,164);fw_word(20,1);fw_word(24,0x5000);fw_word(28,52);
    fw_half(40,52);fw_half(42,32);fw_half(44,3);fw_word(68,148);fw_word(76,0x07000000);
    fw_word(88,160);fw_word(96,0x6000);fw_word(100,32);fw_word(104,4096);fw_word(108,0x02200000);fw_word(112,4096);
    fw_word(116,1);fw_word(120,256);fw_word(128,0x5000);fw_word(132,16);fw_word(136,64);fw_word(140,0x08000007);fw_word(144,0x100000);
}
static const char *authenticate(struct fixture *f,struct dre_a619_pas13 *pas)
{
    firmware_init();memset(metadata,0xcc,sizeof(metadata));memset(destination,0xa5,sizeof(destination));
    *pas=(struct dre_a619_pas13){.chip_id=DRE_GPU_FW_CHIP_ID,.prerequisites=DRE_A619_PAS13_PREREQUISITES,
        .tick_hz=19200000,.firmware=firmware,.firmware_bytes=sizeof(firmware),.metadata_cpu=metadata,
        .metadata_pa=0x90100000,.metadata_capacity=65536,.reserved_cpu=destination,.reserved_pa=0x90000000,.reserved_bytes=16384};
    struct dre_a619_pas13_ops ops;assert(dre_gpu_scm_channel_pas_ops(&f->channel,&ops)==1);
    return dre_a619_pas13_authenticate(pas,&ops);
}
int main(int argc,char **argv)
{
    struct fixture f;struct dre_a619_smc_regs r;struct dre_a619_pas13_ops ops;unsigned cases=0;
    struct dre_gpu_scm_profile profile;
    assert(!strcmp(dre_gpu_scm_profile_from_fdt(dt,fdt(0),&profile),"SCM_EMPTY_NAMED_CLOCK_PROFILE_ONLY")&&!profile.clock_mask);cases++;
    for(unsigned flag=1;flag<=8;flag<<=1) {
        const char *s=dre_gpu_scm_profile_from_fdt(dt,fdt(flag),&profile);
        assert(strcmp(s,"SCM_EMPTY_NAMED_CLOCK_PROFILE_ONLY"));cases++;
    }
    init(&f);assert(!dre_gpu_scm_channel_pas_ops(&f.channel,&ops));discover(&f);
    assert(f.calls==6&&f.reserves==6&&f.finishes==6&&!f.logical&&!f.quarantine&&
        f.channel.phase==DRE_SCM_PAS_OFF&&dre_gpu_scm_channel_retirable(&f.channel)==1);cases++;
    assert(request(&f,7,&r)==1&&r.x[1]==1&&f.channel.phase==DRE_SCM_PAS_OFF);cases++;
    assert(request(&f,5,&r)==0&&f.channel.phase==DRE_SCM_PAS_OFF);cases++;
    assert(request(&f,1,&r)==1&&f.channel.phase==DRE_SCM_PAS_INITIALIZED);cases++;
    assert(request(&f,2,&r)==1&&f.channel.phase==DRE_SCM_PAS_MEMORY);cases++;
    assert(request(&f,5,&r)==1&&f.channel.phase==DRE_SCM_PAS_AUTHENTICATED&&!dre_gpu_scm_channel_retirable(&f.channel));cases++;
    assert(request(&f,6,&r)==1&&f.channel.phase==DRE_SCM_PAS_OFF&&dre_gpu_scm_channel_retirable(&f.channel)==1);cases++;
    for(unsigned command=1;command<=7;command++) {
        init(&f);f.missing_target=UINT64_C(0x02000200)|command;
        const char *result=dre_gpu_scm_channel_discover(&f.channel);
        assert((strcmp(result,"SCM64_PAS13_CALLS_OBSERVED_ONLY")==0)==(command==3||command==4));
        if(command!=3&&command!=4)assert(!dre_gpu_scm_channel_pas_ops(&f.channel,&ops));
        cases++;
    }
    init(&f);f.interrupts=3;discover(&f);assert(f.calls==9&&f.reserves==6&&f.finishes==6);cases++;
    init(&f);f.busy=2;discover(&f);assert(f.calls==8&&!f.logical);cases++;
    init(&f);f.always_busy=1;assert(strcmp(dre_gpu_scm_channel_discover(&f.channel),"SCM64_PAS13_CALLS_OBSERVED_ONLY"));
    assert(!f.channel.discovered&&!f.channel.uncertain&&!f.logical&&f.calls<=21);cases++;
    init(&f);f.always_interrupt=1;assert(strcmp(dre_gpu_scm_channel_discover(&f.channel),"SCM64_PAS13_CALLS_OBSERVED_ONLY"));
    assert(f.channel.uncertain&&f.channel.inflight&&f.logical&&f.quarantine&&f.calls==64);cases++;
    init(&f);f.fail_call=1;assert(strcmp(dre_gpu_scm_channel_discover(&f.channel),"SCM64_PAS13_CALLS_OBSERVED_ONLY"));
    assert(f.channel.uncertain&&f.logical&&f.quarantine);cases++;
    init(&f);f.lose_call=1;assert(strcmp(dre_gpu_scm_channel_discover(&f.channel),"SCM64_PAS13_CALLS_OBSERVED_ONLY"));
    assert(f.channel.uncertain&&f.logical&&f.quarantine);cases++;
    init(&f);f.finish_fail=-1;assert(strcmp(dre_gpu_scm_channel_discover(&f.channel),"SCM64_PAS13_CALLS_OBSERVED_ONLY"));
    assert(f.channel.uncertain&&f.logical&&f.quarantine);cases++;
    init(&f);f.missing=1;assert(!strcmp(dre_gpu_scm_channel_discover(&f.channel),"SCM64_CONVENTION_NOT_OBSERVED"));
    assert(f.calls==1&&!f.channel.discovered&&!f.logical);cases++;
    init(&f);f.reserve_fail=-1;assert(!strcmp(dre_gpu_scm_channel_discover(&f.channel),"SCM64_CONVENTION_NOT_OBSERVED"));
    assert(!f.calls&&!f.channel.inflight&&!f.logical);cases++;
    init(&f);discover(&f);unsigned before=f.calls;f.owner=-1;
    assert(!dre_gpu_scm_channel_pas_ops(&f.channel,&ops)&&f.calls==before);cases++;
    init(&f);discover(&f);assert(dre_gpu_scm_channel_pas_ops(&f.channel,&ops)==1);
    for(unsigned field=0;field<8;field++) {
        r=(struct dre_a619_smc_regs){{0x42000201,0x82,13,0x90100000,0,0,0,0}};
        r.x[field]^=field==3?16384:128;before=f.calls;
        assert(!ops.smc64(ops.cookie,&r)&&f.calls==before);cases++;
    }
    init(&f);discover(&f);assert(dre_gpu_scm_channel_pas_ops(&f.channel,&ops)==1);f.interrupts=1;
    assert(request(&f,1,&r)==1&&r.x[0]==1&&f.logical);
    r=(struct dre_a619_smc_regs){{1,0x82,13,0x90100000,0,0,0xdead,0}};
    before=f.calls;assert(!ops.smc64(ops.cookie,&r)&&f.calls==before&&f.quarantine&&f.channel.uncertain);cases++;
    struct dre_a619_pas13 pas;
    init(&f);discover(&f);f.interrupts=2;f.busy=2;
    assert(!strcmp(authenticate(&f,&pas),"PAS13_AUTHENTICATED")&&pas.authenticated&&
        f.channel.phase==DRE_SCM_PAS_AUTHENTICATED&&!f.logical);cases++;
    assert(dre_gpu_scm_channel_pas_ops(&f.channel,&ops)==1);
    assert(!strcmp(dre_a619_pas13_shutdown(&pas,&ops),"PAS13_SHUTDOWN")&&
        dre_gpu_scm_channel_retirable(&f.channel)==1);cases++;
    for(unsigned command=1;command<=5;command++)if(command==1||command==2||command==5) {
        init(&f);discover(&f);f.service_fail=command;
        assert(strcmp(authenticate(&f,&pas),"PAS13_AUTHENTICATED")&&!pas.authenticated&&
            !pas.initialized&&f.channel.phase==DRE_SCM_PAS_OFF&&!f.logical);cases++;
    }
    init(&f);discover(&f);f.fail_call=f.calls+3; /* supported,init,then uncertain memory setup */
    assert(!strcmp(authenticate(&f,&pas),"PAS_RESET_REQUIRED")&&pas.unsafe&&pas.initialized&&
        f.channel.uncertain&&f.logical&&f.quarantine&&!dre_gpu_scm_channel_retirable(&f.channel));cases++;
    printf("PASS %u actual SCM engine/profile/PAS composition cases; synthetic firmware replies only\n",cases);
    if(argc==2) {
        FILE *in=fopen(argv[1],"rb");assert(in);assert(!fseek(in,0,SEEK_END));long n=ftell(in);
        assert(n>=40&&n<=2*1024*1024);rewind(in);void *blob=malloc((size_t)n);assert(blob);
        assert(fread(blob,1,(size_t)n,in)==(size_t)n);assert(!fclose(in));
        assert(!strcmp(dre_gpu_scm_profile_from_fdt(blob,(size_t)n,&profile),"SCM_EMPTY_NAMED_CLOCK_PROFILE_ONLY"));
        puts("PASS private original-FDT exact empty SCM clock set; PIL bus policy still required");free(blob);
    }else assert(argc==1);
    return 0;
}
