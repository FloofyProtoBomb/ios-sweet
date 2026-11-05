/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Pinned OnePlus98a38783 pinctrl-msm/pinctrl-holi and live Holi
 * topology facts. This is awake-only GPIO22 summary delivery, not MPM wake.
 */
#include "tlmm.h"
#include <libfdt.h>
#include <string.h>
static int words(const void *f,int n,const char *key,const uint32_t *want,unsigned count)
{
    int bytes=0;
    const fdt32_t *p=fdt_getprop(f,n,key,&bytes);
    if (!p || bytes!=(int)count*4) return 0;
    for (unsigned i=0;i<count;i++) if (fdt32_to_cpu(p[i])!=want[i]) return 0;
    return 1;
}
static int interrupt_parent(const void *f,int n)
{
    for (unsigned depth=0;n>=0 && depth<128;depth++,n=fdt_parent_offset(f,n)) {
        int bytes=0;
        const fdt32_t *p=fdt_getprop(f,n,"interrupt-parent",&bytes);
        if (p) return bytes==4?fdt_node_offset_by_phandle(f,fdt32_to_cpu(*p)):-1;
    }
    return -1;
}
enum dre_tn_result dre_tn_tlmm_profile_from_fdt(const void *f,size_t bytes,struct dre_tn_tlmm_profile *p)
{
    if (!f || !p || bytes<sizeof(struct fdt_header) || fdt_check_full(f,bytes) ||
        fdt_totalsize(f)!=bytes) return DRE_TN_PROFILE;
    int t=fdt_path_offset(f,"/soc/pinctrl@400000"),g=interrupt_parent(f,t),n=0;
    const uint32_t tr[]={0x00400000,0x00800000},ti[]={0,227,4},two[]={2},three[]={3};
    const uint32_t gr[]={0x0f200000,0x10000,0x0f240000,0x100000};
    if (fdt_node_check_compatible(f,t,"qcom,holi-pinctrl") ||
        fdt_node_check_compatible(f,g,"arm,gic-v3") ||
        !words(f,t,"reg",tr,2) || !words(f,t,"interrupts",ti,3) ||
        !words(f,t,"#interrupt-cells",two,1) || !words(f,t,"#gpio-cells",two,1) ||
        !words(f,g,"reg",gr,4) || !words(f,g,"#interrupt-cells",three,1)) return DRE_TN_PROFILE;
    const fdt32_t *ref=fdt_getprop(f,t,"wakeup-parent",&n);
    if (!ref || n!=4) return DRE_TN_PROFILE;
    int w=fdt_node_offset_by_phandle(f,fdt32_to_cpu(*ref));
    const uint32_t wr[]={0x045f01b8,0x1000,0x0f40000c,4},wi[]={0,197,1},ninety_six[]={96};
    if (fdt_node_check_compatible(f,w,"qcom,mpm-gic-holi") ||
        fdt_node_check_compatible(f,w,"qcom,mpm") || interrupt_parent(f,w)!=g ||
        !words(f,w,"reg",wr,4) || !words(f,w,"interrupts",wi,3) ||
        !words(f,w,"#interrupt-cells",two,1) || !words(f,w,"qcom,num-mpm-irqs",ninety_six,1))
        return DRE_TN_PROFILE;
    /* No direct/PDC/wake route is inferred or programmed. The native owner
     * must exclude foreign summary producers and hold awake operation. */
    *p=(struct dre_tn_tlmm_profile){
        .base=0x00400000,.bytes=0x00800000,.pin_base=0x00516000,
        .gicd=0x0f200000,.gicr=0x0f240000,.mpm=0x045f01b8,.mpm_bytes=0x1000,.ipc=0x0f40000c,
        .pin=22,.parent_intid=259,.parent_trigger=4,.wake_intid=229,.wake_trigger=1,.wake_pin=21 };
    return DRE_TN_OK;
}
static enum dre_tn_result fail(struct dre_tn_tlmm_pin *p,enum dre_tn_result r)
{ if (p->mutated) __atomic_store_n(&p->poisoned,1,__ATOMIC_RELEASE); return r; }
static int poisoned(const struct dre_tn_tlmm_pin *p)
{ return __atomic_load_n(&p->poisoned,__ATOMIC_ACQUIRE)!=0; }
static int rd(struct dre_tn_tlmm_pin *p,unsigned off,uint32_t *v)
{ return !p->ops.guard_fast(p->ops.cookie) && !p->ops.read32(p->ops.cookie,off,v); }
static int wr(struct dre_tn_tlmm_pin *p,unsigned off,uint32_t v)
{
    if (p->ops.guard_fast(p->ops.cookie)) return 0;
    p->mutated=1; /* An error does not prove a posted write had no effect. */
    if (p->ops.write32(p->ops.cookie,off,v)) return 0;
    p->ops.finish(p->ops.cookie);
    return 1;
}
static int observe(struct dre_tn_tlmm_pin *p)
{
    return rd(p,0,&p->last.control) && rd(p,4,&p->last.input) &&
        rd(p,8,&p->last.config) && rd(p,12,&p->last.status);
}
static int input_gpio(uint32_t ctl)
{ return !(ctl&((15u<<2)|(1u<<9)|(1u<<12))); } /* GPIO mux, input, not EGPIO-owned. */
static int current(struct dre_tn_tlmm_pin *p)
{
    return p && p->active && p->configured && !poisoned(p) && observe(p) &&
        p->last.control==p->original.control && input_gpio(p->last.control) &&
        (p->last.config&~1u)==((p->original.config&~0xffu)|0x7au);
}
enum dre_tn_result dre_tn_tlmm_begin(struct dre_tn_tlmm_pin *p,const struct dre_tn_tlmm_profile *profile,
                                   const struct dre_tn_tlmm_ops *o)
{
    if (!p || p->active || p->mutated || !profile || profile->pin!=22 ||
        profile->pin_base!=0x00516000 || profile->parent_intid!=259 || profile->parent_trigger!=4 ||
        !o || !o->guard_fast || !o->lock_irq || !o->unlock_irq || !o->read32 || !o->write32 || !o->finish)
        return DRE_TN_ARGUMENT;
    *p=(struct dre_tn_tlmm_pin){.profile=*profile,.ops=*o};
    uintptr_t state=o->lock_irq(o->cookie);
    int good=observe(p) && input_gpio(p->last.control) && !(p->last.config&1) && !(p->last.status&1);
    if (good) { p->original=p->last; p->active=1; }
    o->unlock_irq(o->cookie,state);
    return good?DRE_TN_OK:DRE_TN_OWNERSHIP;
}
enum dre_tn_result dre_tn_tlmm_configure(struct dre_tn_tlmm_pin *p)
{
    if (!p || !p->active || p->configured || poisoned(p)) return DRE_TN_STATE;
    uintptr_t state=p->ops.lock_irq(p->ops.cookie);
    uint32_t expected=(p->original.config&~0xffu)|0x7au;
    int good=observe(p) && p->last.control==p->original.control &&
        p->last.config==p->original.config && !(p->last.config&1) && input_gpio(p->last.control) &&
        wr(p,8,expected) && rd(p,8,&p->last.config) && p->last.config==expected &&
        rd(p,12,&p->last.status) && wr(p,12,p->last.status&~1u);
    if (good) p->configured=1;
    else fail(p,DRE_TN_MMIO);
    p->ops.unlock_irq(p->ops.cookie,state);
    return good?DRE_TN_OK:DRE_TN_OWNERSHIP;
}
enum dre_tn_result dre_tn_tlmm_mask(struct dre_tn_tlmm_pin *p,int ack)
{
    if (!p || !p->active || !p->configured || poisoned(p)) return DRE_TN_STATE;
    uintptr_t state=p->ops.lock_irq(p->ops.cookie);
    int good=current(p) && wr(p,8,p->last.config&~1u);
    if (good && ack) good=rd(p,12,&p->last.status) && wr(p,12,p->last.status&~1u);
    if (!good) fail(p,DRE_TN_MMIO);
    p->ops.unlock_irq(p->ops.cookie,state);
    return good?DRE_TN_OK:DRE_TN_OWNERSHIP;
}
static enum dre_tn_result recheck(struct dre_tn_tlmm_pin *p,unsigned *again,int arm)
{
    if (!p || !again || !p->active || !p->configured || poisoned(p)) return DRE_TN_STATE;
    uintptr_t state=p->ops.lock_irq(p->ops.cookie);
    int good=current(p);
    *again=0;
    if (good && arm) good=wr(p,8,p->last.config&~1u) &&
        rd(p,12,&p->last.status) && wr(p,12,p->last.status&~1u) && rd(p,4,&p->last.input);
    if (good && arm && !(p->last.input&1)) *again=1;
    if (good && arm && !*again) good=wr(p,8,p->last.config|1u);
    if (good) good=rd(p,4,&p->last.input) && rd(p,12,&p->last.status);
    if (good && (!(p->last.input&1) || (p->last.status&1))) {
        good=rd(p,8,&p->last.config) && wr(p,8,p->last.config&~1u);
        if (good) *again=1;
    }
    if (!good) fail(p,DRE_TN_MMIO);
    p->ops.unlock_irq(p->ops.cookie,state);
    return good?DRE_TN_OK:DRE_TN_OWNERSHIP;
}
enum dre_tn_result dre_tn_tlmm_arm(struct dre_tn_tlmm_pin *p,unsigned *again)
{ return recheck(p,again,1); }
enum dre_tn_result dre_tn_tlmm_recheck(struct dre_tn_tlmm_pin *p,unsigned *again)
{ return recheck(p,again,0); }
enum dre_tn_result dre_tn_tlmm_sample(struct dre_tn_tlmm_pin *p,unsigned *level)
{
    if (!p || !level || !p->active || !p->configured || poisoned(p)) return DRE_TN_STATE;
    uintptr_t state=p->ops.lock_irq(p->ops.cookie);
    int good=current(p);
    if (good) *level=p->last.input&1;
    else fail(p,DRE_TN_MMIO);
    p->ops.unlock_irq(p->ops.cookie,state);
    return good?DRE_TN_OK:DRE_TN_OWNERSHIP;
}
enum dre_tn_result dre_tn_tlmm_pending(struct dre_tn_tlmm_pin *p,unsigned *pending)
{
    if (!p || !pending || !p->active || !p->configured || poisoned(p)) return DRE_TN_STATE;
    uintptr_t state=p->ops.lock_irq(p->ops.cookie);
    int good=current(p);
    *pending=0;
    if (good && (p->last.config&1) && (p->last.status&1)) {
        good=wr(p,8,p->last.config&~1u);
        if (good) *pending=1;
    }
    if (!good) fail(p,DRE_TN_MMIO);
    p->ops.unlock_irq(p->ops.cookie,state);
    return good?DRE_TN_OK:DRE_TN_OWNERSHIP;
}
enum dre_tn_result dre_tn_tlmm_end(struct dre_tn_tlmm_pin *p)
{
    if (!p) return DRE_TN_ARGUMENT;
    if (!p->active && !p->mutated) return DRE_TN_OK;
    if (poisoned(p)) return DRE_TN_POISONED;
    uintptr_t state=p->ops.lock_irq(p->ops.cookie);
    int good=(p->configured?current(p):observe(p) && p->last.control==p->original.control &&
        p->last.config==p->original.config) && !(p->last.config&1);
    if (good && p->mutated) good=wr(p,8,p->original.config) &&
        rd(p,8,&p->last.config) && p->last.config==p->original.config &&
        rd(p,12,&p->last.status) && wr(p,12,p->last.status&~1u);
    if (good) p->active=p->configured=p->mutated=0;
    else fail(p,DRE_TN_MMIO);
    p->ops.unlock_irq(p->ops.cookie,state);
    return good?DRE_TN_OK:DRE_TN_OWNERSHIP;
}
