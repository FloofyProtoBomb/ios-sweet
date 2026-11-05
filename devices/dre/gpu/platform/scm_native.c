/* SPDX-License-Identifier: MIT
 * Pinned98a3878 SCM64 call convention and PAS13 request state. No SMC is
 * executed except through the actual runtime adapter; fixtures use a model.
 */
#include "scm_native.h"
#include <libfdt.h>
#define SCM_QUERY_FAST UINT64_C(0xc2000601)
#define SCM_QUERY_STD UINT64_C(0x42000601)
#define SCM_PAS_BASE UINT64_C(0x42000200)
static int enabled(const void *f,int node)
{
    for(unsigned depth=0;node>=0&&depth<32;depth++) {
        int n;const char *p=fdt_getprop(f,node,"status",&n);
        if(p&&!((n==3&&p[0]=='o'&&p[1]=='k'&&!p[2])||
           (n==5&&p[0]=='o'&&p[1]=='k'&&p[2]=='a'&&p[3]=='y'&&!p[4])))return 0;
        if(!node)return 1;
        node=fdt_parent_offset(f,node);
    }
    return 0;
}
const char *dre_gpu_scm_profile_from_fdt(const void *f,size_t bytes,struct dre_gpu_scm_profile *out)
{
    if(!f||!out||bytes<40||bytes>(2u<<20)||fdt_check_full(f,bytes)||
       fdt_totalsize(f)!=bytes||fdt_node_check_compatible(f,0,"qcom,holi"))return "SCM_ORIGINAL_FDT_REQUIRED";
    int node=fdt_path_offset(f,"/firmware/scm");
    if(node<0||!enabled(f,node)||fdt_node_check_compatible(f,node,"qcom,scm")||
       fdt_stringlist_count(f,node,"compatible")!=1||
       fdt_node_offset_by_compatible(f,-1,"qcom,scm")!=node||
       fdt_node_offset_by_compatible(f,node,"qcom,scm")!=-FDT_ERR_NOTFOUND)
        return "SCM_EXACT_GENERIC_PROFILE_REQUIRED";
    /* qcom_scm_dt_match generic .data=0 and this original node has no clocks.
     * NULL Linux clocks are no-ops; do not manufacture extra clock providers. */
    if(fdt_getprop(f,node,"clocks",0)||fdt_getprop(f,node,"clock-names",0)||
       fdt_getprop(f,node,"method",0)||fdt_getprop(f,node,"reg",0))
        return "SCM_NAMED_CLOCK_OR_CONDUIT_PROFILE_UNIMPLEMENTED";
    int bus=fdt_path_offset(f,"/soc/pil_scm_pas"),n;
    const void *icc=bus>=0?fdt_getprop(f,bus,"interconnects",&n):0;
    if(bus<0||!enabled(f,bus)||fdt_node_check_compatible(f,bus,"qcom,pil-tz-scm-pas")||!icc||n!=16)
        return "SCM_PIL_INTERCONNECT_POLICY_GRAPH_REQUIRED";
    *out=(struct dre_gpu_scm_profile){0,(uint32_t)node,(uint32_t)bus};
    return "SCM_EMPTY_NAMED_CLOCK_PROFILE_ONLY";
}
static int guarded(struct dre_gpu_scm_channel *s)
{return s&&s->bound&&!s->uncertain&&s->ops.guard(s->ops.cookie)==1&&
    (!s->inflight||s->ops.reservation_held(s->ops.cookie)==1);}
static int poison(struct dre_gpu_scm_channel *s)
{
    s->uncertain=1;
    if(s->inflight)(void)s->ops.finish(s->ops.cookie,1);
    /* Logical uncertain reservation remains retained; ordinary mutexes do not. */
    return 0;
}
int dre_gpu_scm_channel_init(struct dre_gpu_scm_channel *s,const struct dre_gpu_scm_channel_ops *o,
    const struct dre_gpu_fw_relocation *p)
{
    if(!s||!o||!p||s->bound||!o->guard||!o->reserve||!o->reservation_held||
       !o->finish||!o->smc64||!o->barrier||!o->now||o->guard(o->cookie)!=1||
       p->load_bytes!=16384||p->metadata_bytes!=65536||!p->zap.memory_bytes||
       p->zap.memory_bytes>p->load_bytes||(p->load_pa&1048575)||(p->metadata_pa&16383)||
       p->load_pa<UINT64_C(0x80000000)||p->metadata_pa<UINT64_C(0x80000000)||
       p->load_pa>UINT64_C(0x100000000)-p->load_bytes||
       p->metadata_pa>UINT64_C(0x100000000)-p->metadata_bytes||
       (p->load_pa<p->metadata_pa+p->metadata_bytes&&p->metadata_pa<p->load_pa+p->load_bytes))return 0;
    s->ops=*o;s->plan=*p;s->bound=1;return 1;
}
/* One actual SMC. Retain logical registry ownership across continuations,
 * while adapter releases ordinary mutexes on EVERY return, including errors. */
static int exchange(struct dre_gpu_scm_channel *s,struct dre_a619_smc_regs *r)
{
    if(!guarded(s)||s->calls==UINT64_MAX)return s&&s->inflight?poison(s):0;
    if(s->inflight) {
        if(r->x[0]!=1||r->x[6]!=s->continuation)return poison(s);
        for(unsigned i=1;i<8;i++)if(i!=6&&r->x[i]!=s->saved[i])return poison(s);
    } else {
        if(s->ops.reserve(s->ops.cookie)!=1)return 0;
        s->inflight=1;
        for(unsigned i=0;i<8;i++)s->saved[i]=r->x[i];
        if(r->x[0]==(SCM_PAS_BASE|1))s->phase=DRE_SCM_PAS_INIT_PUBLISHED;
    }
    if(!guarded(s))return poison(s);
    s->ops.barrier(s->ops.cookie);s->calls++;
    if(s->ops.smc64(s->ops.cookie,r)!=1||!guarded(s))return poison(s);
    s->ops.barrier(s->ops.cookie);
    if(r->x[0]==1){s->continuation=r->x[6];return 1;}
    if(s->ops.finish(s->ops.cookie,0)!=1)return poison(s);
    s->inflight=0;s->continuation=0;
    return 1;
}
static int query(struct dre_gpu_scm_channel *s,uint64_t function,uint64_t target)
{
    uint64_t start=s->ops.now(s->ops.cookie);
    for(unsigned busy=0;busy<=20;busy++) {
        struct dre_a619_smc_regs r={{function,1,target,0,0,0,0,0}};
        for(unsigned count=0;;count++) {
            if(!exchange(s,&r))return 0;
            if(r.x[0]!=1)break;
            if(count==63||s->ops.now(s->ops.cookie)-start>=19200000)return poison(s);
            for(unsigned i=0;i<8;i++)r.x[i]=s->saved[i];
            r.x[0]=1;r.x[6]=s->continuation;
        }
        if((int64_t)r.x[0]!=-12)return r.x[0]==0&&r.x[1]==1;
        if(busy==20||s->ops.now(s->ops.cookie)-start>=19200000)return 0;
        uint64_t before=s->ops.now(s->ops.cookie);unsigned polls=0;
        while(s->ops.now(s->ops.cookie)-before<576000) {
            if(!guarded(s)||++polls==1000000)return 0;
        }
    }
    return 0;
}
const char *dre_gpu_scm_channel_discover(struct dre_gpu_scm_channel *s)
{
    if(!guarded(s)||s->inflight||s->discovered||s->available||s->phase!=DRE_SCM_PAS_OFF)
        return "SCM_DISCOVERY_NOT_FRESH_OR_OWNED";
    if(!query(s,SCM_QUERY_FAST,UINT64_C(0x02000601)))return "SCM64_CONVENTION_NOT_OBSERVED";
    const unsigned commands[]={1,2,5,6,7};
    for(unsigned i=0;i<5;i++) {
        if(!query(s,SCM_QUERY_STD,UINT64_C(0x02000200)|commands[i]))
            return "SCM_PAS13_CALL_AVAILABILITY_NOT_OBSERVED";
        s->available|=1u<<i;
    }
    if(!guarded(s))return "SCM_DISCOVERY_OWNERSHIP_LOST";
    s->discovered=1;return "SCM64_PAS13_CALLS_OBSERVED_ONLY";
}
int dre_gpu_scm_channel_discovered(struct dre_gpu_scm_channel *s)
{return guarded(s)&&s->discovered==1&&s->available==31&&s->calls>=6;}
static int pas_guard(void *cookie){return dre_gpu_scm_channel_discovered(cookie);}
static int pas_call(void *cookie,struct dre_a619_smc_regs *r)
{
    struct dre_gpu_scm_channel *s=cookie;
    if(!r||!dre_gpu_scm_channel_discovered(s))return 0;
    uint32_t command;
    if(s->inflight)command=s->command;
    else {
        command=(uint32_t)(r->x[0]&255);
        if(r->x[0]!=(SCM_PAS_BASE|command)||r->x[2]!=13||r->x[5]||r->x[6]||r->x[7])return 0;
        if(command!=1&&command!=2&&command!=5&&command!=6&&command!=7)return 0;
        if(r->x[1]!=(command==1?0x82u:command==2?3u:1u)||
           r->x[3]!=(command==1?s->plan.metadata_pa:command==2?s->plan.load_pa:0)||
           r->x[4]!=(command==2?s->plan.zap.memory_bytes:0))return 0;
        if((command==1&&s->phase!=DRE_SCM_PAS_OFF&&s->phase!=DRE_SCM_PAS_INIT_PUBLISHED)||
           (command==2&&s->phase!=DRE_SCM_PAS_INITIALIZED)||
           (command==5&&s->phase!=DRE_SCM_PAS_MEMORY)||
           (command==6&&s->phase==DRE_SCM_PAS_OFF)||
           (command==7&&s->phase!=DRE_SCM_PAS_OFF))return 0;
        s->command=command;
    }
    if(!exchange(s,r))return 0;
    if(!s->inflight&&r->x[0]==0&&r->x[1]==0) {
        if(command==1)s->phase=DRE_SCM_PAS_INITIALIZED;
        else if(command==2)s->phase=DRE_SCM_PAS_MEMORY;
        else if(command==5)s->phase=DRE_SCM_PAS_AUTHENTICATED;
        else if(command==6)s->phase=DRE_SCM_PAS_OFF;
    }
    return 1;
}
static void pas_barrier(void *cookie)
{struct dre_gpu_scm_channel *s=cookie;s->ops.barrier(s->ops.cookie);}
static uint64_t pas_now(void *cookie)
{struct dre_gpu_scm_channel *s=cookie;return s->ops.now(s->ops.cookie);}
int dre_gpu_scm_channel_pas_ops(struct dre_gpu_scm_channel *s,struct dre_a619_pas13_ops *out)
{
    if(!out||!dre_gpu_scm_channel_discovered(s))return 0;
    *out=(struct dre_a619_pas13_ops){pas_guard,pas_call,pas_barrier,pas_now,s};return 1;
}
int dre_gpu_scm_channel_retirable(struct dre_gpu_scm_channel *s)
{return s&&s->bound&&!s->uncertain&&!s->inflight&&s->phase==DRE_SCM_PAS_OFF&&s->ops.guard(s->ops.cookie)==1;}
