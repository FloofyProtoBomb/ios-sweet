/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Original bounded implementation of named-parent hold semantics
 * grounded in pinned Lineage98a3878 gcc-holi.c/clk-branch.c/clk-alpha-pll.c.
 * No source/rate/rail policy, clock ownership or GPU readiness is invented. */
#include "gcc_hold.h"
static const uint64_t registers[4]={0x01436004,0x01471154,0x0143600c,0x01479004};
static const uint32_t masks[4]={1,1,1,UINT32_C(1)<<16};
static const uint64_t gpll_registers[6]={0x01400000,0x01400004,0x01400008,
    0x0140000c,0x0140002c,0x01400038};
static const char *fail(struct dre_holi_gcc_hold *s,const char *why)
{ s->error=why;s->active=0;s->unsafe=1;return why; }
static int ack_holds(const struct dre_holi_gcc_hold *s)
{
    const struct dre_rpm_votes *v=s->votes;
    static const uint32_t types[]={0x78637772,0x786d7772,0x78677772};
    if(!v||!v->link.open||v->link.unsafe||v->unsafe||v->pending||v->link.lease!=s->lease)return 0;
    for(unsigned i=0;i<3;i++) {
        uint32_t bit=dre_rpm_level_mask(v->held_level[i]);
        if(v->resource[i].type!=types[i]||v->resource[i].id||!bit||
           !(v->resource[i].allowed_levels&bit)||v->acked_id[i]<2||
           v->acked_id[i]>=v->next_id)return 0;
        for(unsigned j=0;j<i;j++)if(v->acked_id[j]==v->acked_id[i])return 0;
    }
    return 1;
}
static int owned(struct dre_holi_gcc_hold *s,int source)
{
    return s&&!s->unsafe&&s->lease&&s->io.lease(s->io.cookie)==s->lease&&ack_holds(s)&&
        (!source||(s->source_hold&&s->policy.check(s->policy.cookie,s->lease,s->source_hold)==0));
}
static int read32(struct dre_holi_gcc_hold *s,uint64_t address,uint32_t *value,int source)
{
    if(!value||!owned(s,source))return 0;
    uint32_t observed=0;
    int status=s->io.read32(s->io.cookie,address,&observed);
    s->io.barrier(s->io.cookie);
    if(status!=1||!owned(s,source))return 0;
    *value=observed;return 1;
}
static int parent(struct dre_holi_gcc_hold *s,int source)
{
    uint32_t v[6];
    for(unsigned i=0;i<6;i++)if(!read32(s,gpll_registers[i],&v[i],source))return 0;
    /* Same actually supported600MHz Fabia parent shape as holi_power.c.
     * Never program or retune a shared PLL to make a mismatched read pass. */
    if((v[0]&0xc0100000u)!=0xc0100000u||(v[0]&((1u<<22)|(1u<<7)))||
       v[1]!=31||!v[2]||(v[3]&0xf07)!=0x107||v[4]!=1||v[5]!=0x4000)return 0;
    for(unsigned i=0;i<6;i++)s->gpll0[i]=v[i];
    return 1;
}
static int branch(uint32_t value)
{
    /* Named GCC branches preserve inherited HWCG. With HWCG held, an idle
     * branch's HALT may be set while actual hardware demand can wake it. */
    return !(value&(1u<<22))&&(value&1)&&((value&2)||!(value&(1u<<31)));
}
static int held_register(unsigned i,uint32_t value)
{ return i==3?!!(value&masks[i]):branch(value); }
static int gate(struct dre_holi_gcc_hold *s,unsigned i,uint64_t budget)
{
    uint64_t start=s->io.now(s->io.cookie);
    if(!owned(s,1))return 0;
    for(unsigned poll=0;poll<100000;poll++) {
        uint32_t value;
        if(!read32(s,registers[i],&value,1))return 0;
        s->last[i]=value;
        uint64_t now=s->io.now(s->io.cookie);
        if(now<start||now-start>=budget)return 0;
        if(held_register(i,value))return 1;
    }
    return 0;
}
static int vote_delay(struct dre_holi_gcc_hold *s,uint64_t budget)
{
    /* The pinned GPLL0-div branch uses BRANCH_HALT_DELAY:10us, not a HALT
     * bit on this shared vote register. Use the real qualified counter. */
    uint64_t start=s->io.now(s->io.cookie);
    for(unsigned poll=0;poll<100000;poll++) {
        if(!owned(s,1))return 0;
        uint64_t now=s->io.now(s->io.cookie);
        if(now<start||now-start>=budget)return 0;
        if(now-start>=192)return 1;
    }
    return 0;
}
static int enable(struct dre_holi_gcc_hold *s,unsigned i,uint64_t budget)
{
    if(!owned(s,1)||s->policy.lock_register(s->policy.cookie,s->lease,s->source_hold,
        registers[i],masks[i]))return 0;
    uint32_t before=0;int good=read32(s,registers[i],&before,1);
    s->before[i]=before;
    if(good&&!(before&masks[i])) {
        if(s->writes==UINT64_MAX)good=0;
        else {
            ++s->writes; /* Journal before any possibly effective store. */
            s->io.barrier(s->io.cookie);
            good=s->io.write32(s->io.cookie,registers[i],before|masks[i])==1;
            s->io.barrier(s->io.cookie);
            good=good&&owned(s,1);
        }
    }
    /* No independent local mutex can authorize this RMW of shared registers.
     * Actual platform serialization is retained until the readback/poll ends. */
    if(good&&i==3)good=vote_delay(s,budget);
    if(good)good=gate(s,i,budget);
    s->policy.unlock_register(s->policy.cookie,s->lease,s->source_hold,registers[i],masks[i]);
    return good&&owned(s,1);
}
const char *dre_holi_gcc_acquire(struct dre_holi_gcc_hold *s,
    const struct dre_rpm_mmio_ops *io,const struct dre_holi_gcc_policy *p,
    const struct dre_rpm_votes *votes,uint64_t budget)
{
    if(!s||!io||!p||!votes||!io->lease||!io->read32||!io->write32||!io->barrier||!io->now||
       !p->acquire||!p->check||!p->lock_register||!p->unlock_register||!budget||budget>DRE_RPM_TIMER_HZ)
        return "GCC_PARENT_BINDING_INCOMPLETE";
    if(s->attempted||s->source_hold||s->active||s->unsafe||s->writes)return "GCC_PARENT_FRESH_OWNER_REQUIRED";
    s->io=*io;s->policy=*p;s->votes=votes;s->lease=io->lease(io->cookie);s->attempted=1;
    if(!owned(s,0)||!parent(s,0))return fail(s,"GCC_ACTUAL_600MHZ_PARENT_OR_ACKS_UNQUALIFIED");
    int status=p->acquire(p->cookie,s->lease,votes,s->gpll0,&s->source_hold);
    if(status||!s->source_hold||!owned(s,1)||!parent(s,1))
        return fail(s,"GCC_REAL_SHARED_SOURCE_POLICY_UNAVAILABLE_RETAINED");
    /* Enable actual APSS GPLL0div vote first, then the named config/AXI/MEM
     * gates. This programs no PLL, RCG, interconnect rate, rail or GDSC. */
    static const unsigned order[]={3,0,1,2};
    for(unsigned n=0;n<4;n++)if(!enable(s,order[n],budget))
        return fail(s,"GCC_ENABLE_OR_READBACK_UNCERTAIN_RETAINED");
    if(!parent(s,1))return fail(s,"GCC_HELD_PARENT_CHANGED_RETAINED");
    s->active=1;s->error=0;return "GCC_NAMED_PARENTS_HELD";
}
const char *dre_holi_gcc_validate(struct dre_holi_gcc_hold *s)
{
    if(!s||!s->active||!owned(s,1)||!parent(s,1))
        return s?fail(s,"GCC_PARENT_SOURCE_HOLD_LOST"):"GCC_PARENT_INVALID_ARGUMENT";
    for(unsigned i=0;i<4;i++)if(!read32(s,registers[i],&s->last[i],1)||!held_register(i,s->last[i]))
        return fail(s,"GCC_PARENT_GATE_HOLD_LOST");
    return "GCC_NAMED_PARENTS_HELD";
}
