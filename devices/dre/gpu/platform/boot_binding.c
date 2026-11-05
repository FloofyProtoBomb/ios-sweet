/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source-only integration. No execution has been run.
 * Real allocator/firmware/router/coordinator composition, no native policy
 * factory, cache-completion shortcut, PAS bypass or synthetic Metal device.
 */
#include "boot_binding.h"

#define RESERVED_FACTS (DRE_A619_SMMU_CB_OWNED|DRE_A619_SMMU_ASID_OWNED)
#define CPU_FACTS (RESERVED_FACTS|DRE_A619_SMMU_TABLES_NC)

static int equal(const char *a,const char *b)
{ if(!a||!b)return 0;while(*a&&*a==*b){++a;++b;}return *a==*b; }
static const char *fail(struct dre_gpu_boot_binding *b,const char *reason)
{ if(b)b->error=reason;return reason; }
static int range(const void *p,size_t n)
{ return p&&n&&(uintptr_t)p<=UINTPTR_MAX-n; }
static int separate(const void *a,size_t n,const void *b,size_t m)
{
    return range(a,n)&&range(b,m)&&
        ((uintptr_t)a+n<=(uintptr_t)b||(uintptr_t)b+m<=(uintptr_t)a);
}
static int allocator_complete(const struct dre_gpu_native_allocator *a)
{
    return a&&a->allocate&&a->allocate_contiguous&&a->owns_cpu&&a->owns_ram&&a->verify_memory;
}
static int same_mmu(const struct dre_a619_smmu_ops *a,const struct dre_a619_smmu_ops *b)
{
    return a->cookie==b->cookie&&a->guard==b->guard&&a->owns_ram==b->owns_ram&&
        a->read32==b->read32&&a->write32==b->write32&&a->read64==b->read64&&
        a->write64==b->write64&&a->barrier==b->barrier&&a->now==b->now;
}
static int same_vm(const struct dre_a619_vm_ops *a,const struct dre_a619_vm_ops *b)
{
    return same_mmu(&a->mmu,&b->mmu)&&a->owns_cpu==b->owns_cpu&&
        a->table_mapping==b->table_mapping&&a->quiescent==b->quiescent;
}
/* This selects only immutable reservation identity, not table geometry, power
 * readiness, a live context bank, or resource ownership by matching numbers.
 * Actual external reservation/live callbacks still have to prove ownership.
 */
static int same_reservation(const struct dre_gpu_boot_binding *b,const struct dre_a619_smmu *s)
{
    const struct dre_a619_smmu *r=&b->reservation;
    return s&&s->chip_id==r->chip_id&&s->tick_hz==r->tick_hz&&
        s->context_bank==r->context_bank&&s->smr_index==r->smr_index&&
        s->asid==r->asid&&s->va_base==r->va_base&&!s->unsafe;
}
static struct dre_gpu_boot_binding *binding(void *cookie)
{
    struct dre_gpu_io_router *r=cookie;
    struct dre_gpu_boot_binding *b=dre_gpu_io_router_context(r);
    if(!b||!b->bound||b->router!=r||!allocator_complete(&b->allocator)||
       !b->memory||!b->startup||!b->firmware_prepared||!b->firmware||
       b->firmware_ops.lease!=r->lease||!r->lease||
       dre_gpu_io_router_guard(r)!=1||
       b->allocator.owns_cpu(b->allocator.cookie,b,sizeof(*b))!=1||
       b->allocator.owns_cpu(b->allocator.cookie,b->memory,sizeof(*b->memory))!=1||
       b->memory->unsafe||b->memory->vm.unsafe)return 0;
    if(b->memory->initialized&&!same_vm(&b->memory->ops,&b->vm_ops))return 0;
    return b;
}
static const struct dre_a619_smmu *current_domain(const struct dre_gpu_boot_binding *b)
{ return b->memory->attempted?&b->memory->domain:&b->reservation; }
/* Every callback is synchronous under the actual common owner mutex. Refuse
 * recursion rather than allowing external policy to reenter MMU validation.
 * Raw owner guards, SCM guards and external predicates must stay nonrecursive.
 */
enum domain_check { CHECK_RESERVED=1,CHECK_LIVE=2,CHECK_QUIESCENT=3 };
static int domain_proof(struct dre_gpu_boot_binding *b,const struct dre_a619_smmu *s,
                        enum domain_check which)
{
    if(!same_reservation(b,s)||!b->domain.reservation_lease||b->checking_domain||
       dre_gpu_io_router_guard(b->router)!=1)return 0;
    b->checking_domain=1;
    int good=b->domain.reserved(b->domain.cookie,b->router->lease,
        b->domain.reservation_lease,s)==1;
    if(good&&which>=CHECK_LIVE)good=b->domain.live(b->domain.cookie,b->router->lease,
        b->domain.reservation_lease,s)==1;
    if(good&&which>=CHECK_QUIESCENT)good=b->domain.quiescent(b->domain.cookie,b->router->lease,
        b->domain.reservation_lease,s)==1;
    b->checking_domain=0;
    return good&&same_reservation(b,s)&&dre_gpu_io_router_guard(b->router)==1;
}
static int table_prefix(struct dre_gpu_boot_binding *b)
{
    struct dre_a619_smmu *s=&b->memory->domain;
    return same_reservation(b,s)&&dre_gpu_boot_table_mapping(b->memory,
        s->table_pa,s->tables,s->table_capacity_bytes)==1;
}
static int mmu_guard(void *cookie)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    if(!b)return 0;
    const struct dre_a619_smmu *s=current_domain(b);
    if(b->preparing_cpu) {
        /* No caller-controlled scope API. These facts hold only while this
         * TU invokes the two actual known CPU-only helpers below. Nothing
         * device-visible is invoked inside that private synchronous scope.
         */
        return b->preparing_cpu==1&&b->memory_attempted==1&&!b->memory_prepared&&
            !b->plan_bound&&b->startup->phase==DRE_A619_START_EMPTY&&
            !b->startup->lease&&!b->startup->hold_resources&&!b->startup->reset_required&&
            !s->active&&!s->changed&&
            (s->prerequisites==RESERVED_FACTS||s->prerequisites==CPU_FACTS)&&
            domain_proof(b,s,CHECK_RESERVED);
    }
    /* Once preparation returns, even an inactive VM requires real current
     * power/global/TBU/domain proof. In particular, phaseDOMAIN cannot use
     * CPU-only reservation semantics to bypass real powered qualification.
     * No cold-idle test here: a genuinely owned CP may be executing later.
     */
    return b->memory_prepared&&
        (s->prerequisites==CPU_FACTS||s->prerequisites==DRE_A619_SMMU_PREREQUISITES)&&
        table_prefix(b)&&domain_proof(b,s,CHECK_LIVE);
}
static int owns_cpu(void *cookie,const void *cpu,size_t bytes)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    return b&&range(cpu,bytes)&&b->allocator.owns_cpu(b->allocator.cookie,cpu,bytes)==1&&
        dre_gpu_io_router_guard(b->router)==1;
}
static int owns_ram(void *cookie,uint64_t pa,size_t bytes)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    return b&&bytes&&pa<=UINT64_MAX-bytes&&
        b->allocator.owns_ram(b->allocator.cookie,pa,bytes)==1&&
        dre_gpu_io_router_guard(b->router)==1;
}
static int table_mapping(void *cookie,uint64_t pa,const void *cpu,size_t bytes)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    return b&&same_reservation(b,&b->memory->domain)&&
        dre_gpu_boot_table_mapping(b->memory,pa,cpu,bytes)==1&&
        dre_gpu_io_router_guard(b->router)==1;
}
static int quiescent(void *cookie)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    return b&&!b->preparing_cpu&&b->memory_prepared&&table_prefix(b)&&
        domain_proof(b,&b->memory->domain,CHECK_QUIESCENT);
}
static int fresh_reservation(const struct dre_a619_smmu *s)
{
    return s&&s->chip_id==DRE_A619_CHIP_ID&&s->tick_hz==19200000&&
        s->prerequisites==RESERVED_FACTS&&s->context_bank<8&&s->smr_index<128&&
        s->asid&&s->asid<=255&&!(s->va_base&0x1fffff)&&s->va_base<(UINT64_C(1)<<39)&&
        !s->active&&!s->changed&&!s->unsafe&&!s->tables&&!s->table_pa&&!s->pages&&
        !s->page_count&&!s->page_capacity&&!s->table_capacity_bytes&&!s->cb_base&&
        !s->owned_tables&&!s->owned_pages&&!s->owned_table_pa&&!s->owned_page_count;
}
const char *dre_gpu_boot_binding_firmware_prepare(struct dre_gpu_boot_binding *b,
    struct dre_gpu_fw_native *fw,const struct dre_gpu_fw_native_ops *ops)
{
    if(!b||!fw||!ops||!ops->lease)return "BOOT_BINDING_FIRMWARE_ARGUMENT_REQUIRED";
    if(b->firmware_attempted||b->firmware_prepared||b->firmware||b->bound||
       b->memory_attempted||b->plan_bound||b->preparing_cpu)
        return "BOOT_BINDING_FRESH_FIRMWARE_OWNER_REQUIRED";
    /* Record the actual partial native owner BEFORE any mapping/preparation.
     * Never erase it or retry with a wider shared-page alias after refusal.
     */
    b->firmware_attempted=1;b->firmware=fw;b->firmware_ops=*ops;
    const char *status=dre_gpu_fw_native_prepare_pas13(fw,ops);
    if(!equal(status,"FW_NATIVE_PAS_BUFFERS_PREPARED_ONLY"))return fail(b,status);
    struct dre_gpu_fw_view source={0};
    if(dre_gpu_fw_native_source(fw,DRE_GPU_FW_SQE,&source)!=1||
       !source.bytes||!source.size||source.size>DRE_GPU_FW_FILE_MAX||
       dre_gpu_fw_native_sqe_verified(fw,source.bytes,source.size)!=1)
        return fail(b,"BOOT_BINDING_ACTUAL_PRIVATE_SQE_SOURCE_UNQUALIFIED");
    b->sqe_file=source.bytes;b->sqe_bytes=source.size;b->firmware_prepared=1;
    b->error=0;return status;
}
int dre_gpu_boot_binding_init(struct dre_gpu_boot_binding *b,struct dre_gpu_io_router *r,
    const struct dre_gpu_native_allocator *a,struct dre_a619_startup *startup,
    struct dre_gpu_boot_memory *memory,const struct dre_a619_smmu *reservation,
    const struct dre_gpu_boot_domain_ops *domain)
{
    if(!b||!r||!allocator_complete(a)||!startup||!memory||!domain||
       !domain->reservation_lease||!domain->reserved||!domain->live||!domain->quiescent||
       !fresh_reservation(reservation))return DRE_GPU_INVALID;
    if(b->bound||b->router||b->memory||b->startup||b->memory_attempted||b->memory_prepared||
       b->plan_bound||b->preparing_cpu||b->checking_domain||b->private_queue||
       b->firmware_attempted!=1||b->firmware_prepared!=1||!b->firmware||
       !b->sqe_file||!b->sqe_bytes||b->firmware_ops.lease!=r->lease||!r->lease||
       dre_gpu_io_router_context(r)!=b||dre_gpu_io_router_guard(r)!=1||
       !separate(b,sizeof(*b),memory,sizeof(*memory))||
       a->owns_cpu(a->cookie,b,sizeof(*b))!=1||
       a->owns_cpu(a->cookie,memory,sizeof(*memory))!=1||
       memory->attempted||memory->held||memory->initialized||memory->unsafe||
       startup->phase!=DRE_A619_START_EMPTY||startup->lease||startup->hold_resources||
       startup->reset_required||dre_gpu_fw_native_sqe_verified(b->firmware,b->sqe_file,b->sqe_bytes)!=1)
        return DRE_GPU_NOT_READY;
    struct dre_gpu_native_io io={0};struct dre_a619_start_plan template={0};
    int result=dre_gpu_io_router_native_io(r,&io);
    if(result!=DRE_GPU_OK)return result;
    result=dre_gpu_io_router_plan_io(r,&template);
    if(result!=DRE_GPU_OK)return result;
    b->router=r;b->allocator=*a;b->domain=*domain;b->reservation=*reservation;
    b->startup=startup;b->memory=memory;
    b->vm_ops=(struct dre_a619_vm_ops){.mmu=template.smmu_ops,
        .owns_cpu=owns_cpu,.table_mapping=table_mapping,.quiescent=quiescent};
    b->vm_ops.mmu.guard=mmu_guard;b->vm_ops.mmu.owns_ram=owns_ram;
    /* VM checks actual GPU idle/fault/RPTR/WPTR via this same read32 as well
     * as its SMMU registers. The genuine GPU-only router view covers both
     * qualified apertures; SMMU stores/read64 remain SMMU-scoped. No new
     * aperture, raw caller pointer, unrelated device or RPM access is added.
     */
    b->vm_ops.mmu.read32=io.read32;b->bound=1;
    if(!binding(r)||!domain_proof(b,&b->reservation,CHECK_RESERVED)) {
        b->error="BOOT_BINDING_REAL_RESERVATION_OWNER_UNQUALIFIED";
        return DRE_GPU_NOT_READY;
    }
    b->error=0;return DRE_GPU_OK;
}
const char *dre_gpu_boot_binding_prepare(struct dre_gpu_boot_binding *b,uint32_t window_pages)
{
    if(!b||binding(b->router)!=b)return "BOOT_BINDING_ACTUAL_OWNER_REQUIRED";
    if(b->memory_attempted||b->memory_prepared||b->plan_bound||b->private_queue||
       b->preparing_cpu||b->checking_domain||b->startup->phase!=DRE_A619_START_EMPTY||
       b->startup->lease||b->startup->hold_resources||b->startup->reset_required)
        return fail(b,"BOOT_BINDING_FRESH_CPU_PREPARATION_REQUIRED");
    if(!domain_proof(b,&b->reservation,CHECK_RESERVED)||
       dre_gpu_fw_native_sqe_verified(b->firmware,b->sqe_file,b->sqe_bytes)!=1)
        return fail(b,"BOOT_BINDING_RESERVED_DOMAIN_OR_SQE_OWNER_LOST");
    b->memory_attempted=1;b->preparing_cpu=1;
    const char *status=dre_gpu_boot_memory_prepare(b->memory,&b->allocator,&b->vm_ops,
        &b->reservation,window_pages,b->sqe_file,b->sqe_bytes);
    if(equal(status,"BOOT_MEMORY_STAGED")) {
        /* The queue retains this pointer, and startup binds &memory->ops,
         * not &binding->vm_ops. Keep actual table-pointer identity intact.
         * A partial opaque queue remains reachable even when staging fails.
         */
        status=dre_gpu_trusted_prepare(&b->allocator,&b->memory->vm,&b->memory->ops,
            b->memory->first_unused_gpu_va,&b->private_queue_state,&b->private_queue);
    }
    /* No early return inside the private CPU-only preparation scope. */
    b->preparing_cpu=0;
    if(!equal(status,"TRUSTED_PRIVATE_IB_BUFFERS_STAGED"))return fail(b,status);
    if(!b->private_queue||!b->memory->initialized||!same_vm(&b->memory->ops,&b->vm_ops)||
       b->memory->domain.prerequisites!=CPU_FACTS||!table_prefix(b)||
       !domain_proof(b,&b->memory->domain,CHECK_RESERVED)||
       dre_gpu_fw_native_sqe_verified(b->firmware,b->sqe_file,b->sqe_bytes)!=1)
        return fail(b,"BOOT_BINDING_CPU_STAGE_POSTCONDITION_UNQUALIFIED");
    b->memory_prepared=1;b->error=0;return "BOOT_BINDING_CPU_STAGED";
}
int dre_gpu_boot_binding_vm_ops(struct dre_gpu_boot_binding *b,struct dre_a619_vm_ops *out)
{
    if(!b||!out)return DRE_GPU_INVALID;
    if(binding(b->router)!=b||b->preparing_cpu||!b->memory_prepared)return DRE_GPU_NOT_READY;
    *out=b->memory->ops;return DRE_GPU_OK;
}
static int rbbm_ready(void *cookie);
static int cp_ready(void *cookie);
static int cp_mapping(void *cookie,const struct dre_a619_cp_buffer *,uint32_t);
static int firmware_verified(void *cookie,const void *,size_t);
static int authenticate_zap(void *cookie);
static int startup_identity(struct dre_gpu_boot_binding *b)
{
    const struct dre_a619_start_plan *p=&b->startup->plan;
    return b->plan_bound&&!b->preparing_cpu&&b->memory_prepared&&
        b->startup->lease==b->router->lease&&b->startup->lease&&
        p->io.cookie==b->router&&p->mmio.cookie==b->router&&
        p->rbbm==b->rbbm&&p->votes==b->votes&&p->smmu==&b->memory->domain&&
        p->cp==&b->memory->cp&&p->cp->queue==&b->memory->queue&&
        p->vm==&b->memory->vm&&p->vm_ops==&b->memory->ops&&
        same_mmu(&p->smmu_ops,&b->memory->ops.mmu)&&
        p->qualify_domain==dre_gpu_boot_binding_qualify_domain&&
        p->rbbm_ops.providers_ready==rbbm_ready&&p->rbbm_ops.io.cookie==b->router&&
        p->cp_ops.io.cookie==b->router&&p->cp_ops.platform_ready==cp_ready&&
        p->cp_ops.mapping==cp_mapping&&p->cp_ops.firmware_verified==firmware_verified&&
        p->cp_ops.authenticate_zap==authenticate_zap;
}
static int rbbm_ready(void *cookie)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    return b&&startup_identity(b)&&dre_a619_startup_rbbm_ready(b->startup)==1&&
        dre_gpu_io_router_guard(b->router)==1;
}
static int cp_ready(void *cookie)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    return b&&startup_identity(b)&&dre_a619_startup_cp_ready(b->startup)==1&&
        dre_gpu_io_router_guard(b->router)==1;
}
static int cp_mapping(void *cookie,const struct dre_a619_cp_buffer *buffer,uint32_t permissions)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    return b&&startup_identity(b)&&dre_gpu_boot_cp_mapping(b->memory,buffer,permissions)==1&&
        dre_gpu_io_router_guard(b->router)==1;
}
static int firmware_verified(void *cookie,const void *file,size_t bytes)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    return b&&startup_identity(b)&&b->startup->phase==DRE_A619_START_CP&&
        file==b->sqe_file&&bytes==b->sqe_bytes&&b->memory->cp.firmware==file&&
        b->memory->cp.firmware_bytes==bytes&&
        dre_gpu_fw_native_sqe_verified(b->firmware,file,bytes)==1&&
        dre_gpu_io_router_guard(b->router)==1;
}
static int authenticate_zap(void *cookie)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    if(!b||!startup_identity(b)||b->startup->phase!=DRE_A619_START_CP||
       b->memory->cp.stage!=3||!cp_ready(cookie)||
       !firmware_verified(cookie,b->sqe_file,b->sqe_bytes))return 0;
    const char *status=dre_gpu_fw_native_authenticate(b->firmware);
    if(!equal(status,"PAS13_AUTHENTICATED")) {
        b->error=status?status:"BOOT_BINDING_PAS13_NATIVE_STATUS_MISSING";return 0;
    }
    return b->memory->cp.stage==3&&cp_ready(cookie);
}
const char *dre_gpu_boot_binding_plan(struct dre_gpu_boot_binding *b,struct dre_a619_start_plan *plan)
{
    if(!b||!plan||binding(b->router)!=b)return "BOOT_BINDING_ACTUAL_PLAN_OWNER_REQUIRED";
    if(b->plan_bound||b->preparing_cpu||b->checking_domain||!b->memory_prepared||
       !b->private_queue||!b->memory->initialized||b->memory->domain.active||
       b->memory->domain.changed||b->memory->domain.prerequisites!=CPU_FACTS||
       b->startup->phase!=DRE_A619_START_EMPTY||b->startup->lease||
       b->startup->hold_resources||b->startup->reset_required||!plan->rbbm||!plan->votes||
       !plan->cp_budget_ticks||plan->cp_budget_ticks>19200000||
       b->allocator.owns_cpu(b->allocator.cookie,plan->rbbm,sizeof(*plan->rbbm))!=1||
       b->allocator.owns_cpu(b->allocator.cookie,plan->votes,sizeof(*plan->votes))!=1||
       !separate(plan,sizeof(*plan),b,sizeof(*b))||
       !separate(plan,sizeof(*plan),b->memory,sizeof(*b->memory))||
       (plan->smmu&&plan->smmu!=&b->memory->domain)||
       (plan->cp&&plan->cp!=&b->memory->cp)||plan->vm||plan->vm_ops||
       (plan->rbbm_ops.providers_ready&&plan->rbbm_ops.providers_ready!=rbbm_ready)||
       (plan->cp_ops.platform_ready&&plan->cp_ops.platform_ready!=cp_ready)||
       (plan->cp_ops.mapping&&plan->cp_ops.mapping!=cp_mapping)||
       (plan->cp_ops.firmware_verified&&plan->cp_ops.firmware_verified!=firmware_verified)||
       (plan->cp_ops.authenticate_zap&&plan->cp_ops.authenticate_zap!=authenticate_zap)||
       (plan->smmu_ops.guard&&plan->smmu_ops.guard!=mmu_guard)||
       (plan->smmu_ops.owns_ram&&plan->smmu_ops.owns_ram!=owns_ram)||
       (plan->qualify_domain&&plan->qualify_domain!=dre_gpu_boot_binding_qualify_domain))
        return fail(b,"BOOT_BINDING_FRESH_PLAN_OR_REAL_POLICY_REQUIRED");
    struct dre_a619_start_plan next=*plan;
    if(dre_gpu_io_router_plan_io(b->router,&next)!=DRE_GPU_OK)
        return fail(b,"BOOT_BINDING_ROUTER_PLAN_CONFLICT");
    const char *status=dre_gpu_boot_memory_bind(b->memory,&next);
    if(!equal(status,"BOOT_MEMORY_PLAN_BOUND"))return fail(b,status);
    next.rbbm_ops.providers_ready=rbbm_ready;next.cp_ops.platform_ready=cp_ready;
    next.cp_ops.mapping=cp_mapping;next.cp_ops.firmware_verified=firmware_verified;
    next.cp_ops.authenticate_zap=authenticate_zap;
    next.qualify_domain=dre_gpu_boot_binding_qualify_domain;
    if(!table_prefix(b)||!domain_proof(b,&b->memory->domain,CHECK_RESERVED))
        return fail(b,"BOOT_BINDING_RESERVED_PLAN_OWNER_LOST");
    b->rbbm=next.rbbm;b->votes=next.votes;*plan=next;
    b->plan_bound=1;b->error=0;return "BOOT_BINDING_PLAN_BOUND";
}
int dre_gpu_boot_binding_qualify_domain(void *cookie,const struct dre_a619_smmu *s)
{
    struct dre_gpu_boot_binding *b=binding(cookie);
    if(!b||!startup_identity(b)||b->preparing_cpu||
       b->startup->phase!=DRE_A619_START_DOMAIN||s!=&b->memory->domain||
       s->prerequisites!=CPU_FACTS||s->active||s->changed||s->unsafe||
       !table_prefix(b)||!domain_proof(b,s,CHECK_QUIESCENT))return 0;
    /* Coordinator, not this predicate, derives63 only AFTER this real proof
     * plus its own still-held power/cold-owner checks. Never set future bits.
     */
    return s->prerequisites==CPU_FACTS&&table_prefix(b)&&
        dre_gpu_io_router_guard(b->router)==1;
}
