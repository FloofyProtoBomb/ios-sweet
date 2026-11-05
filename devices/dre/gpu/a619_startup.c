/* SPDX-License-Identifier: MIT
 * UNVALIDATED source-only overnight integration; do not enable by default. */
#include "a619_startup.h"

static int equal(const char *a,const char *b)
{
    if (!a || !b)return 0;
    while (*a && *a==*b){++a;++b;}
    return *a==*b;
}
static int mutated(const struct dre_a619_startup *s)
{
    const struct dre_a619_start_plan *p=&s->plan;
    return s->power.changed || s->power.unsafe || s->sptprac.changed || s->sptprac.unsafe ||
        (p->rbbm && (p->rbbm->changed || p->rbbm->unsafe)) ||
        (p->smmu && (p->smmu->changed || p->smmu->unsafe)) ||
        (p->cp && (p->cp->changed || p->cp->unsafe ||
            (p->cp->queue && p->cp->queue->poisoned)));
}
static const char *fail(struct dre_a619_startup *s,const char *error)
{
    if (s->phase!=DRE_A619_START_FAILED)s->failed_phase=s->phase;
    s->error=error?error:"NATIVE_PROVIDER_UNAVAILABLE";
    s->reset_required|=mutated(s);
    s->hold_resources=1;
    s->phase=DRE_A619_START_FAILED;
    if (s->plan.cp && s->plan.cp->queue && s->reset_required)
        s->plan.cp->queue->poisoned=1;
    return s->error;
}
static int bound(const struct dre_a619_start_plan *p)
{
    return p && p->mmio.lease && p->mmio.read32 && p->mmio.write32 &&
        p->mmio.barrier && p->mmio.now && p->io.guard && p->io.read32 &&
        p->io.write32 && p->io.barrier && p->io.now && p->policy_held && p->cold_owner && p->qualify_domain &&
        p->votes && p->rbbm && p->smmu && p->cp && p->cp->queue &&
        p->rbbm_ops.providers_ready && p->cp_ops.platform_ready &&
        p->cp_ops.mapping && p->cp_ops.firmware_verified && p->cp_ops.authenticate_zap &&
        p->smmu_ops.guard && p->smmu_ops.owns_ram && p->smmu_ops.read32 &&
        p->smmu_ops.write32 && p->smmu_ops.read64 && p->smmu_ops.write64 &&
        p->smmu_ops.barrier && p->smmu_ops.now &&
        p->io.cookie==p->mmio.cookie && p->rbbm_ops.io.cookie==p->io.cookie &&
        p->smmu_ops.cookie==p->io.cookie && p->cp_ops.io.cookie==p->io.cookie &&
        p->rbbm_ops.io.guard==p->io.guard && p->rbbm_ops.io.read32==p->io.read32 &&
        p->rbbm_ops.io.write32==p->io.write32 && p->rbbm_ops.io.barrier==p->io.barrier &&
        p->rbbm_ops.io.now==p->io.now && p->cp_ops.io.guard==p->io.guard &&
        p->cp_ops.io.read32==p->io.read32 && p->cp_ops.io.write32==p->io.write32 &&
        p->cp_ops.io.barrier==p->io.barrier && p->cp_ops.io.now==p->io.now &&
        (!!p->vm==!!p->vm_ops) && (!p->vm || (p->vm->domain==p->smmu &&
            p->vm_ops->mmu.cookie==p->io.cookie &&
            p->vm_ops->mmu.guard==p->smmu_ops.guard &&
            p->vm_ops->mmu.owns_ram==p->smmu_ops.owns_ram &&
            p->vm_ops->mmu.read32==p->smmu_ops.read32 &&
            p->vm_ops->mmu.write32==p->smmu_ops.write32 &&
            p->vm_ops->mmu.read64==p->smmu_ops.read64 &&
            p->vm_ops->mmu.write64==p->smmu_ops.write64 &&
            p->vm_ops->mmu.barrier==p->smmu_ops.barrier &&
            p->vm_ops->mmu.now==p->smmu_ops.now)) &&
        p->cp_budget_ticks && p->cp_budget_ticks<=UINT64_C(19200000);
}
static int owner(struct dre_a619_startup *s)
{
    struct dre_a619_start_plan *p=&s->plan;
    return s->lease && p->mmio.lease(p->mmio.cookie)==s->lease &&
        p->votes->link.lease==s->lease && p->votes->link.open &&
        !p->votes->link.unsafe && !p->votes->unsafe && !p->votes->pending &&
        p->io.guard(p->io.cookie)==1 &&
        p->policy_held(p->io.cookie,s->lease,p->votes,&p->power_config)==1 &&
        p->mmio.lease(p->mmio.cookie)==s->lease;
}
static int power_live(struct dre_a619_startup *s)
{
    return owner(s) && s->power.active && !s->power.unsafe &&
        equal(dre_holi_power_validate(&s->power,&s->plan.mmio,s->plan.votes),"HOLI_DOMAINS_BRANCHES_ACQUIRED") &&
        equal(dre_a619_holi_sptprac_check_live(&s->sptprac,&s->plan.io),"SPTPRAC_HELD") && owner(s);
}
int dre_a619_startup_rbbm_ready(struct dre_a619_startup *s)
{
    return s && s->phase==DRE_A619_START_RBBM && power_live(s) &&
        s->plan.cold_owner(s->plan.io.cookie,s->lease)==1;
}
int dre_a619_startup_cp_ready(struct dre_a619_startup *s)
{
    if (!s || (s->phase!=DRE_A619_START_CP && s->phase!=DRE_A619_START_MARKER_CONFIRMED))return 0;
    if (!power_live(s) || !s->plan.rbbm->active || s->plan.rbbm->unsafe ||
        !s->plan.smmu->active || s->plan.smmu->unsafe)return 0;
    /* Live CB proof only. No cold-idle/reset condition is repeated while CP
     * executes ME_INIT or a job. VM updates have their own stronger drain. */
    const char *status=dre_a619_smmu_check_live(s->plan.smmu,&s->plan.smmu_ops);
    return equal(status,"SMMU_LIVE_STATE_VERIFIED") && owner(s);
}
const char *dre_a619_startup_begin(struct dre_a619_startup *s,
    const struct dre_a619_start_plan *plan)
{
    if (!s || !bound(plan))return "START_BINDING_INCOMPLETE";
    if (s->phase!=DRE_A619_START_EMPTY || s->lease || s->hold_resources || s->reset_required)
        return "START_REQUIRES_FRESH_OWNER";
    s->plan=*plan;
    s->lease=plan->mmio.lease(plan->mmio.cookie);
    if (!owner(s) || plan->cold_owner(plan->io.cookie,s->lease)!=1)
        return fail(s,"NATIVE_OWNERSHIP_OR_POLICY_UNQUALIFIED");
    if (plan->power_config.chip_id!=DRE_A619_CHIP_ID ||
        plan->power_config.timer_hz!=19200000 || plan->rbbm->chip_id!=DRE_A619_CHIP_ID ||
        plan->smmu->chip_id!=DRE_A619_CHIP_ID || plan->cp->chip_id!=DRE_A619_CHIP_ID ||
        plan->cp->queue->chip_id!=DRE_A619_CHIP_ID || plan->rbbm->active ||
        plan->rbbm->changed || plan->rbbm->unsafe || plan->smmu->active ||
        plan->smmu->changed || plan->smmu->unsafe || plan->cp->active ||
        plan->cp->changed || plan->cp->unsafe || plan->cp->queue->poisoned)
        return fail(s,"NATIVE_START_PROFILE_NOT_FRESH");
    s->hold_resources=1;
    s->phase=DRE_A619_START_POWER;
    const char *status=dre_holi_power_acquire(&s->power,&s->plan.mmio,
        s->plan.votes,&s->plan.power_config);
    if (!s->power.active || s->power.unsafe)return fail(s,status);
    if (!owner(s))return fail(s,"OWNERSHIP_LOST_AFTER_POWER");
    s->phase=DRE_A619_START_SPTPRAC;
    s->sptprac.chip_id=DRE_A619_CHIP_ID;
    s->sptprac.tick_hz=19200000;
    /* These bits follow actual held/observed providers plus cold-owner proof,
     * not DT identification. The leaf re-reads domains/branches itself. */
    if (plan->cold_owner(plan->io.cookie,s->lease)!=1)return fail(s,"COLD_OWNERSHIP_LOST");
    s->sptprac.prerequisites=DRE_A619_POWER_PREREQUISITES;
    status=dre_a619_holi_sptprac_enable(&s->sptprac,&s->plan.io);
    if (!s->sptprac.active || s->sptprac.unsafe)return fail(s,status);
    if (!owner(s))return fail(s,"OWNERSHIP_LOST_AFTER_SPTPRAC");
    s->phase=DRE_A619_START_RBBM;
    status=dre_a619_rbbm_initialize(s->plan.rbbm,&s->plan.rbbm_ops);
    if (!s->plan.rbbm->active || s->plan.rbbm->unsafe)return fail(s,status);
    if (!power_live(s))return fail(s,"RBBM_PROVIDER_HOLD_LOST");
    s->phase=DRE_A619_START_DOMAIN;
    if (plan->cold_owner(plan->io.cookie,s->lease)!=1)return fail(s,"COLD_OWNERSHIP_LOST");
    const uint32_t cpu_facts=DRE_A619_SMMU_CB_OWNED|DRE_A619_SMMU_ASID_OWNED|DRE_A619_SMMU_TABLES_NC;
    if (s->plan.smmu->prerequisites!=cpu_facts ||
        s->plan.qualify_domain(s->plan.io.cookie,s->plan.smmu)!=1 ||
        s->plan.smmu->prerequisites!=cpu_facts || !power_live(s) ||
        s->plan.cold_owner(s->plan.io.cookie,s->lease)!=1)
        return fail(s,"NATIVE_DOMAIN_LIVE_PROVIDERS_UNQUALIFIED");
    /* These existing bits now follow actual held post-power observations,
     * not reservations or tables that had not been allocated yet. */
    s->plan.smmu->prerequisites=cpu_facts|DRE_A619_SMMU_POWERED|
        DRE_A619_SMMU_QUIESCENT|DRE_A619_SMMU_GLOBAL_READY;
    if (s->plan.vm)status=dre_a619_vm_activate(s->plan.vm,s->plan.vm_ops);
    else status=dre_a619_smmu_activate(s->plan.smmu,&s->plan.smmu_ops);
    if (!s->plan.smmu->active || s->plan.smmu->unsafe ||
        (s->plan.vm && s->plan.vm->unsafe))return fail(s,status);
    if (!power_live(s))return fail(s,"DOMAIN_PROVIDER_HOLD_LOST");
    s->phase=DRE_A619_START_CP;
    status=dre_a619_cp_start(s->plan.cp,&s->plan.cp_ops,s->plan.cp_budget_ticks);
    struct dre_a619_queue *q=s->plan.cp->queue;
    if (!equal(status,"NATIVE_CP_STARTED"))return fail(s,status);
    if (!s->plan.cp->active || s->plan.cp->unsafe ||
        q->poisoned || q->ready_mask!=DRE_A619_READY_MASK || !q->submitted ||
        q->completed!=q->submitted || !dre_a619_startup_cp_ready(s))
        return fail(s,"CP_START_POSTCONDITION_UNVERIFIED");
    s->phase=DRE_A619_START_MARKER_CONFIRMED;
    s->error=0;
    return "NATIVE_START_MARKER_CONFIRMED";
}
const char *dre_a619_startup_check_live(struct dre_a619_startup *s)
{
    if (!s || s->phase!=DRE_A619_START_MARKER_CONFIRMED)return "NATIVE_START_NOT_CONFIRMED";
    if (s->reset_required || !s->plan.cp->active || s->plan.cp->unsafe ||
        !s->plan.cp->queue || s->plan.cp->queue->poisoned ||
        s->plan.cp->queue->ready_mask!=DRE_A619_READY_MASK || !dre_a619_startup_cp_ready(s))
        return fail(s,"NATIVE_RUNNING_PROVIDER_LOST");
    return "NATIVE_START_HELD";
}
void dre_a619_startup_invalidate(struct dre_a619_startup *s,const char *reason)
{
    if (!s || s->phase==DRE_A619_START_EMPTY)return;
    s->reset_required=1;
    (void)fail(s,reason?reason:"NATIVE_OWNER_INVALIDATED");
}
