/* SPDX-License-Identifier: MIT
 * Synthetic provider-boundary fixture for the REAL startup coordinator.
 * Leaf implementations are independently tested by their production fixtures.
 * These fixture providers are never linked into a kernel or device image.
 */
#include "a619_startup.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    struct dre_a619_startup state;
    struct dre_a619_start_plan plan;
    struct dre_rpm_votes votes;
    struct dre_a619_rbbm rbbm;
    struct dre_a619_smmu smmu;
    struct dre_a619_cp cp;
    struct dre_a619_queue queue;
    struct dre_a619_vm vm;
    struct dre_a619_vm_ops vm_ops;
    uint64_t lease;
    int owner,policy,cold,qualifier,mutate_qualifier,fail_at,power_untouched;
    unsigned sequence,cold_calls,qualifications,cp_calls,domain_calls;
    int bad_marker;
};
static struct fixture *active;
static uint64_t lease(void *p){return ((struct fixture *)p)->lease;}
static int guard(void *p){return ((struct fixture *)p)->owner;}
static int read32(void *p,uint64_t a,uint32_t *v){(void)p;(void)a;*v=0;return 1;}
static int write32(void *p,uint64_t a,uint32_t v){(void)p;(void)a;(void)v;return 1;}
static int read64(void *p,uint64_t a,uint64_t *v){(void)p;(void)a;*v=0;return 1;}
static int write64(void *p,uint64_t a,uint64_t v){(void)p;(void)a;(void)v;return 1;}
static int owns_ram(void *p,uint64_t a,size_t n){(void)p;(void)a;(void)n;return 1;}
static void barrier(void *p){(void)p;}
static uint64_t now(void *p){(void)p;return 1;}
static int policy(void *p,uint64_t l,const struct dre_rpm_votes *v,const struct dre_holi_power_config *c)
{struct fixture *f=p;assert(l==f->lease&&v==&f->votes&&c==&f->state.plan.power_config);return f->policy;}
static int cold(void *p,uint64_t l)
{struct fixture *f=p;assert(l==f->lease);f->cold_calls++;return f->cold;}
static int qualify(void *p,const struct dre_a619_smmu *s)
{
    struct fixture *f=p;assert(f->state.phase==DRE_A619_START_DOMAIN);
    assert(f->sequence++==3&&s==&f->smmu&&s->prerequisites==44);
    f->qualifications++;
    if(f->mutate_qualifier)f->smmu.prerequisites=63; /* Forbidden provider mutation. */
    return f->qualifier;
}
static int rbbm_ready(void *p){return dre_a619_startup_rbbm_ready(&((struct fixture *)p)->state);}
static int cp_ready(void *p){return dre_a619_startup_cp_ready(&((struct fixture *)p)->state);}
static int mapping(void *p,const struct dre_a619_cp_buffer *b,uint32_t access)
{(void)p;(void)b;(void)access;return 1;}
static int firmware(void *p,const void *b,size_t n){(void)p;(void)b;(void)n;return 1;}
static int authenticate(void *p){(void)p;return 1;}

/* Deliberate boundary substitutes, not production readiness providers. */
const char *dre_holi_power_acquire(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,
    const struct dre_rpm_votes *v,const struct dre_holi_power_config *c)
{
    struct fixture *f=o->cookie;assert(f==active&&f->sequence++==0);
    assert(f->state.phase==DRE_A619_START_POWER&&v==&f->votes&&c==&f->state.plan.power_config);
    s->changed=!f->power_untouched;
    if(f->fail_at==DRE_A619_START_POWER)return "TEST_POWER_REFUSAL";
    s->active=1;return "HOLI_DOMAINS_BRANCHES_ACQUIRED";
}
const char *dre_holi_power_validate(struct dre_holi_power *s,const struct dre_rpm_mmio_ops *o,
    const struct dre_rpm_votes *v)
{assert(s==&active->state.power&&o->cookie==active&&v==&active->votes);return "HOLI_DOMAINS_BRANCHES_ACQUIRED";}
const char *dre_a619_holi_sptprac_enable(struct dre_a619_holi_power *s,const struct dre_a619_ops *o)
{
    struct fixture *f=o->cookie;assert(f->sequence++==1&&f->state.phase==DRE_A619_START_SPTPRAC);
    assert(s->prerequisites==15);s->changed=1;
    if(f->fail_at==DRE_A619_START_SPTPRAC)return "TEST_SPTPRAC_REFUSAL";
    s->active=1;return "SPTPRAC_ENABLED";
}
const char *dre_a619_holi_sptprac_check_live(const struct dre_a619_holi_power *s,const struct dre_a619_ops *o)
{assert(o->cookie==active&&s->active);return "SPTPRAC_HELD";}
const char *dre_a619_rbbm_initialize(struct dre_a619_rbbm *s,const struct dre_a619_rbbm_ops *o)
{
    struct fixture *f=o->io.cookie;assert(f->sequence++==2&&f->state.phase==DRE_A619_START_RBBM);
    assert(o->providers_ready(f)==1);s->changed=1;
    if(f->fail_at==DRE_A619_START_RBBM)return "TEST_RBBM_REFUSAL";
    s->active=1;return "RBBM_INITIALIZED";
}
const char *dre_a619_smmu_activate(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    struct fixture *f=o->cookie;assert(f->sequence++==4&&f->state.phase==DRE_A619_START_DOMAIN);
    assert(s->prerequisites==63&&f->qualifications==1);f->domain_calls++;s->changed=1;
    if(f->fail_at==DRE_A619_START_DOMAIN)return "TEST_DOMAIN_REFUSAL";
    s->active=1;return "SMMU_ACTIVE";
}
const char *dre_a619_vm_activate(struct dre_a619_vm *s,const struct dre_a619_vm_ops *o)
{assert(s==&active->vm);return dre_a619_smmu_activate(s->domain,&o->mmu);}
const char *dre_a619_smmu_check_live(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{assert(s==&active->smmu&&s->active&&o->cookie==active);return "SMMU_LIVE_STATE_VERIFIED";}
const char *dre_a619_cp_start(struct dre_a619_cp *s,const struct dre_a619_cp_ops *o,uint64_t budget)
{
    struct fixture *f=o->io.cookie;assert(f->sequence++==5&&f->state.phase==DRE_A619_START_CP&&budget==100);
    assert(o->platform_ready(f)==1);f->cp_calls++;s->changed=1;
    if(f->fail_at==DRE_A619_START_CP)return "TEST_CP_REFUSAL";
    s->active=1;s->queue->submitted=1;s->queue->completed=f->bad_marker?0:1;s->queue->ready_mask=31;
    return "NATIVE_CP_STARTED";
}
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));active=f;
    f->lease=77;f->owner=f->policy=f->cold=f->qualifier=1;
    f->votes.link.lease=f->lease;f->votes.link.open=1;
    f->rbbm.chip_id=f->smmu.chip_id=f->cp.chip_id=f->queue.chip_id=DRE_A619_CHIP_ID;
    f->smmu.prerequisites=44;f->cp.queue=&f->queue;
    f->plan.mmio=(struct dre_rpm_mmio_ops){lease,read32,write32,barrier,now,f};
    f->plan.io=(struct dre_a619_ops){guard,read32,write32,barrier,now,f};
    f->plan.rbbm_ops=(struct dre_a619_rbbm_ops){f->plan.io,rbbm_ready};
    f->plan.smmu_ops=(struct dre_a619_smmu_ops){guard,owns_ram,read32,write32,read64,write64,barrier,now,f};
    f->plan.cp_ops=(struct dre_a619_cp_ops){f->plan.io,cp_ready,mapping,firmware,authenticate};
    f->plan.votes=&f->votes;f->plan.rbbm=&f->rbbm;f->plan.smmu=&f->smmu;f->plan.cp=&f->cp;
    f->plan.power_config.chip_id=DRE_A619_CHIP_ID;f->plan.power_config.timer_hz=19200000;
    f->plan.cp_budget_ticks=100;f->plan.policy_held=policy;f->plan.cold_owner=cold;f->plan.qualify_domain=qualify;
}
static void expect(struct fixture *f,const char *want)
{const char *got=dre_a619_startup_begin(&f->state,&f->plan);if(strcmp(got,want))fprintf(stderr,"expected %s, got %s\n",want,got);assert(!strcmp(got,want));}
int main(void)
{
    struct fixture f;unsigned cases=0;
    init(&f);expect(&f,"NATIVE_START_MARKER_CONFIRMED");cases++;
    assert(f.sequence==6&&f.qualifications==1&&f.domain_calls==1&&f.cp_calls==1);
    unsigned cold_calls=f.cold_calls;f.cold=0;
    assert(!strcmp(dre_a619_startup_check_live(&f.state),"NATIVE_START_HELD")&&f.cold_calls==cold_calls);cases++;
    expect(&f,"START_REQUIRES_FRESH_OWNER");assert(f.sequence==6);cases++;
    f.owner=-1;assert(!strcmp(dre_a619_startup_check_live(&f.state),"NATIVE_RUNNING_PROVIDER_LOST"));
    assert(f.state.hold_resources&&f.state.reset_required&&f.queue.poisoned);cases++;
    init(&f);f.vm.domain=&f.smmu;f.vm_ops.mmu=f.plan.smmu_ops;f.plan.vm=&f.vm;f.plan.vm_ops=&f.vm_ops;
    expect(&f,"NATIVE_START_MARKER_CONFIRMED");assert(f.domain_calls==1);cases++;
    for(int proof=0;proof<3;proof++)for(int value=-1;value<=0;value++) {
        init(&f);if(proof==0)f.owner=value;else if(proof==1)f.policy=value;else f.cold=value;
        expect(&f,"NATIVE_OWNERSHIP_OR_POLICY_UNQUALIFIED");
        assert(!f.sequence&&!f.state.reset_required&&f.state.hold_resources);cases++;
    }
    for(int value=-1;value<=0;value++) {
        init(&f);f.qualifier=value;expect(&f,"NATIVE_DOMAIN_LIVE_PROVIDERS_UNQUALIFIED");
        assert(f.qualifications==1&&!f.domain_calls&&!f.cp_calls&&f.smmu.prerequisites==44);
        assert(f.state.reset_required&&f.state.hold_resources&&f.queue.poisoned);cases++;
    }
    init(&f);f.mutate_qualifier=1;expect(&f,"NATIVE_DOMAIN_LIVE_PROVIDERS_UNQUALIFIED");
    assert(!f.domain_calls&&!f.cp_calls);cases++;
    init(&f);f.smmu.prerequisites=63;expect(&f,"NATIVE_DOMAIN_LIVE_PROVIDERS_UNQUALIFIED");
    assert(!f.qualifications&&!f.domain_calls);cases++;
    const char *failures[]={"TEST_POWER_REFUSAL","TEST_SPTPRAC_REFUSAL","TEST_RBBM_REFUSAL","TEST_DOMAIN_REFUSAL","TEST_CP_REFUSAL"};
    for(int phase=DRE_A619_START_POWER;phase<=DRE_A619_START_CP;phase++) {
        init(&f);f.fail_at=phase;expect(&f,failures[phase-1]);
        assert(f.state.failed_phase==(enum dre_a619_start_phase)phase&&f.state.hold_resources&&f.state.reset_required&&f.queue.poisoned);cases++;
    }
    init(&f);f.power_untouched=1;f.fail_at=DRE_A619_START_POWER;expect(&f,"TEST_POWER_REFUSAL");
    assert(!f.state.reset_required&&f.state.hold_resources&&!f.queue.poisoned);cases++;
    init(&f);f.bad_marker=1;expect(&f,"CP_START_POSTCONDITION_UNVERIFIED");assert(f.state.reset_required&&f.queue.poisoned);cases++;
    init(&f);f.plan.qualify_domain=NULL;expect(&f,"START_BINDING_INCOMPLETE");assert(!f.state.hold_resources&&!f.sequence);cases++;
    init(&f);f.plan.cp_ops.io.cookie=NULL;expect(&f,"START_BINDING_INCOMPLETE");assert(!f.sequence);cases++;
    init(&f);f.plan.power_config.chip_id=0;expect(&f,"NATIVE_START_PROFILE_NOT_FRESH");assert(!f.sequence&&f.state.hold_resources);cases++;
    init(&f);expect(&f,"NATIVE_START_MARKER_CONFIRMED");
    dre_a619_startup_invalidate(&f.state,"TEST_IRQ_LOST");
    assert(f.state.phase==DRE_A619_START_FAILED&&f.state.failed_phase==DRE_A619_START_MARKER_CONFIRMED&&f.state.reset_required&&f.state.hold_resources&&f.queue.poisoned);cases++;
    printf("PASS %u startup provider-boundary cases; no hardware execution\n",cases);
    return 0;
}
