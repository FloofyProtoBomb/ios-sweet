/* SPDX-License-Identifier: MIT
 * ARM SMMUv2/LPAE and Holi implementation protocol/constants from pinned
 * arm-smmu.{c,h}, arm-smmu-qcom.c, io-pgtable-arm.c, msm-arm-smmu-holi.dtsi at
 * LineageOS 98a38783f42f5588bf00cd933e299aedc589bc10. Independent bounded
 * implementation, no copied generic driver or firmware. */
#include "a619_smmu.h"
#define BASE DRE_A619_SMMU_BASE
#define SMR(n) (BASE+0x800+(uint64_t)(n)*4)
#define S2CR(n) (BASE+0xc00+(uint64_t)(n)*4)
#define CBAR(n) (BASE+0x1000+(uint64_t)(n)*4)
#define CBA2R(n) (BASE+0x1800+(uint64_t)(n)*4)
#define TABLE_BYTES 12288u
#define FAULT_STREAM 0x20300u
#define SCTLR_ENABLE 0x00c01027u /* NC/NSH, ASIDPNE, CFRE, AFE/TRE/M; no IRQ/stall */
static const uint32_t offsets[]={0,4,0x10,0x30,0x38,0x3c};
static const uint32_t impl_offsets[]={0x6000,0x6060,0x678c,0x6794,0x6800,0x6900,0x6924,0x6928,0x6930,0x6960,0x6b64,0x6b68};
static const uint32_t impl_values[]={0x2378,0x1055,8,0x28,6,0x3ff,0x204,0x11000,0x800,0xffffffff,0x1a5551,0x9a82a382};
static int immutable_owned(const struct dre_a619_smmu *s);
static int valid(const struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{ return s && o && o->guard && o->owns_ram && o->read32 && o->write32 &&
    o->read64 && o->write64 && o->barrier && o->now; }
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{ return n && m && a<b+m && b<a+n; }
static int cpu_range(const void *p,size_t n)
{ return p && n && (uintptr_t)p<=UINTPTR_MAX-n; }
static int rd(const struct dre_a619_smmu_ops *o,uint64_t a,uint32_t *v)
{ return o->guard(o->cookie) && o->read32(o->cookie,a,v); }
static int rd64(const struct dre_a619_smmu_ops *o,uint64_t a,uint64_t *v)
{ return o->guard(o->cookie) && o->read64(o->cookie,a,v); }
static int wr(const struct dre_a619_smmu_ops *o,uint64_t a,uint32_t v)
{
    if (!o->guard(o->cookie) || !o->write32(o->cookie,a,v)) return 0;
    o->barrier(o->cookie); return o->guard(o->cookie);
}
static int wr64(const struct dre_a619_smmu_ops *o,uint64_t a,uint64_t v)
{
    if (!o->guard(o->cookie) || !o->write64(o->cookie,a,v)) return 0;
    o->barrier(o->cookie); return o->guard(o->cookie);
}
static int check32(const struct dre_a619_smmu_ops *o,uint64_t a,uint32_t expected)
{ uint32_t v; return rd(o,a,&v) && v==expected; }
static int check64(const struct dre_a619_smmu_ops *o,uint64_t a,uint64_t expected)
{ uint64_t v; return o->guard(o->cookie) && o->read64(o->cookie,a,&v) && v==expected; }
static int tlb(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    if (!wr(o,s->cb_base+0x610,s->asid) || !wr(o,s->cb_base+0x7f0,0)) return 0;
    uint64_t start=o->now(o->cookie);
    for (unsigned i=0;i<1000000;i++) {
        uint32_t status;
        if (!rd(o,s->cb_base+0x7f4,&status)) return 0;
        if (!(status&1)) return 1;
        if (o->now(o->cookie)-start>=192000) return 0;
    }
    return 0;
}
static size_t table_bytes(const struct dre_a619_smmu *s)
{
    if (!s->page_count || s->page_count>DRE_A619_SMMU_MAX_PAGES)return 0;
    if (!s->page_capacity && !s->table_capacity_bytes)
        return s->page_count<=512?TABLE_BYTES:0;
    size_t bytes=(2u+(s->page_count+511u)/512u)*4096u;
    return s->page_capacity>=s->page_count && s->page_capacity<=DRE_A619_SMMU_MAX_PAGES &&
        s->table_capacity_bytes==bytes?bytes:0;
}
static const char *memory_check(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    size_t bytes=table_bytes(s);
    uint64_t map_bytes=(uint64_t)s->page_count*4096;
    if (!bytes || !cpu_range(s->tables,bytes) || ((uintptr_t)s->tables&4095) ||
        !s->table_pa || (s->table_pa&4095) || s->table_pa>(UINT64_C(1)<<40)-bytes ||
        !cpu_range(s->pages,(size_t)s->page_count*sizeof(*s->pages)) ||
        (s->va_base&0x1fffff) || s->va_base>=(UINT64_C(1)<<39) ||
        map_bytes>(UINT64_C(1)<<30)-(s->va_base&0x3fffffff) ||
        !s->asid || s->asid>255) return "SMMU_INVALID_MEMORY";
    size_t page_bytes=(size_t)s->page_count*sizeof(*s->pages);
    if (overlap((uintptr_t)s->tables,bytes,(uintptr_t)s,sizeof(*s)) ||
        overlap((uintptr_t)s->tables,bytes,(uintptr_t)o,sizeof(*o)) ||
        overlap((uintptr_t)s->tables,bytes,(uintptr_t)s->pages,page_bytes) ||
        !o->owns_ram(o->cookie,s->table_pa,bytes)) return "SMMU_UNOWNED_MEMORY";
    unsigned mapped=0;
    for (unsigned i=0;i<s->page_count;i++) {
        const struct dre_a619_smmu_page *p=&s->pages[i];
        if (!p->pa && !p->permissions) continue; /* explicit guard hole */
        if (!p->pa || (p->pa&4095) || p->pa>=(UINT64_C(1)<<40) ||
            !(p->permissions&DRE_A619_SMMU_READ) || (p->permissions&~7u) ||
            (p->permissions&6u)==6u ||
            overlap(p->pa,4096,s->table_pa,bytes) ||
            !o->owns_ram(o->cookie,p->pa,4096)) return "SMMU_INVALID_PAGE";
        if (s->page_count<=512)
            for (unsigned j=0;j<i;j++)if (p->pa==s->pages[j].pa)return "SMMU_PHYSICAL_ALIAS";
        mapped++;
    }
    return mapped ? 0 : "SMMU_EMPTY_MAP";
}
static void sift(uint64_t *a,unsigned root,unsigned count)
{
    while (root<count/2) {
        unsigned child=root*2+1;
        if (child+1<count && a[child]<a[child+1])child++;
        if (a[root]>=a[child])break;
        uint64_t tmp=a[root];a[root]=a[child];a[child]=tmp;root=child;
    }
}
static int large_unique(struct dre_a619_smmu *s)
{
    if (s->page_count<=512)return 1;
    /* Exclusively owned, unpublished table pages provide bounded scratch.
     * Validate page ownership/geometry and the disabled CB before this write.
     * Tables are then rebuilt from immutable metadata, not these sorted PAs. */
    unsigned count=0;
    for (unsigned i=0;i<s->page_count;i++)if (s->pages[i].pa)s->tables[count++]=s->pages[i].pa;
    for (unsigned i=count/2;i;i--)sift(s->tables,i-1,count);
    for (unsigned end=count;end>1;end--) {
        uint64_t tmp=s->tables[0];s->tables[0]=s->tables[end-1];s->tables[end-1]=tmp;
        sift(s->tables,0,end-1);
    }
    for (unsigned i=1;i<count;i++)if (s->tables[i-1]==s->tables[i])return 0;
    return 1;
}
static void tables(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    size_t bytes=table_bytes(s);
    for (size_t i=0;i<bytes/8;i++) s->tables[i]=0;
    s->tables[(s->va_base>>30)&511]=(s->table_pa+4096)|UINT64_C(0x8000000000000003);
    for (unsigned i=0;i<(s->page_count+511u)/512u;i++)
        s->tables[512+((s->va_base>>21)&511)+i]=
            (s->table_pa+8192+(uint64_t)i*4096)|UINT64_C(0x8000000000000003);
    for (unsigned i=0;i<s->page_count;i++) {
        const struct dre_a619_smmu_page *p=&s->pages[i];
        if (!p->pa) continue;
        /* NS, AF, outer-shareable, nG, MAIR index0 Normal-NC, user RW/RO. */
        uint64_t descriptor=p->pa|0xe63;
        if (!(p->permissions&DRE_A619_SMMU_WRITE)) descriptor|=0x80;
        if (!(p->permissions&DRE_A619_SMMU_EXECUTE)) descriptor|=UINT64_C(3)<<53;
        s->tables[1024+i]=descriptor;
    }
    o->barrier(o->cookie);
}
static const char *hardware_check(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    uint32_t id0,id1,id2,cr0,v;
    if (!rd(o,BASE+0x20,&id0) || !rd(o,BASE+0x24,&id1) ||
        !rd(o,BASE+0x28,&id2) || !rd(o,BASE,&cr0)) return "SMMU_ID_READ_FAILED";
    unsigned smrs=id0&255,cbs=id1&255,s2=(id1>>16)&255;
    unsigned pages=1u<<(((id1>>28)&7)+1);
    if (!(id0&(1u<<30)) || !(id0&(1u<<27)) || (id0&(1u<<8)) ||
        (id1&(1u<<31)) || pages!=8 || !cbs || cbs>8 || s2>cbs ||
        !smrs || smrs>128 || !(id2&(1u<<12)) ||
        (id2&15)<2 || ((id2>>4)&15)<2 || ((id2>>8)&15)<2 ||
        (id2&15)>5 || ((id2>>4)&15)>5 || ((id2>>8)&15)>5)
        return "SMMU_UNSUPPORTED_CAPABILITIES";
    if ((cr0&9) || !(cr0&(1u<<10))) return "SMMU_GLOBAL_POLICY_REQUIRED";
    if (!rd(o,BASE+0x48,&v))return "SMMU_GLOBAL_STATE_READ_FAILED";
    if (v)return "SMMU_GLOBAL_FAULT"; /* preserve global fault evidence */
    if (s->context_bank>=cbs || s->context_bank<s2 || s->smr_index>=smrs)
        return "SMMU_INVALID_CONTEXT";
    s->cb_base=BASE+0x8000+(uint64_t)s->context_bank*4096;
    for (unsigned i=0;i<12;i++) {
        if (!rd(o,BASE+impl_offsets[i],&v)) return "SMMU_GLOBAL_STATE_READ_FAILED";
        uint32_t mask=i ? UINT32_MAX : ~12u; /* halt/idle are dynamic */
        if ((v&mask)!=(impl_values[i]&mask) || (!i && (v&4))) return "SMMU_GLOBAL_INIT_REQUIRED";
    }
    for (unsigned i=0;i<smrs;i++) {
        uint32_t smr,s2cr;
        if (!rd(o,SMR(i),&smr) || !rd(o,S2CR(i),&s2cr)) return "SMMU_STREAM_READ_FAILED";
        if (i==s->smr_index) { s->saved_smr=smr; s->saved_s2cr=s2cr; }
        if (!(smr&(1u<<31))) continue;
        uint32_t mask=(smr>>16)&0x7fff,sid=smr&0xffff;
        int matches_zero=(sid&~mask)==0;
        if (i==s->smr_index) {
            if (sid || mask) return "SMMU_TARGET_STREAM_NOT_SID0";
        } else {
            if (matches_zero) return "SMMU_SID0_ALREADY_MATCHED";
            if (!(s2cr&0x30000) && (s2cr&255)==s->context_bank) return "SMMU_CONTEXT_SHARED";
        }
    }
    for (unsigned i=0;i<6;i++) if (!rd(o,s->cb_base+offsets[i],&s->saved32[i])) return "SMMU_CONTEXT_READ_FAILED";
    if (!rd(o,CBAR(s->context_bank),&s->saved32[6]) ||
        !rd(o,CBA2R(s->context_bank),&s->saved32[7]) ||
        !rd64(o,s->cb_base+0x20,&s->saved_ttbr[0]) ||
        !rd64(o,s->cb_base+0x28,&s->saved_ttbr[1])) return "SMMU_CONTEXT_READ_FAILED";
    if (s->saved32[0]&1) return "SMMU_CONTEXT_ACTIVE";
    if (!rd(o,s->cb_base+0x58,&v) || v) return "SMMU_CONTEXT_FAULT";
    if (!rd(o,s->cb_base+0x7f4,&v) || (v&1)) return "SMMU_CONTEXT_TLB_BUSY";
    return 0;
}
static int restore(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    /* Establish a faulting stream before disabling translation. Never create
     * an intermediate bypass path to physical memory. */
    if (!wr(o,S2CR(s->smr_index),FAULT_STREAM) ||
        !check32(o,S2CR(s->smr_index),FAULT_STREAM) ||
        !wr(o,s->cb_base,0) || !check32(o,s->cb_base,0) || !tlb(s,o)) return 0;
    if (!wr(o,CBA2R(s->context_bank),s->saved32[7]) ||
        !wr(o,CBAR(s->context_bank),s->saved32[6])) return 0;
    /* Restore TCR before TTBR, matching architectural field dependencies. */
    for (unsigned i=2;i<6;i++) if (!wr(o,s->cb_base+offsets[i],s->saved32[i])) return 0;
    if (!wr64(o,s->cb_base+0x20,s->saved_ttbr[0]) ||
        !wr64(o,s->cb_base+0x28,s->saved_ttbr[1]) ||
        !wr(o,s->cb_base+4,s->saved32[1]) || !tlb(s,o) ||
        !wr(o,s->cb_base,s->saved32[0])) return 0;
    for (unsigned i=0;i<6;i++) if (!check32(o,s->cb_base+offsets[i],s->saved32[i])) return 0;
    if (!check64(o,s->cb_base+0x20,s->saved_ttbr[0]) ||
        !check64(o,s->cb_base+0x28,s->saved_ttbr[1]) ||
        !check32(o,CBAR(s->context_bank),s->saved32[6]) ||
        !check32(o,CBA2R(s->context_bank),s->saved32[7]) ||
        !wr(o,SMR(s->smr_index),s->saved_smr) ||
        !wr(o,S2CR(s->smr_index),s->saved_s2cr) ||
        !check32(o,SMR(s->smr_index),s->saved_smr) ||
        !check32(o,S2CR(s->smr_index),s->saved_s2cr)) return 0;
    s->active=0; s->changed=0; return o->guard(o->cookie);
}
const char *dre_a619_smmu_activate(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    if (!valid(s,o)) return "INVALID_ARGUMENT";
    if (s->unsafe) return "SMMU_RESET_REQUIRED";
    if (s->chip_id!=DRE_A619_CHIP_ID) return "WRONG_GPU";
    if (s->prerequisites!=DRE_A619_SMMU_PREREQUISITES) return "SMMU_PLATFORM_UNAVAILABLE";
    if (s->tick_hz!=19200000) return "UNSUPPORTED_TIMER";
    if (s->active || s->changed) return "SMMU_ALREADY_ACTIVE";
    if (!o->guard(o->cookie)) return "SMMU_OWNERSHIP_REJECTED";
    const char *status=memory_check(s,o);
    if (status) return status;
    status=hardware_check(s,o);
    if (status) return status;
    if (!o->guard(o->cookie))return "SMMU_OWNERSHIP_CHANGED";
    if (!large_unique(s))return "SMMU_PHYSICAL_ALIAS";
    if (!o->guard(o->cookie)) return "SMMU_OWNERSHIP_CHANGED";
    tables(s,o);
    if (!o->guard(o->cookie)) return "SMMU_OWNERSHIP_CHANGED";
    s->owned_context_bank=s->context_bank; s->owned_smr_index=s->smr_index; s->owned_asid=s->asid;
    s->owned_table_pa=s->table_pa;s->owned_tables=s->tables;s->owned_va_base=s->va_base;
    s->owned_pages=s->pages;s->owned_page_count=s->page_count;s->owned_table_bytes=table_bytes(s);
    s->changed=1; /* a failed MMIO write may still reach hardware */
    uint64_t ttbr=s->table_pa|((uint64_t)s->asid<<48);
    int ok=wr(o,S2CR(s->smr_index),FAULT_STREAM) && check32(o,S2CR(s->smr_index),FAULT_STREAM) &&
        wr(o,s->cb_base,0) && wr(o,CBA2R(s->context_bank),1) && wr(o,CBAR(s->context_bank),0x1f300) &&
        wr(o,s->cb_base+0x10,0x38012) && wr(o,s->cb_base+0x30,0x802019) &&
        wr64(o,s->cb_base+0x20,ttbr) && wr64(o,s->cb_base+0x28,0) &&
        wr(o,s->cb_base+0x38,0x44) && wr(o,s->cb_base+0x3c,0) &&
        wr(o,s->cb_base+4,0x70000000) && tlb(s,o) &&
        wr(o,s->cb_base,SCTLR_ENABLE) && check32(o,s->cb_base,SCTLR_ENABLE) &&
        check32(o,s->cb_base+0x58,0) && check32(o,s->cb_base+0x7f4,0) &&
        check64(o,s->cb_base+0x20,ttbr) && check64(o,s->cb_base+0x28,0) &&
        check32(o,s->cb_base+0x10,0x38012) && check32(o,s->cb_base+0x30,0x802019) &&
        check32(o,s->cb_base+0x38,0x44) && check32(o,s->cb_base+0x3c,0) &&
        check32(o,s->cb_base+4,0x70000000) && check32(o,CBAR(s->context_bank),0x1f300) &&
        check32(o,CBA2R(s->context_bank),1) &&
        wr(o,S2CR(s->smr_index),0x300|s->context_bank) &&
        wr(o,SMR(s->smr_index),0x80000000) &&
        check32(o,S2CR(s->smr_index),0x300|s->context_bank) &&
        check32(o,SMR(s->smr_index),0x80000000) && o->guard(o->cookie);
    if (ok) { s->active=1; return "SMMU_SID0_ACTIVE"; }
    if (!restore(s,o)) { s->unsafe=1; return "SMMU_RESET_REQUIRED"; }
    return "SMMU_ACTIVATE_FAILED_RESTORED";
}
const char *dre_a619_smmu_release(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    if (!valid(s,o)) return "INVALID_ARGUMENT";
    if (s->unsafe) return "SMMU_RESET_REQUIRED";
    if (!s->changed) return "SMMU_NO_OWNED_CONTEXT";
    if (s->chip_id!=DRE_A619_CHIP_ID || s->prerequisites!=DRE_A619_SMMU_PREREQUISITES ||
        s->tick_hz!=19200000 || s->context_bank>=8 || s->smr_index>=128 ||
        s->owned_context_bank!=s->context_bank || s->owned_smr_index!=s->smr_index ||
        s->owned_asid!=s->asid ||
        s->cb_base!=BASE+0x8000+(uint64_t)s->context_bank*4096 ||
        !s->asid || s->asid>255 || !immutable_owned(s) || !restore(s,o)) {
        s->unsafe=1; return "SMMU_RESET_REQUIRED";
    }
    return "SMMU_SID0_RESTORED";
}
static int immutable_owned(const struct dre_a619_smmu *s)
{
    return s->chip_id==DRE_A619_CHIP_ID && s->prerequisites==DRE_A619_SMMU_PREREQUISITES &&
        s->tick_hz==19200000 && s->context_bank<8 && s->smr_index<128 &&
        s->owned_context_bank==s->context_bank && s->owned_smr_index==s->smr_index &&
        s->owned_asid==s->asid && s->asid && s->asid<=255 &&
        s->cb_base==BASE+0x8000+(uint64_t)s->context_bank*4096 &&
        s->owned_table_pa==s->table_pa && s->owned_tables==s->tables &&
        s->owned_va_base==s->va_base && s->owned_pages==s->pages &&
        s->owned_page_count==s->page_count && s->owned_table_bytes==table_bytes(s);
}
const char *dre_a619_smmu_check_live(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    if (!valid(s,o))return "INVALID_ARGUMENT";
    if (s->unsafe)return "SMMU_RESET_REQUIRED";
    if (!s->active || !s->changed)return "SMMU_NOT_ACTIVE";
    uint64_t ttbr=s->table_pa|((uint64_t)s->asid<<48);
    uint32_t global;
    int ok=immutable_owned(s) && o->guard(o->cookie) &&
        rd(o,BASE,&global) && !(global&9) && (global&(1u<<10)) && check32(o,BASE+0x48,0) &&
        check32(o,SMR(s->smr_index),0x80000000) &&
        check32(o,S2CR(s->smr_index),0x300|s->context_bank) &&
        check32(o,s->cb_base,SCTLR_ENABLE) &&
        check64(o,s->cb_base+0x20,ttbr) && check64(o,s->cb_base+0x28,0) &&
        check32(o,s->cb_base+0x10,0x38012) && check32(o,s->cb_base+0x30,0x802019) &&
        check32(o,s->cb_base+0x38,0x44) && check32(o,s->cb_base+0x3c,0) &&
        check32(o,s->cb_base+4,0x70000000) &&
        check32(o,CBAR(s->context_bank),0x1f300) && check32(o,CBA2R(s->context_bank),1) &&
        check32(o,s->cb_base+0x58,0) && check32(o,s->cb_base+0x7f4,0) && o->guard(o->cookie);
    if (!ok) { s->unsafe=1;return "SMMU_LIVE_STATE_UNSAFE"; }
    return "SMMU_LIVE_STATE_VERIFIED";
}
const char *dre_a619_smmu_sync_live(struct dre_a619_smmu *s,const struct dre_a619_smmu_ops *o)
{
    const char *status=dre_a619_smmu_check_live(s,o);
    if (!valid(s,o) || !s->active || !s->changed || s->unsafe)return status;
    if (!tlb(s,o)) { s->unsafe=1;return "SMMU_TLB_RETIREMENT_UNSAFE"; }
    return dre_a619_smmu_check_live(s,o);
}
