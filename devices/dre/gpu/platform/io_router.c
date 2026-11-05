/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Routes real native GPU and RPM Device providers without
 * creating or weakening either provider's ownership/power/access policy.
 */
#include "io_router.h"
#include <string.h>

static int gpu_address(uint64_t pa,size_t bytes)
{
    /* Exact six original-FDT-qualified apertures of native_owner.cpp. This
     * outer dispatch is NOT per-register/power authorization: the real owner
     * independently checks its live map, reset/IRQ lease and mmio_access. */
    static const uint64_t aperture[][2]={
        {0x05900000,0x40000},{0x05940000,0x10000},{0x05961000,0x800},
        {0x0596a000,0x30000},{0x0599e000,0x1000},{0x01400000,0x1f0000}};
    /* GPUCC0x05990000..0x05999000 is inside the existing wrapper through
     *0x0599a000; preserve the same six-aperture, single-map ownership model. */
    if ((bytes!=4 && bytes!=8) || (pa&(bytes-1))) return 0;
    for (unsigned i=0;i<sizeof(aperture)/sizeof(aperture[0]);i++)
        if (pa>=aperture[i][0] && bytes<=aperture[i][1] &&
            pa-aperture[i][0]<=aperture[i][1]-bytes) return 1;
    return 0;
}
static int smmu_address(uint64_t pa,size_t bytes)
{
    return (bytes==4 || bytes==8) && !(pa&(bytes-1)) &&
        pa>=DRE_A619_SMMU_BASE && pa-DRE_A619_SMMU_BASE<=0x10000-bytes;
}
static int rpm_address(const struct dre_gpu_io_router *r,uint64_t pa,int write)
{
    if (pa&3) return 0;
    if (write && pa==r->rpm_map.ipcc_base+0x0c) return 1;
    if (pa<r->rpm_map.message_base || r->rpm_map.message_bytes<4 ||
        pa-r->rpm_map.message_base>r->rpm_map.message_bytes-4) return 0;
    /* Foreign vMPM lives inside the advertised RPM RAM. Never dispatch its
     * words even if a malformed caller/TOC points there. Native RPM repeats
     * this check against its original-FDT protected consumer and word owner. */
    return !(pa<UINT64_C(0x045f11b8) && UINT64_C(0x045f01b8)<pa+4);
}
static int rpm_profile(const struct dre_rpm_mmio_map *m,const struct dre_rpm_config *p)
{
    return m->message_base==DRE_RPM_MSG_RAM_BASE && m->message_bytes==DRE_RPM_MSG_RAM_BYTES &&
        m->ipcc_base==DRE_RPM_IPCC_BASE && m->ipcc_bytes==DRE_RPM_IPCC_BYTES &&
        !m->client_id && !m->signal_id && p->physical_base==m->message_base &&
        p->bytes==m->message_bytes && p->timer_hz==DRE_RPM_TIMER_HZ &&
        p->channel_type==15 && !p->version_reg_present && !p->version_register;
}
static int same_gpu(const struct dre_gpu_native_io *a,const struct dre_gpu_native_io *b)
{
    return a->cookie==b->cookie && a->read32==b->read32 && a->write32==b->write32 &&
        a->read64==b->read64 && a->write64==b->write64 && a->barrier==b->barrier && a->now==b->now;
}
static int same_rpm(const struct dre_rpm_mmio_ops *a,const struct dre_rpm_mmio_ops *b)
{
    return a->cookie==b->cookie && a->lease==b->lease && a->read32==b->read32 &&
        a->write32==b->write32 && a->barrier==b->barrier && a->now==b->now;
}
static int same_profile(const struct dre_gpu_io_router *r,const struct dre_rpm_mmio_map *m,
                         const struct dre_rpm_config *p)
{
    /* The admitted Holi shape is exact. Revalidate fields without comparing
     * structure padding or trusting a retained mutable descriptor pointer. */
    return rpm_profile(m,p) && rpm_profile(&r->rpm_map,&r->rpm_protocol);
}
static int poison(struct dre_gpu_io_router *r)
{ if (r) __atomic_store_n(&r->unsafe,1,__ATOMIC_RELEASE); return 0; }
static int common(struct dre_gpu_io_router *r)
{
    if (!r || !r->bound || !r->lease || !r->owner || !r->rpm ||
        __atomic_load_n(&r->unsafe,__ATOMIC_ACQUIRE)) return 0;
    uint32_t expected=0;
    if (!__atomic_compare_exchange_n(&r->guarding,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return poison(r);
    struct dre_gpu_native_io gpu={0};
    struct dre_rpm_mmio_ops rpm={0};
    struct dre_rpm_mmio_map map={0};
    struct dre_rpm_config protocol={0};
    int good=dre_gpu_native_owner_guard(r->owner)==1 &&
        dre_gpu_native_owner_lease(r->owner)==r->lease &&
        dre_gpu_native_owner_io(r->owner,&gpu)==DRE_GPU_OK && same_gpu(&gpu,&r->gpu_io) &&
        dre_rpm_native_binding(r->rpm,&rpm,&map,&protocol)==DRE_GPU_OK &&
        same_rpm(&rpm,&r->rpm_io) && same_profile(r,&map,&protocol) &&
        rpm.lease(rpm.cookie)==r->lease && dre_gpu_native_owner_guard(r->owner)==1 &&
        dre_gpu_native_owner_lease(r->owner)==r->lease;
    __atomic_store_n(&r->guarding,0,__ATOMIC_RELEASE);
    if (!good || __atomic_load_n(&r->unsafe,__ATOMIC_ACQUIRE)) return poison(r);
    return 1;
}
int dre_gpu_io_router_bind(struct dre_gpu_io_router *r,struct dre_gpu_native_owner *owner,
                          struct dre_rpm_native *rpm,void *context)
{
    if (!r || !owner || !rpm || (void *)owner==(void *)r || (void *)rpm==(void *)r)
        return DRE_GPU_INVALID;
    if (r->bound || r->owner || r->rpm || r->lease || r->unsafe || r->guarding || r->accessing ||
        r->predicate || r->attempted_writes) return DRE_GPU_BUSY;
    struct dre_gpu_io_router next={0};
    next.owner=owner; next.rpm=rpm; next.context=context;
    if (dre_gpu_native_owner_guard(owner)!=1 ||
        dre_gpu_native_owner_io(owner,&next.gpu_io)!=DRE_GPU_OK ||
        dre_rpm_native_binding(rpm,&next.rpm_io,&next.rpm_map,&next.rpm_protocol)!=DRE_GPU_OK)
        return DRE_GPU_NOT_READY;
    if (next.gpu_io.cookie!=(void *)owner || next.rpm_io.cookie!=(void *)rpm ||
        !next.gpu_io.read32 || !next.gpu_io.write32 || !next.gpu_io.read64 || !next.gpu_io.write64 ||
        !next.gpu_io.barrier || !next.gpu_io.now || !next.rpm_io.lease || !next.rpm_io.read32 ||
        !next.rpm_io.write32 || !next.rpm_io.barrier || !next.rpm_io.now ||
        !rpm_profile(&next.rpm_map,&next.rpm_protocol)) return DRE_GPU_UNSUPPORTED;
    next.lease=dre_gpu_native_owner_lease(owner);
    if (!next.lease || next.rpm_io.lease(next.rpm_io.cookie)!=next.lease ||
        dre_gpu_native_owner_guard(owner)!=1 || dre_gpu_native_owner_lease(owner)!=next.lease)
        return DRE_GPU_NOT_READY;
    next.bound=1;
    *r=next;
    return DRE_GPU_OK;
}
void *dre_gpu_io_router_context(void *cookie)
{ struct dre_gpu_io_router *r=cookie; return r && r->bound?r->context:NULL; }
int dre_gpu_io_router_guard(void *cookie)
{ return common(cookie); }
static int enter_access(struct dre_gpu_io_router *r)
{
    if (!r) return 0;
    uint32_t expected=0;
    if (!__atomic_compare_exchange_n(&r->accessing,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return poison(r);
    if (common(r)) return 1;
    __atomic_store_n(&r->accessing,0,__ATOMIC_RELEASE);
    return 0;
}
static int leave_access(struct dre_gpu_io_router *r,int result)
{
    int good=result==1 && common(r);
    __atomic_store_n(&r->accessing,0,__ATOMIC_RELEASE);
    return good?1:poison(r);
}
static int gpu_read32(void *cookie,uint64_t pa,uint32_t *out)
{
    struct dre_gpu_io_router *r=cookie;
    if (!out || !gpu_address(pa,4)) return 0;
    *out=0;
    if (!enter_access(r)) return 0;
    uint32_t value=0;
    int good=leave_access(r,r->gpu_io.read32(r->gpu_io.cookie,pa,&value));
    if (good) *out=value;
    return good;
}
static int count_write(struct dre_gpu_io_router *r)
{
    if (r->attempted_writes==UINT64_MAX) return poison(r);
    ++r->attempted_writes; /* Delegation can publish before a reported failure. */
    return 1;
}
static int gpu_write32(void *cookie,uint64_t pa,uint32_t value)
{
    struct dre_gpu_io_router *r=cookie;
    if (!gpu_address(pa,4) || !enter_access(r)) return 0;
    return leave_access(r,count_write(r)?r->gpu_io.write32(r->gpu_io.cookie,pa,value):0);
}
static int gpu_read64(void *cookie,uint64_t pa,uint64_t *out)
{
    struct dre_gpu_io_router *r=cookie;
    if (!out || !gpu_address(pa,8)) return 0;
    *out=0;
    if (!enter_access(r)) return 0;
    uint64_t value=0;
    int good=leave_access(r,r->gpu_io.read64(r->gpu_io.cookie,pa,&value));
    if (good) *out=value;
    return good;
}
static int gpu_write64(void *cookie,uint64_t pa,uint64_t value)
{
    struct dre_gpu_io_router *r=cookie;
    if (!gpu_address(pa,8) || !enter_access(r)) return 0;
    return leave_access(r,count_write(r)?r->gpu_io.write64(r->gpu_io.cookie,pa,value):0);
}
static int mmio_read32(void *cookie,uint64_t pa,uint32_t *out)
{
    struct dre_gpu_io_router *r=cookie;
    if (gpu_address(pa,4)) return gpu_read32(cookie,pa,out);
    if (!out || !r || !r->bound || !rpm_address(r,pa,0)) return 0;
    *out=0;
    if (!enter_access(r)) return 0;
    uint32_t value=0;
    int good=leave_access(r,r->rpm_io.read32(r->rpm_io.cookie,pa,&value));
    if (good) *out=value;
    return good;
}
static int mmio_write32(void *cookie,uint64_t pa,uint32_t value)
{
    struct dre_gpu_io_router *r=cookie;
    if (gpu_address(pa,4)) return gpu_write32(cookie,pa,value);
    if (!r || !r->bound || !rpm_address(r,pa,1) ||
        (pa==r->rpm_map.ipcc_base+0x0c && value) || !enter_access(r)) return 0;
    return leave_access(r,count_write(r)?r->rpm_io.write32(r->rpm_io.cookie,pa,value):0);
}
static int smmu_read32(void *r,uint64_t pa,uint32_t *out)
{ return smmu_address(pa,4)?gpu_read32(r,pa,out):0; }
static int smmu_write32(void *r,uint64_t pa,uint32_t v)
{ return smmu_address(pa,4)?gpu_write32(r,pa,v):0; }
static int smmu_read64(void *r,uint64_t pa,uint64_t *out)
{ return smmu_address(pa,8)?gpu_read64(r,pa,out):0; }
static int smmu_write64(void *r,uint64_t pa,uint64_t v)
{ return smmu_address(pa,8)?gpu_write64(r,pa,v):0; }
static void barrier(void *cookie)
{
    struct dre_gpu_io_router *r=cookie;
    if (!enter_access(r)) return;
    r->gpu_io.barrier(r->gpu_io.cookie);
    if (!__atomic_load_n(&r->unsafe,__ATOMIC_ACQUIRE)) r->rpm_io.barrier(r->rpm_io.cookie);
    (void)leave_access(r,1);
}
static uint64_t now(void *cookie)
{
    struct dre_gpu_io_router *r=cookie;
    if (!enter_access(r)) return 0;
    uint64_t ticks=r->gpu_io.now(r->gpu_io.cookie);
    return leave_access(r,1)?ticks:0;
}
static uint64_t lease(void *cookie)
{ struct dre_gpu_io_router *r=cookie; return common(r)?r->lease:0; }
static int predicate_enter(struct dre_gpu_io_router *r,uint64_t current)
{
    if (!r || !current || r->lease!=current) return 0;
    uint32_t expected=0;
    if (!__atomic_compare_exchange_n(&r->predicate,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return poison(r);
    if (common(r)) return 1;
    __atomic_store_n(&r->predicate,0,__ATOMIC_RELEASE);
    return 0;
}
int dre_gpu_io_router_cold(void *cookie,uint64_t current)
{
    struct dre_gpu_io_router *r=cookie;
    if (!predicate_enter(r,current)) return 0;
    int good=dre_gpu_native_owner_cold(r->owner,current)==1 && common(r);
    __atomic_store_n(&r->predicate,0,__ATOMIC_RELEASE);
    return good;
}
int dre_gpu_io_router_policy(void *cookie,uint64_t current,const struct dre_rpm_votes *votes,
                             const struct dre_holi_power_config *config)
{
    struct dre_gpu_io_router *r=cookie;
    if (!votes || !config || !predicate_enter(r,current)) return 0;
    int good=dre_gpu_native_owner_policy(r->owner,current,votes,config)==1 && common(r);
    __atomic_store_n(&r->predicate,0,__ATOMIC_RELEASE);
    return good;
}
int dre_gpu_io_router_native_io(struct dre_gpu_io_router *r,struct dre_gpu_native_io *out)
{
    if (!out) return DRE_GPU_INVALID;
    if (!common(r)) return DRE_GPU_NOT_READY;
    *out=(struct dre_gpu_native_io){r,gpu_read32,gpu_write32,gpu_read64,gpu_write64,barrier,now};
    return DRE_GPU_OK;
}
int dre_gpu_io_router_mmio(struct dre_gpu_io_router *r,struct dre_rpm_mmio_ops *out)
{
    if (!out) return DRE_GPU_INVALID;
    if (!common(r)) return DRE_GPU_NOT_READY;
    *out=(struct dre_rpm_mmio_ops){lease,mmio_read32,mmio_write32,barrier,now,r};
    return DRE_GPU_OK;
}
#define SAME_OR_EMPTY(a,b) (!(a) || (a)==(b))
static int compatible_io(const struct dre_a619_ops *o,struct dre_gpu_io_router *r)
{
    return SAME_OR_EMPTY(o->cookie,(void *)r) && SAME_OR_EMPTY(o->guard,dre_gpu_io_router_guard) &&
        SAME_OR_EMPTY(o->read32,gpu_read32) && SAME_OR_EMPTY(o->write32,gpu_write32) &&
        SAME_OR_EMPTY(o->barrier,barrier) && SAME_OR_EMPTY(o->now,now);
}
int dre_gpu_io_router_plan_io(struct dre_gpu_io_router *r,struct dre_a619_start_plan *p)
{
    if (!r || !p) return DRE_GPU_INVALID;
    uintptr_t a=(uintptr_t)r,b=(uintptr_t)p;
    if (a>UINTPTR_MAX-sizeof(*r) || b>UINTPTR_MAX-sizeof(*p) ||
        (a<b+sizeof(*p) && b<a+sizeof(*r))) return DRE_GPU_INVALID;
    if (!common(r)) return DRE_GPU_NOT_READY;
    if (!compatible_io(&p->io,r) || !compatible_io(&p->rbbm_ops.io,r) || !compatible_io(&p->cp_ops.io,r) ||
        !SAME_OR_EMPTY(p->mmio.cookie,(void *)r) || !SAME_OR_EMPTY(p->mmio.lease,lease) ||
        !SAME_OR_EMPTY(p->mmio.read32,mmio_read32) || !SAME_OR_EMPTY(p->mmio.write32,mmio_write32) ||
        !SAME_OR_EMPTY(p->mmio.barrier,barrier) || !SAME_OR_EMPTY(p->mmio.now,now) ||
        !SAME_OR_EMPTY(p->smmu_ops.cookie,(void *)r) || !SAME_OR_EMPTY(p->smmu_ops.read32,smmu_read32) ||
        !SAME_OR_EMPTY(p->smmu_ops.write32,smmu_write32) || !SAME_OR_EMPTY(p->smmu_ops.read64,smmu_read64) ||
        !SAME_OR_EMPTY(p->smmu_ops.write64,smmu_write64) || !SAME_OR_EMPTY(p->smmu_ops.barrier,barrier) ||
        !SAME_OR_EMPTY(p->smmu_ops.now,now) || !SAME_OR_EMPTY(p->cold_owner,dre_gpu_io_router_cold) ||
        !SAME_OR_EMPTY(p->policy_held,dre_gpu_io_router_policy) ||
        (p->rbbm_ops.providers_ready && p->rbbm_ops.io.cookie!=r) ||
        ((p->cp_ops.platform_ready || p->cp_ops.mapping || p->cp_ops.firmware_verified || p->cp_ops.authenticate_zap) &&
            p->cp_ops.io.cookie!=r) ||
        ((p->smmu_ops.guard || p->smmu_ops.owns_ram) && p->smmu_ops.cookie!=r) || p->vm || p->vm_ops)
        return DRE_GPU_UNSUPPORTED;
    struct dre_a619_start_plan next=*p;
    next.mmio=(struct dre_rpm_mmio_ops){lease,mmio_read32,mmio_write32,barrier,now,r};
    next.io=(struct dre_a619_ops){dre_gpu_io_router_guard,gpu_read32,gpu_write32,barrier,now,r};
    next.rbbm_ops.io=next.io; next.cp_ops.io=next.io;
    next.smmu_ops.read32=smmu_read32; next.smmu_ops.write32=smmu_write32;
    next.smmu_ops.read64=smmu_read64; next.smmu_ops.write64=smmu_write64;
    next.smmu_ops.barrier=barrier; next.smmu_ops.now=now; next.smmu_ops.cookie=r;
    next.cold_owner=dre_gpu_io_router_cold; next.policy_held=dre_gpu_io_router_policy;
    /* Deliberately preserve stronger SMMU guard/owns_ram and all actual
     * readiness, allocator, VM, CP memory, firmware/PAS and startup pointers.
     * Missing callbacks remain NULL. No power/ready bit is inferred here. */
    *p=next;
    return DRE_GPU_OK;
}
#undef SAME_OR_EMPTY
