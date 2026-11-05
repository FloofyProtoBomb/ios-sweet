/* SPDX-License-Identifier: MIT
 * UNVALIDATED independent SMMUv2/LPAE component for Holi apps QSMMU-v500.
 * Facts: pinned98a38783 arm-smmu.{c,h}, arm-smmu-qcom.c,
 * io-pgtable-arm.c and msm-arm-smmu-holi.dtsi. Not the GPU private SMMU.
 */
#include "smmu.h"
#include <libfdt.h>
#include <string.h>

enum { CR0 = 0, ACR = 0x10, ID0 = 0x20, ID1 = 0x24, ID2 = 0x28,
       GFSR = 0x48, SMR0 = 0x800, S2CR0 = 0xc00, GR1 = 0x1000,
       CBAR0 = 0x1000, CBA2R0 = 0x1800, SCTLR = 0, ACTLR = 4,
       TCR2 = 0x10, TTBR0 = 0x20, TTBR1 = 0x28, TCR = 0x30,
       CONTEXTIDR = 0x34, MAIR0 = 0x38, MAIR1 = 0x3c, FSR = 0x58,
       TLBIASID = 0x610, TLBSYNC = 0x7f0, TLBSTATUS = 0x7f4 };
static const uint32_t saved_offsets[7] = {SCTLR,ACTLR,TCR2,TCR,CONTEXTIDR,MAIR0,MAIR1};
#define S2CR_FAULT UINT32_C(0x20300)
#define CONTROL UINT32_C(0x00c01027) /* NC/NSH, no stall/IRQ, ASIDPNE/CFRE/AFE/TRE/M. */
#define TCR_VALUE UINT32_C(0x00802019) /*39bit IOVA,4KiB,OSH,NC walks,TTBR1 disabled. */
#define TCR2_VALUE UINT32_C(0x00038012) /*40bit PA,upstream SEP,AArch64 ASID. */
#define PA_MASK UINT64_C(0x000000fffffff000)

static int scalar(const void *f, int node, const char *key, uint32_t wanted)
{
    int n; const fdt32_t *p = fdt_getprop(f,node,key,&n);
    return p && n == 4 && fdt32_to_cpu(*p) == wanted;
}
enum dre_tn_result dre_tn_smmu_profile_from_fdt(const void *f, size_t bytes,
                                               struct dre_tn_smmu_profile *out)
{
    if (!f || !out || bytes < 40 || bytes > (2u<<20) || fdt_check_full(f,bytes)) return DRE_TN_ARGUMENT;
    int gpi = fdt_path_offset(f,"/soc/qcom,gpi-dma@4a00000"), n;
    const fdt32_t *p = fdt_getprop(f,gpi,"iommus",&n);
    if (!p || n != 12 || fdt32_to_cpu(p[1]) != 0x16 || p[2]) return DRE_TN_PROFILE;
    int node = fdt_node_offset_by_phandle(f,fdt32_to_cpu(p[0]));
    if (fdt_node_check_compatible(f,node,"qcom,qsmmu-v500") ||
        !scalar(f,node,"#iommu-cells",2) || !scalar(f,node,"#global-interrupts",1) ||
        !fdt_getprop(f,node,"qcom,skip-init",&n) || n ||
        !fdt_getprop(f,node,"qcom,use-3-lvl-tables",&n) || n) return DRE_TN_PROFILE;
    if (fdt_getprop(f,node,"qcom,no-asid-retention",&n)) return DRE_TN_PROFILE;
    p = fdt_getprop(f,node,"reg",&n);
    if (!p || n != 16 || fdt32_to_cpu(p[0]) != 0x0c600000 || fdt32_to_cpu(p[1]) != 0x100000 ||
        fdt32_to_cpu(p[2]) != 0x0c702000 || fdt32_to_cpu(p[3]) != 0x20) return DRE_TN_PROFILE;
    struct dre_tn_smmu_profile profile = { .base=0x0c600000, .bytes=0x100000,
        .tcu=0x0c702000, .tbu=0x0c705000, .sid=0x16, .iova=0x100000, .iova_bytes=0x100000 };
    p = fdt_getprop(f,node,"interrupts",&n);
    if (!p || n != 65*12 || p[0] || fdt32_to_cpu(p[1]) != 81 || fdt32_to_cpu(p[2]) != 4)
        return DRE_TN_PROFILE;
    profile.cb_irqs = 64;
    for (unsigned i=0;i<64;i++) {
        const fdt32_t *irq=p+3*(i+1);
        if (irq[0] || fdt32_to_cpu(irq[1]) != 89+i || fdt32_to_cpu(irq[2]) != 4) return DRE_TN_PROFILE;
        profile.cb_irq[i]=121+i;
    }
    p=fdt_getprop(f,node,"qcom,actlr",&n);
    if (!p || n<=0 || n%12 || n>32*12) return DRE_TN_PROFILE;
    unsigned matches=0;
    for (int i=0;i<n/4;i+=3) {
        uint32_t id=fdt32_to_cpu(p[i]),mask=fdt32_to_cpu(p[i+1]),value=fdt32_to_cpu(p[i+2]);
        if ((profile.sid^id)&~mask) continue;
        if (value!=1) return DRE_TN_PROFILE;
        profile.actlr=value; ++matches;
    }
    if (matches!=1) return DRE_TN_PROFILE;
    unsigned tbus=0;
    int child;
    fdt_for_each_subnode(child,f,node) {
        if (fdt_node_check_compatible(f,child,"qcom,qsmmuv500-tbu")) continue;
        p=fdt_getprop(f,child,"qcom,stream-id-range",&n);
        if (!p || n!=8) return DRE_TN_PROFILE;
        uint32_t first=fdt32_to_cpu(p[0]),count=fdt32_to_cpu(p[1]);
        if (profile.sid<first || profile.sid-first>=count) continue;
        p=fdt_getprop(f,child,"reg",&n);
        if (!p || n!=16 || fdt32_to_cpu(p[0])!=profile.tbu || fdt32_to_cpu(p[1])!=0x1000 ||
            fdt32_to_cpu(p[2])!=0x0c702200 || fdt32_to_cpu(p[3])!=8 ||
            first || count!=0x400) return DRE_TN_PROFILE;
        ++tbus;
    }
    if (tbus!=1) return DRE_TN_PROFILE;
    *out=profile;
    return DRE_TN_OK;
}
static int methods(const struct dre_tn_smmu_ops *o)
{
    return o && o->guard && o->owns_nc && o->owns_page && o->read32 && o->read64 &&
        o->write32 && o->write64 && o->barrier && o->ticks && o->ticks_hz && o->delay_us;
}
static enum dre_tn_result failed(struct dre_tn_smmu *s, enum dre_tn_result result)
{
    if (!s->first_error) s->first_error=result;
    if (s->changed) s->unsafe=1;
    return result;
}
static int guard(struct dre_tn_smmu *s, int quiet)
{
    if (s->ops.guard(s->ops.cookie,&s->owner,quiet)) { failed(s,DRE_TN_OWNERSHIP); return 0; }
    return 1;
}
static int read32(struct dre_tn_smmu *s,uint64_t a,uint32_t *v)
{
    if (!guard(s,0)) return 0;
    if (s->ops.read32(s->ops.cookie,a,v)) { failed(s,DRE_TN_MMIO); return 0; }
    return 1;
}
static int read64(struct dre_tn_smmu *s,uint64_t a,uint64_t *v)
{
    if (!guard(s,0)) return 0;
    if (s->ops.read64(s->ops.cookie,a,v)) { failed(s,DRE_TN_MMIO); return 0; }
    return 1;
}
static int write32(struct dre_tn_smmu *s,uint64_t a,uint32_t v)
{
    if (!guard(s,1)) return 0;
    s->changed=1;
    if (s->ops.write32(s->ops.cookie,a,v)) { failed(s,DRE_TN_MMIO); return 0; }
    s->ops.barrier(s->ops.cookie,DRE_TN_MMIO_FINISH);
    return guard(s,1);
}
static int write64(struct dre_tn_smmu *s,uint64_t a,uint64_t v)
{
    if (!guard(s,1)) return 0;
    s->changed=1;
    if (s->ops.write64(s->ops.cookie,a,v)) { failed(s,DRE_TN_MMIO); return 0; }
    s->ops.barrier(s->ops.cookie,DRE_TN_MMIO_FINISH);
    return guard(s,1);
}
static int equal32(struct dre_tn_smmu *s,uint64_t a,uint32_t wanted)
{
    uint32_t v;
    if (!read32(s,a,&v)) return 0;
    if (v!=wanted) { failed(s,DRE_TN_PROTOCOL); return 0; }
    return 1;
}
static int equal64(struct dre_tn_smmu *s,uint64_t a,uint64_t wanted)
{
    uint64_t v;
    if (!read64(s,a,&v)) return 0;
    if (v!=wanted) { failed(s,DRE_TN_PROTOCOL); return 0; }
    return 1;
}
static int faults(struct dre_tn_smmu *s)
{
    if (!read32(s,s->profile.base+GFSR,&s->last_global_fsr) ||
        !read32(s,s->cb_base+FSR,&s->last_fsr)) return 1;
    if (s->last_global_fsr || s->last_fsr) { failed(s,DRE_TN_PROTOCOL); return 1; }
    return 0; /* Preserve all fault evidence; never clear a peer's fault. */
}
static int sync(struct dre_tn_smmu *s)
{
    s->ops.barrier(s->ops.cookie,DRE_TN_PUBLISH);
    if (!write32(s,s->cb_base+TLBIASID,s->owner.asid) ||
        !write32(s,s->cb_base+TLBSYNC,0)) return 0;
    uint64_t start=s->ops.ticks(s->ops.cookie);
    for (unsigned i=0;i<20000;i++) {
        uint32_t status;
        if (!read32(s,s->cb_base+TLBSTATUS,&status)) return 0;
        if (status&~1u) { failed(s,DRE_TN_PROTOCOL); return 0; }
        if (!(status&1)) {
            s->ops.barrier(s->ops.cookie,DRE_TN_CONSUME);
            return !faults(s) && guard(s,1);
        }
        if (s->ops.ticks(s->ops.cookie)-start>=UINT64_C(1920000)) break;
        s->ops.delay_us(s->ops.cookie,5);
    }
    failed(s,DRE_TN_TIMEOUT); return 0;
}
static uint64_t leaf(uint64_t pa,unsigned permissions)
{
    /* NS,AF,OSH,nG,Normal-NC MAIR0; unpriv+priv RO/RW; both PXN/UXN. */
    return pa|UINT64_C(0x0060000000000e63)|((permissions&DRE_TN_SMMU_WRITE)?0:0x80);
}
static int memory(struct dre_tn_smmu *s)
{
    for (unsigned i=0;i<DRE_TN_SMMU_TABLES;i++) {
        const struct dre_tn_smmu_tables *t=&s->tables;
        if (!t->cpu[i] || ((uintptr_t)t->cpu[i]&4095) ||
            (uintptr_t)t->cpu[i]>UINTPTR_MAX-4096 || !t->pa[i] || (t->pa[i]&4095) ||
            t->pa[i]>=(UINT64_C(1)<<40) ||
            s->ops.owns_nc(s->ops.cookie,(const void *)t->cpu[i],t->pa[i],4096)) return 0;
        for (unsigned j=0;j<i;j++)
            if (t->pa[i]==t->pa[j] ||
                ((uintptr_t)t->cpu[i]<(uintptr_t)t->cpu[j]+4096 &&
                 (uintptr_t)t->cpu[j]<(uintptr_t)t->cpu[i]+4096)) return 0;
    }
    return 1;
}
static int tables(struct dre_tn_smmu *s)
{
    if (!memory(s)) { failed(s,DRE_TN_DMA); return 0; }
    for (unsigned m=0;m<DRE_TN_SMMU_MAPS;m++) {
        const struct dre_tn_smmu_entry *entry=&s->map[m];
        if (!entry->used) continue;
        if (entry->token.slot!=m || !entry->token.generation ||
            entry->token.generation>s->next_generation || entry->token.pages!=4 ||
            entry->token.iova!=s->profile.iova+(uint64_t)m*16384 ||
            !(entry->permissions&DRE_TN_SMMU_READ) || (entry->permissions&~3u)) {
            failed(s,DRE_TN_PROTOCOL); return 0;
        }
        for (unsigned i=0;i<4;i++) {
            uint64_t pa=entry->page[i].pa;
            if (!pa || (pa&4095) || pa>=(UINT64_C(1)<<40) ||
                s->ops.owns_page(s->ops.cookie,pa)) { failed(s,DRE_TN_DMA); return 0; }
            for (unsigned j=0;j<3;j++) if (pa==s->tables.pa[j]) { failed(s,DRE_TN_DMA); return 0; }
            for (unsigned j=0;j<i;j++) if (pa==entry->page[j].pa) { failed(s,DRE_TN_DMA); return 0; }
            for (unsigned other=0;other<m;other++) if (s->map[other].used)
                for (unsigned j=0;j<4;j++) if (pa==s->map[other].page[j].pa) {
                    failed(s,DRE_TN_DMA); return 0;
                }
        }
    }
    s->ops.barrier(s->ops.cookie,DRE_TN_CONSUME);
    for (unsigned i=0;i<512;i++) {
        uint64_t l1=i==0 ? s->tables.pa[1]|UINT64_C(0x8000000000000003) : 0;
        uint64_t l2=i==0 ? s->tables.pa[2]|UINT64_C(0x8000000000000003) : 0;
        uint64_t l3=0;
        for (unsigned m=0;m<DRE_TN_SMMU_MAPS;m++) {
            const struct dre_tn_smmu_entry *entry=&s->map[m];
            unsigned first=(unsigned)(entry->token.iova>>12);
            if (entry->used && i>=first && i-first<entry->token.pages)
                l3=leaf(entry->page[i-first].pa,entry->permissions);
        }
        if (s->tables.cpu[0][i]!=l1 || s->tables.cpu[1][i]!=l2 || s->tables.cpu[2][i]!=l3) {
            failed(s,DRE_TN_PROTOCOL); return 0;
        }
    }
    return 1;
}
static int global(struct dre_tn_smmu *s)
{
    const struct dre_tn_smmu_reservation *r=&s->owner;
    return guard(s,0) && equal32(s,s->profile.base+ID0,r->id0) &&
        equal32(s,s->profile.base+ID1,r->id1) && equal32(s,s->profile.base+ID2,r->id2) &&
        equal32(s,s->profile.base+CR0,r->cr0) && equal32(s,s->profile.base+ACR,r->acr) &&
        equal32(s,s->profile.tcu+0x18,r->tcu_version);
}
static int routes(struct dre_tn_smmu *s,int active)
{
    for (unsigned i=0;i<s->num_smr;i++) {
        uint32_t smr,route;
        if (!read32(s,s->profile.base+SMR0+i*4,&smr) ||
            !read32(s,s->profile.base+S2CR0+i*4,&route)) return 0;
        if (i==s->owner.smr) {
            uint32_t expected_smr=active ? 0x80000016u : s->saved_smr;
            uint32_t expected_route=active ? 0x300u|s->owner.cb : s->saved_s2cr;
            if (smr!=expected_smr || route!=expected_route) { failed(s,DRE_TN_OWNERSHIP); return 0; }
            continue;
        }
        if (!(smr&0x80000000u)) continue;
        uint32_t mask=(smr>>16)&0x7fff,id=smr&0x7fff;
        if (!((id^0x16u)&~mask) || (!(route&0x30000u) && (route&255)==s->owner.cb)) {
            failed(s,DRE_TN_OWNERSHIP); return 0;
        }
    }
    return 1;
}
static int context(struct dre_tn_smmu *s)
{
    uint64_t asid=(uint64_t)s->owner.asid<<48;
    return global(s) && routes(s,1) && !faults(s) &&
        equal32(s,s->profile.base+CBAR0+s->owner.cb*4,0x1f300) &&
        equal32(s,s->profile.base+CBA2R0+s->owner.cb*4,1) &&
        equal32(s,s->cb_base+SCTLR,CONTROL) && equal32(s,s->cb_base+ACTLR,1) &&
        equal32(s,s->cb_base+TCR2,TCR2_VALUE) && equal32(s,s->cb_base+TCR,TCR_VALUE) &&
        equal32(s,s->cb_base+CONTEXTIDR,0) && equal32(s,s->cb_base+MAIR0,0x44) &&
        equal32(s,s->cb_base+MAIR1,0) && equal64(s,s->cb_base+TTBR0,s->tables.pa[0]|asid) &&
        equal64(s,s->cb_base+TTBR1,asid);
}
enum dre_tn_result dre_tn_smmu_begin(struct dre_tn_smmu *s,
    const struct dre_tn_smmu_profile *p,const struct dre_tn_smmu_reservation *r,
    const struct dre_tn_smmu_tables *t,const struct dre_tn_smmu_ops *o)
{
    if (!s || !p || !r || !t || !methods(o)) return DRE_TN_ARGUMENT;
    if (s->active || s->changed || s->unsafe) return DRE_TN_STATE;
    if (p->base!=0x0c600000 || p->bytes!=0x100000 || p->tcu!=0x0c702000 ||
        p->tbu!=0x0c705000 || p->sid!=0x16 || p->iova!=0x100000 || p->iova_bytes!=0x100000 ||
        p->actlr!=1 || p->cb_irqs!=64 || !r->epoch || !r->asid || r->asid>255 ||
        r->cb>=64 || r->fault_irq!=p->cb_irq[r->cb] || o->ticks_hz(o->cookie)!=19200000)
        return DRE_TN_PROFILE;
    *s=(struct dre_tn_smmu){.profile=*p,.owner=*r,.tables=*t,.ops=*o};
    if (!guard(s,1) || !global(s)) return s->first_error;
    uint32_t id0=r->id0,id1=r->id1,id2=r->id2;
    s->num_smr=id0&255; s->num_cb=id1&255;
    unsigned s2=(id1>>16)&255;
    unsigned sid_bits=(id0>>9)&15;
    uint64_t span=UINT64_C(1)<<(((id1>>28)&7)+1+12);
    if (!(id0&(1u<<30)) || !(id0&(1u<<27)) || (r->cr0&9u) || !(r->cr0&(1u<<10)) ||
        (r->acr&(1u<<26)) || !r->tcu_version || r->tcu_version==UINT32_MAX ||
        !sid_bits || 0x16u>=(1u<<sid_bits) || (id1&(1u<<31)) ||
        !s->num_smr || s->num_smr>128 || !s->num_cb || s->num_cb>64 ||
        s2>s->num_cb || r->cb<s2 || r->cb>=s->num_cb || r->smr>=s->num_smr ||
        span>p->bytes/2 || (uint64_t)s->num_cb*4096>span ||
        !(id2&(1u<<12)) || (id2&15)<2 || (id2&15)>5 ||
        ((id2>>4)&15)<2 || ((id2>>4)&15)>5 || ((id2>>8)&15)<2 || ((id2>>8)&15)>5)
        return failed(s,DRE_TN_PROFILE);
    s->cb_base=p->base+span+(uint64_t)r->cb*4096;
    if (!memory(s)) return failed(s,DRE_TN_DMA);
    if (!read32(s,p->base+SMR0+r->smr*4,&s->saved_smr) ||
        !read32(s,p->base+S2CR0+r->smr*4,&s->saved_s2cr)) return s->first_error;
    if (s->saved_smr&0x80000000u) return failed(s,DRE_TN_OWNERSHIP);
    if (!routes(s,0)) return s->first_error;
    for (unsigned i=0;i<7;i++) if (!read32(s,s->cb_base+saved_offsets[i],&s->saved[i])) return s->first_error;
    if (!read32(s,p->base+CBAR0+r->cb*4,&s->saved[7]) ||
        !read32(s,p->base+CBA2R0+r->cb*4,&s->saved[8]) ||
        !read64(s,s->cb_base+TTBR0,&s->saved_ttbr[0]) ||
        !read64(s,s->cb_base+TTBR1,&s->saved_ttbr[1])) return s->first_error;
    if ((s->saved[0]&1) || faults(s) || !equal32(s,s->cb_base+TLBSTATUS,0))
        return failed(s,DRE_TN_OWNERSHIP);
    /* Read-only peer checks supplement, never replace, native ASID/CB/SMR
     * allocation. Secure/disabled context state cannot establish ownership. */
    for (unsigned cb=s2;cb<s->num_cb;cb++) {
        if (cb==r->cb) continue;
        uint64_t base=p->base+span+(uint64_t)cb*4096,ttbr;
        uint32_t control,cba2r;
        if (!read32(s,base,&control)) return s->first_error;
        if (!(control&1)) continue;
        if (!read32(s,p->base+CBA2R0+cb*4,&cba2r)) return s->first_error;
        if (!(cba2r&1)) return failed(s,DRE_TN_PROFILE);
        if (!read64(s,base+TTBR0,&ttbr)) return s->first_error;
        if (((ttbr>>48)&0xffff)==r->asid) return failed(s,DRE_TN_OWNERSHIP);
        if (!read64(s,base+TTBR1,&ttbr)) return s->first_error;
        if (((ttbr>>48)&0xffff)==r->asid) return failed(s,DRE_TN_OWNERSHIP);
    }
    if (!guard(s,1)) return s->first_error;
    for (unsigned l=0;l<3;l++) for (unsigned i=0;i<512;i++) s->tables.cpu[l][i]=0;
    s->tables.cpu[0][0]=s->tables.pa[1]|UINT64_C(0x8000000000000003);
    s->tables.cpu[1][0]=s->tables.pa[2]|UINT64_C(0x8000000000000003);
    s->ops.barrier(s->ops.cookie,DRE_TN_PUBLISH);
    if (!tables(s) || !global(s) || !routes(s,0) || !guard(s,1)) return s->first_error;
    uint64_t asid=(uint64_t)r->asid<<48;
    /* Publish a faulting exact SID before touching a previously disabled CB.
     * There is never an intermediate physical-bypass path. */
    if (!write32(s,p->base+S2CR0+r->smr*4,S2CR_FAULT) ||
        !equal32(s,p->base+S2CR0+r->smr*4,S2CR_FAULT) ||
        !write32(s,p->base+SMR0+r->smr*4,0x80000016) ||
        !equal32(s,p->base+SMR0+r->smr*4,0x80000016) ||
        !write32(s,s->cb_base+SCTLR,0) || !write32(s,p->base+CBA2R0+r->cb*4,1) ||
        !write32(s,p->base+CBAR0+r->cb*4,0x1f300) ||
        !write32(s,s->cb_base+TCR2,TCR2_VALUE) || !write32(s,s->cb_base+TCR,TCR_VALUE) ||
        !write64(s,s->cb_base+TTBR0,s->tables.pa[0]|asid) ||
        !write64(s,s->cb_base+TTBR1,asid) || !write32(s,s->cb_base+CONTEXTIDR,0) ||
        !write32(s,s->cb_base+MAIR0,0x44) || !write32(s,s->cb_base+MAIR1,0) ||
        !write32(s,s->cb_base+ACTLR,1) || !sync(s) ||
        !write32(s,s->cb_base+SCTLR,CONTROL) ||
        !equal32(s,s->cb_base+SCTLR,CONTROL) ||
        !write32(s,p->base+S2CR0+r->smr*4,0x300|r->cb)) return failed(s,s->first_error?s->first_error:DRE_TN_PROTOCOL);
    s->active=1;
    if (!context(s) || !tables(s)) return failed(s,s->first_error?s->first_error:DRE_TN_PROTOCOL);
    return DRE_TN_OK;
}
static struct dre_tn_smmu_entry *token(struct dre_tn_smmu *s,const struct dre_tn_smmu_token *t)
{
    if (!t || t->slot>=DRE_TN_SMMU_MAPS || !t->generation) return NULL;
    struct dre_tn_smmu_entry *m=&s->map[t->slot];
    return m->used && m->token.generation==t->generation && m->token.iova==t->iova &&
        m->token.pages==t->pages ? m : NULL;
}
enum dre_tn_result dre_tn_smmu_map(struct dre_tn_smmu *s,
    const struct dre_tn_smmu_page *pages,unsigned count,unsigned permissions,
    struct dre_tn_smmu_token *out)
{
    if (!s || !out || out->generation || !pages || count!=4 ||
        !(permissions&DRE_TN_SMMU_READ) || (permissions&~3u)) return DRE_TN_ARGUMENT;
    if (!s->active || s->unsafe) return DRE_TN_POISONED;
    if (!context(s) || !tables(s) || !guard(s,1)) return s->first_error;
    unsigned slot=0; while (slot<DRE_TN_SMMU_MAPS && s->map[slot].used) slot++;
    if (slot==DRE_TN_SMMU_MAPS || s->next_generation==UINT64_MAX) return DRE_TN_STATE;
    for (unsigned i=0;i<count;i++) {
        uint64_t pa=pages[i].pa;
        if (!pa || (pa&4095) || pa>=(UINT64_C(1)<<40) || s->ops.owns_page(s->ops.cookie,pa))
            return failed(s,DRE_TN_DMA);
        for (unsigned j=0;j<3;j++) if (pa==s->tables.pa[j]) return failed(s,DRE_TN_DMA);
        for (unsigned j=0;j<i;j++) if (pa==pages[j].pa) return failed(s,DRE_TN_DMA);
        for (unsigned m=0;m<DRE_TN_SMMU_MAPS;m++) if (s->map[m].used)
            for (unsigned j=0;j<s->map[m].token.pages;j++)
                if (pa==s->map[m].page[j].pa) return failed(s,DRE_TN_DMA);
    }
    struct dre_tn_smmu_entry *m=&s->map[slot];
    *m=(struct dre_tn_smmu_entry){.token={++s->next_generation,s->profile.iova+(uint64_t)slot*16384,slot,count},
        .permissions=permissions,.used=1};
    for (unsigned i=0;i<count;i++) m->page[i]=pages[i];
    *out=m->token; /* A failed publication still returns a retained token. */
    unsigned first=(unsigned)(m->token.iova>>12);
    for (unsigned i=0;i<count;i++) s->tables.cpu[2][first+i]=leaf(pages[i].pa,permissions);
    s->ops.barrier(s->ops.cookie,DRE_TN_PUBLISH);
    if (!sync(s) || !context(s) || !tables(s)) return failed(s,s->first_error?s->first_error:DRE_TN_PROTOCOL);
    return DRE_TN_OK;
}
enum dre_tn_result dre_tn_smmu_walk(struct dre_tn_smmu *s,
    const struct dre_tn_smmu_token *t,const struct dre_tn_smmu_page *pages,
    unsigned count,unsigned permissions)
{
    if (!s || !pages || count!=4) return DRE_TN_ARGUMENT;
    if (!s->active || s->unsafe) return DRE_TN_POISONED;
    struct dre_tn_smmu_entry *m=token(s,t);
    if (!m || count!=m->token.pages || permissions!=m->permissions) return DRE_TN_ARGUMENT;
    if (!context(s) || !tables(s)) return s->first_error;
    for (unsigned i=0;i<count;i++)
        if (pages[i].pa!=m->page[i].pa || s->ops.owns_page(s->ops.cookie,pages[i].pa))
            return failed(s,DRE_TN_DMA);
    /* This proves owned PTE/TTBR/control state after completed TLB retirement.
     * It does not claim a physical DMA run or QSMMUv500 ECATS/debug result. */
    return DRE_TN_OK;
}
enum dre_tn_result dre_tn_smmu_unmap(struct dre_tn_smmu *s,const struct dre_tn_smmu_token *t)
{
    if (!s || !t) return DRE_TN_ARGUMENT;
    if (!s->active || s->unsafe) return DRE_TN_POISONED;
    struct dre_tn_smmu_entry *m=token(s,t);
    if (!m) return DRE_TN_ARGUMENT;
    if (!context(s) || !tables(s) || !guard(s,1)) return s->first_error;
    unsigned first=(unsigned)(t->iova>>12);
    for (unsigned i=0;i<t->pages;i++) s->tables.cpu[2][first+i]=0;
    s->ops.barrier(s->ops.cookie,DRE_TN_PUBLISH);
    if (!sync(s) || !context(s)) return failed(s,s->first_error?s->first_error:DRE_TN_PROTOCOL);
    *m=(struct dre_tn_smmu_entry){0};
    if (!tables(s)) return failed(s,s->first_error?s->first_error:DRE_TN_PROTOCOL);
    return DRE_TN_OK;
}
enum dre_tn_result dre_tn_smmu_end(struct dre_tn_smmu *s)
{
    if (!s) return DRE_TN_ARGUMENT;
    if (s->unsafe) return DRE_TN_POISONED;
    if (!s->active) return s->changed ? DRE_TN_POISONED : DRE_TN_OK;
    for (unsigned i=0;i<DRE_TN_SMMU_MAPS;i++) if (s->map[i].used) return DRE_TN_STATE;
    if (!context(s) || !tables(s) || !guard(s,1)) return s->first_error;
    uint64_t base=s->profile.base,cb=s->cb_base;
    unsigned index=s->owner.smr;
    if (!write32(s,base+S2CR0+index*4,S2CR_FAULT) ||
        !equal32(s,base+S2CR0+index*4,S2CR_FAULT) || !write32(s,cb+SCTLR,0) ||
        !equal32(s,cb+SCTLR,0) || !sync(s)) return s->first_error;
    /* Restore only the bank reserved while disabled. TCR precedes TTBR.
     * Restore the original INVALID SMR before its dormant S2CR, keeping an
     * exact faulting route until USFCFG's unmatched-fault policy takes over. */
    /* ACTLR's required invalidate runs while the bank still has this stage1
     * format. Do not issue stage1 TLBIASID after restoring a dormant stage2 or
     * AArch32 format. No table walker can start after this disabled-bank sync. */
    if (!write32(s,cb+ACTLR,s->saved[1]) || !sync(s) ||
        !write32(s,cb+TCR2,s->saved[2]) || !write32(s,cb+TCR,s->saved[3]) ||
        !write64(s,cb+TTBR0,s->saved_ttbr[0]) || !write64(s,cb+TTBR1,s->saved_ttbr[1]) ||
        !write32(s,cb+CONTEXTIDR,s->saved[4]) || !write32(s,cb+MAIR0,s->saved[5]) ||
        !write32(s,cb+MAIR1,s->saved[6]) ||
        !write32(s,base+CBA2R0+s->owner.cb*4,s->saved[8]) ||
        !write32(s,base+CBAR0+s->owner.cb*4,s->saved[7]) ||
        !write32(s,cb+SCTLR,s->saved[0])) return s->first_error;
    for (unsigned i=0;i<7;i++) if (!equal32(s,cb+saved_offsets[i],s->saved[i])) return s->first_error;
    if (!equal64(s,cb+TTBR0,s->saved_ttbr[0]) || !equal64(s,cb+TTBR1,s->saved_ttbr[1]) ||
        !equal32(s,base+CBAR0+s->owner.cb*4,s->saved[7]) ||
        !equal32(s,base+CBA2R0+s->owner.cb*4,s->saved[8]) ||
        !write32(s,base+SMR0+index*4,s->saved_smr) ||
        !equal32(s,base+SMR0+index*4,s->saved_smr) ||
        !write32(s,base+S2CR0+index*4,s->saved_s2cr) ||
        !equal32(s,base+S2CR0+index*4,s->saved_s2cr) ||
        !global(s) || !routes(s,0) || faults(s) || !guard(s,1)) return s->first_error;
    s->active=0; s->changed=0;
    return DRE_TN_OK;
}
