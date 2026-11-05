/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Independent read-only clock/resource logic from pinned98a38783
 * gcc-holi.c, clk-rcg2.c, clk-alpha-pll.c, pinctrl-holi.c, msm-geni-se.c/gpi.c.
 */
#include "qup.h"
#include <string.h>
#include <libfdt.h>
#define GCC UINT64_C(0x01400000)
#define SPI UINT64_C(0x04a80000)
#define EE UINT64_C(0x049f0000)
static int cells(const void *f,int node,const char *key,uint32_t a,uint32_t b,unsigned count)
{
    int bytes=0;
    const fdt32_t *p=fdt_getprop(f,node,key,&bytes);
    return p && (count==1 || count==2) && bytes==(int)count*4 && fdt32_to_cpu(p[0])==a &&
        (count==1 || fdt32_to_cpu(p[1])==b);
}
enum dre_tn_result dre_tn_qup_profile_from_fdt(const void *f,size_t bytes)
{
    if (!f || bytes<sizeof(struct fdt_header) || fdt_check_full(f,bytes) ||
        fdt_totalsize(f)!=bytes) return DRE_TN_PROFILE;
    int gcc=fdt_path_offset(f,"/soc/clock-controller@1400000");
    int wrapper=fdt_path_offset(f,"/soc/qcom,qupv3_0_geni_se@4ac0000");
    int spi=fdt_path_offset(f,"/soc/spi@4a80000");
    int soc=fdt_path_offset(f,"/soc"),n=0;
    const fdt32_t *p=fdt_getprop(f,soc,"ranges",&n);
    if (!p || n!=16 || p[0] || p[1] || p[2] || fdt32_to_cpu(p[3])<0x04ac2000 ||
        fdt_node_check_compatible(f,gcc,"qcom,holi-gcc") ||
        fdt_node_check_compatible(f,wrapper,"qcom,qupv3-geni-se") ||
        !cells(f,gcc,"reg",0x01400000,0x1f0000,2) ||
        !cells(f,gcc,"#clock-cells",1,0,1) ||
        !cells(f,wrapper,"reg",0x04ac0000,0x2000,2)) return DRE_TN_PROFILE;
    p=fdt_getprop(f,spi,"qcom,wrapper-core",&n);
    if (!p || n!=4 || fdt_node_offset_by_phandle(f,fdt32_to_cpu(*p))!=wrapper)
        return DRE_TN_PROFILE;
    const char names[]="se-clk\0m-ahb\0s-ahb";
    const void *v=fdt_getprop(f,spi,"clock-names",&n);
    if (!v || n!=(int)sizeof(names) || memcmp(v,names,sizeof(names))) return DRE_TN_PROFILE;
    p=fdt_getprop(f,spi,"clocks",&n);
    const uint32_t ids[3]={117,143,144}; /* Exact qcom,gcc-holi.h ABI. */
    if (!p || n!=24) return DRE_TN_PROFILE;
    for (unsigned i=0;i<3;i++)
        if (fdt_node_offset_by_phandle(f,fdt32_to_cpu(p[2*i]))!=gcc ||
            fdt32_to_cpu(p[2*i+1])!=ids[i]) return DRE_TN_PROFILE;
    return DRE_TN_OK;
}
int dre_tn_qup_read_allowed(uint64_t a)
{
    if (a&3) return 0;
    if (a==GCC+0x7900c || a==GCC+0x1f004 || a==GCC+0x1f008 || a==GCC+0x1f144 ||
        (a>=GCC+0x1f148 && a<=GCC+0x1f15c) || a==UINT64_C(0x04ac0004) ||
        a==SPI+0x40 || a==SPI+0x48 || a==SPI+0x64 || a==SPI+0x68 || a==SPI+0x7c ||
        a==SPI+0x610) return 1;
    const unsigned perf[3]={0x1f164,0x1f1a4,0x1f1e4};
    for (unsigned i=0;i<3;i++) if (a>=GCC+perf[i] && a<GCC+perf[i]+32) return 1;
    const unsigned plloff[5]={0,4,0x0c,0x2c,0x38};
    for (unsigned p=0;p<2;p++) for (unsigned i=0;i<5;i++)
        if (a==GCC+(p?0x6000:0)+plloff[i]) return 1;
    const unsigned pins[6]={0,1,2,3,21,22};
    for (unsigned i=0;i<6;i++) if (a==UINT64_C(0x00500000)+pins[i]*0x1000) return 1;
    const unsigned gp[9]={0x20000,0x20080,0x21000,0x23080,0x23090,0x23094,
        0x230b0,0x23100,0x23118};
    for (unsigned g=0;g<5;g++) for (unsigned i=0;i<9;i++)
        if (a==EE+g*0x4000+gp[i]) return 1;
    return 0;
}
static int rd(const struct dre_tn_qup_io *o,uint64_t address,uint32_t *v)
{ return !o->guard(o->cookie) && !o->read32(o->cookie,address,v); }
static int clocks(const struct dre_tn_qup_io *o,struct dre_tn_qup_snapshot *s)
{
    const unsigned branch[3]={0x1f004,0x1f008,0x1f144};
    if (!rd(o,GCC+0x7900c,&s->votes) || !rd(o,GCC+0x1f148,&s->rcg_command) ||
        !rd(o,GCC+0x1f14c,&s->rcg_cfg) || !rd(o,GCC+0x1f150,&s->rcg_m) ||
        !rd(o,GCC+0x1f154,&s->rcg_n) || !rd(o,GCC+0x1f158,&s->rcg_d) ||
        !rd(o,GCC+0x1f15c,&s->dfs) || !rd(o,SPI+0x7c,&s->geni_source) ||
        !rd(o,SPI+0x48,&s->geni_clock) || !rd(o,SPI+0x68,&s->geni_revision) ||
        !rd(o,SPI+0x64,&s->fifo_disable) || !rd(o,UINT64_C(0x04ac0004),&s->wrapper_version)) return 0;
    for (unsigned i=0;i<3;i++) if (!rd(o,GCC+branch[i],&s->branches[i])) return 0;
    for (unsigned level=0;level<8;level++)
        if (!rd(o,GCC+0x1f164+4*level,&s->perf[level][0]) ||
            !rd(o,GCC+0x1f1a4+4*level,&s->perf[level][1]) ||
            !rd(o,GCC+0x1f1e4+4*level,&s->perf[level][2])) return 0;
    for (unsigned i=0;i<2;i++) {
        uint64_t p=GCC+(i?0x6000:0);
        if (!rd(o,p,&s->pll[i].mode) || !rd(o,p+4,&s->pll[i].l) ||
            !rd(o,p+0x0c,&s->pll[i].user) || !rd(o,p+0x2c,&s->pll[i].opmode) ||
            !rd(o,p+0x38,&s->pll[i].frac)) return 0;
    }
    return 1;
}
static int stable_clock(const struct dre_tn_qup_snapshot *a,const struct dre_tn_qup_snapshot *b)
{
    /* Unrelated parent PLL activity and physical GPIO levels are not stability
     * evidence for this selected source. Compare only source-relevant state. */
    if (a->rcg_command!=b->rcg_command || a->rcg_cfg!=b->rcg_cfg || a->rcg_m!=b->rcg_m ||
        a->rcg_n!=b->rcg_n || a->rcg_d!=b->rcg_d || a->dfs!=b->dfs ||
        a->geni_source!=b->geni_source || a->geni_clock!=b->geni_clock ||
        a->geni_revision!=b->geni_revision || a->fifo_disable!=b->fifo_disable ||
        a->wrapper_version!=b->wrapper_version ||
        (a->votes&0x4c0)!=(b->votes&0x4c0)) return 0;
    for (unsigned i=0;i<3;i++) if ((a->branches[i]^b->branches[i])&0xf0000000u) return 0;
    unsigned source=(a->rcg_cfg>>8)&7;
    if (a->dfs&1) {
        if (a->geni_source>7 || memcmp(a->perf[a->geni_source],b->perf[a->geni_source],12)) return 0;
        source=(a->perf[a->geni_source][0]>>8)&7;
    }
    if (source==1 || source==2 || source==4) {
        unsigned pll=source==4;
        if (memcmp(a->pll+pll,b->pll+pll,sizeof(a->pll[pll]))) return 0;
    }
    return 1;
}
enum dre_tn_result dre_tn_qup_observe(const struct dre_tn_qup_io *o,struct dre_tn_qup_snapshot *out)
{
    if (!o || !out || !o->guard || !o->read32 || !o->barrier) return DRE_TN_ARGUMENT;
    struct dre_tn_qup_snapshot a={0},b={0};
    if (!clocks(o,&a)) { *out=a; return DRE_TN_MMIO; }
    o->barrier(o->cookie,DRE_TN_CONSUME);
    if (!clocks(o,&b)) { *out=b; return DRE_TN_MMIO; }
    *out=b;
    if (!stable_clock(&a,&b)) return DRE_TN_OWNERSHIP;
    if (!rd(o,SPI+0x40,&out->geni_status) || !rd(o,SPI+0x610,&out->geni_irq)) return DRE_TN_MMIO;
    const unsigned pins[6]={0,1,2,3,21,22};
    for (unsigned i=0;i<6;i++)
        if (!rd(o,UINT64_C(0x00500000)+pins[i]*0x1000,&out->pin_config[i])) return DRE_TN_MMIO;
    for (unsigned g=0;g<5;g++) {
        uint64_t base=EE+g*0x4000;
        if (!rd(o,base+0x20000,&out->gpi[g][0]) || !rd(o,base+0x20080,&out->gpi[g][1]) ||
            !rd(o,base+0x21000,&out->gpi[g][2]) || !rd(o,base+0x23080,&out->gpi_irq[g]) ||
            !rd(o,base+0x23090,&out->gpi_ch_irq[g]) ||
            !rd(o,base+0x23094,&out->gpi_event_irq[g]) ||
            !rd(o,base+0x230b0,&out->gpi_ieob_irq[g]) ||
            !rd(o,base+0x23100,&out->gpi_global_irq[g]) ||
            !rd(o,base+0x23118,&out->gpi_general_irq[g])) return DRE_TN_MMIO;
    }
    return DRE_TN_OK;
}
static enum dre_tn_result refuse(struct dre_tn_qup_clock *c,enum dre_tn_qup_reason why)
{ c->reason=why; return DRE_TN_PROFILE; }
static int branch_on(uint32_t v)
{ return !(v&0x80000000u) || (v&0xf0000000u)==0x20000000u; }
static int pll(const struct dre_tn_qup_pll *p,uint64_t xo,uint64_t *rate)
{
    /* Fabia L +16bit FRAC, not the generic40bit-alpha register layout. */
    if (!(p->mode&0x80000000u) || !(p->mode&1) || !(p->mode&4) ||
        (p->mode&((1u<<22)|(1u<<21))) || p->opmode!=1 ||
        !p->l || p->l>104 || (p->frac&~0xffffu)) return 0;
    *rate=xo*p->l+((xo*p->frac)>>16);
    return *rate>=249600000 && *rate<=2000000000;
}
enum dre_tn_result dre_tn_qup_clock_decode(const struct dre_tn_qup_snapshot *s,uint64_t xo,
                                          struct dre_tn_qup_clock *c)
{
    if (!s || !c) return DRE_TN_ARGUMENT;
    *c=(struct dre_tn_qup_clock){0};
    if (xo!=19200000) return refuse(c,DRE_TN_QUP_PARENT);
    if ((s->votes&0x4c0)!=0x4c0 || !branch_on(s->branches[0]) ||
        !branch_on(s->branches[1]) || !branch_on(s->branches[2])) return refuse(c,DRE_TN_QUP_BRANCH_OFF);
    if (s->rcg_command&(0x80000000u|0xf1u)) return refuse(c,DRE_TN_QUP_ROOT_OFF);
    if (s->geni_source>7 || !(s->geni_clock&1) || !((s->geni_clock>>4)&4095) ||
        ((s->geni_revision>>8)&255)!=1 || !(s->fifo_disable&1)) return refuse(c,DRE_TN_QUP_GENI);
    uint32_t cfg=s->rcg_cfg,m=s->rcg_m,n=s->rcg_n;
    if (s->dfs&1) {
        c->dfs_level=(s->dfs>>1)&15;
        if (c->dfs_level>7 || c->dfs_level!=s->geni_source) return refuse(c,DRE_TN_QUP_DFS);
        cfg=s->perf[c->dfs_level][0]; m=s->perf[c->dfs_level][1]; n=s->perf[c->dfs_level][2];
    }
    if (cfg&~0x0010371fu) return refuse(c,DRE_TN_QUP_PARENT);
    c->parent_mux=(cfg>>8)&7;
    c->hid=cfg&31;
    uint64_t parent=xo;
    if (c->parent_mux==1 || c->parent_mux==2 || c->parent_mux==4) {
        unsigned which=c->parent_mux==4;
        if (!pll(s->pll+which,xo,&c->pll_hz)) return refuse(c,DRE_TN_QUP_PLL_OFF);
        parent=c->pll_hz;
        if (c->parent_mux!=1) {
            /* The pinned Fabia even-output table supports exactly field1 /2. */
            if (((s->pll[which].user>>8)&15)!=1) return refuse(c,DRE_TN_QUP_PARENT);
            parent/=2;
        }
    } else if (c->parent_mux) return refuse(c,DRE_TN_QUP_PARENT); /* No test clock. */
    c->parent_hz=parent;
    uint64_t rate=c->hid ? parent*2/(c->hid+1) : parent;
    unsigned mode=(cfg>>12)&3;
    if (mode) {
        if (mode!=2 || (m&~0xffffu)) return refuse(c,DRE_TN_QUP_MND);
        c->m=m&0xffffu; c->n=((~n)&0xffffu)+c->m;
        if (!c->m || c->n<=c->m || c->n>65535) return refuse(c,DRE_TN_QUP_MND);
        if (!(s->dfs&1)) {
            uint32_t twice=(c->n-c->m)*2,d=c->n;
            if (d<c->m) d=c->m;
            if (d>twice) d=twice;
            if (((~s->rcg_d)&0xffffu)!=d) return refuse(c,DRE_TN_QUP_MND);
        }
        rate=rate*c->m/c->n;
    }
    if (!rate || rate>128000000) return refuse(c,DRE_TN_QUP_RATE);
    unsigned major=s->wrapper_version>>28,minor=(s->wrapper_version>>16)&4095;
    if (!major || major>3) return refuse(c,DRE_TN_QUP_GENI);
    c->oversampling=major==1 && !minor ? 2 : 1;
    c->serial=(struct dre_tn_clock){s->geni_source,(s->geni_clock>>4)&4095,rate};
    c->serial_hz=rate/c->serial.divider/c->oversampling;
    /* Upper bound matches the unchanged transport's stricter conservative
     * guard; no guessed divider is chosen to make an over-fast source pass. */
    if (rate>(uint64_t)12000000*c->serial.divider || rate<(uint64_t)128000*c->serial.divider)
        return refuse(c,DRE_TN_QUP_RATE);
    return DRE_TN_OK;
}
static int pins(const struct dre_tn_qup_snapshot *s)
{
    const unsigned mux[4]={2,2,1,1};
    for (unsigned i=0;i<4;i++)
        if (((s->pin_config[i]>>2)&15)!=mux[i] || (s->pin_config[i]&(1u<<12))) return 0;
    return !(s->pin_config[4]&(15u<<2)) && !(s->pin_config[5]&((15u<<2)|(1u<<9)|(1u<<12)));
    /* ResetGPIO level/direction is deliberately not a power/panel proof. */
}
uint32_t dre_tn_qup_idle_mask(const struct dre_tn_qup_snapshot *s)
{
    if (!s || !pins(s) || (s->geni_status&0x1001) || s->geni_irq || ((s->geni_revision>>8)&255)!=1 ||
        !(s->fifo_disable&1)) return 0;
    uint32_t free=0;
    for (unsigned g=0;g<5;g++) {
        uint32_t state=(s->gpi[g][0]|s->gpi[g][1]|s->gpi[g][2])&0x00f00000u;
        if (!state && !s->gpi_irq[g] && !s->gpi_ch_irq[g] && !s->gpi_event_irq[g] &&
            !s->gpi_ieob_irq[g] && !s->gpi_global_irq[g] && !s->gpi_general_irq[g]) free|=1u<<g;
    }
    return free;
}
static uint32_t resource_mask(unsigned gpii)
{
    /* SPI0; pins0..3/21/22; GENI INTID359; selected paired GPII/event ring;
     * selected GPI INTID367+gpii. Shared clock/power refs are NOT these bits. */
    return 0xffu|(1u<<(8+gpii))|(1u<<(13+gpii));
}
enum dre_tn_result dre_tn_qup_claim_idle(struct dre_tn_qup_registry *r,uintptr_t owner,
    const struct dre_tn_qup_snapshot *s,unsigned preferred,struct dre_tn_qup_claim *out)
{
    if (!r || !owner || !s || !out || out->generation || preferred>5) return DRE_TN_ARGUMENT;
    if (r->generation==UINT64_MAX) return DRE_TN_STATE;
    uint32_t free=dre_tn_qup_idle_mask(s),used=0;
    unsigned slot=0;
    for (unsigned i=0;i<DRE_TN_QUP_REGISTRY_OWNERS;i++) {
        if (r->owner[i].generation) used|=r->owner[i].resources;
        else slot=i;
    }
    if (r->owner[slot].generation) return DRE_TN_OWNERSHIP;
    unsigned chosen=5;
    for (unsigned g=0;g<5;g++)
        if ((preferred==5 || preferred==g) && (free&(1u<<g)) && !(resource_mask(g)&used)) {
            chosen=g; break;
        }
    if (chosen==5) return DRE_TN_OWNERSHIP;
    *out=(struct dre_tn_qup_claim){++r->generation,owner,resource_mask(chosen),chosen};
    r->owner[slot]=*out;
    return DRE_TN_OK;
}
enum dre_tn_result dre_tn_qup_claim_live(const struct dre_tn_qup_registry *r,
                                        const struct dre_tn_qup_claim *t)
{
    if (!r || !t || !t->owner || !t->generation || t->gpii>4 ||
        t->resources!=resource_mask(t->gpii)) return DRE_TN_ARGUMENT;
    for (unsigned i=0;i<DRE_TN_QUP_REGISTRY_OWNERS;i++) {
        const struct dre_tn_qup_claim *v=r->owner+i;
        if (v->generation==t->generation && v->owner==t->owner &&
            v->resources==t->resources && v->gpii==t->gpii) return DRE_TN_OK;
    }
    return DRE_TN_OWNERSHIP;
}
enum dre_tn_result dre_tn_qup_retirement_check(const struct dre_tn_qup_registry *r,
    const struct dre_tn_qup_claim *t,const struct dre_tn_qup_snapshot *s)
{
    if (!s || dre_tn_qup_claim_live(r,t)) return DRE_TN_OWNERSHIP;
    if (s->geni_status&0x1001 || (s->geni_irq&0x3300003eu) || s->gpi_irq[t->gpii] ||
        s->gpi_ch_irq[t->gpii] || s->gpi_event_irq[t->gpii] || s->gpi_ieob_irq[t->gpii] ||
        s->gpi_global_irq[t->gpii] || s->gpi_general_irq[t->gpii]) return DRE_TN_OWNERSHIP;
    for (unsigned ch=0;ch<2;ch++) {
        unsigned state=(s->gpi[t->gpii][ch]>>20)&15;
        if (state && state!=3) return DRE_TN_OWNERSHIP;
    }
    unsigned event=(s->gpi[t->gpii][2]>>20)&15;
    if (event>1) return DRE_TN_OWNERSHIP;
    return DRE_TN_OK;
}
enum dre_tn_result dre_tn_qup_release_retired(struct dre_tn_qup_registry *r,
    const struct dre_tn_qup_claim *t,const struct dre_tn_qup_snapshot *s)
{
    enum dre_tn_result result=dre_tn_qup_retirement_check(r,t,s);
    if (result) return result;
    for (unsigned i=0;i<DRE_TN_QUP_REGISTRY_OWNERS;i++) if (r->owner[i].generation==t->generation) {
        r->owner[i]=(struct dre_tn_qup_claim){0}; return DRE_TN_OK;
    }
    return DRE_TN_OWNERSHIP;
}
