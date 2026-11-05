/* SPDX-License-Identifier: MIT -- host-only owned-register models, not native
 * ownership/power/cache evidence. Exercises the actual portable components. */
#include "smmu.h"
#include "qup.h"
#include "tlmm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(e) do { ++checks;if(!(e)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#e);exit(1);} } while(0)
static unsigned checks;
struct mmu_model { uint32_t reg[0x100000/4];uint64_t pa[3],*cpu[3],tick;unsigned quiet,busy,writes; };
static int guard(void *c,const struct dre_tn_smmu_reservation *r,int quiet)
{ struct mmu_model *m=c;CHECK(r->epoch==17);return quiet&&!m->quiet; }
static int nc(void *c,const void *cpu,uint64_t pa,size_t bytes)
{ struct mmu_model *m=c;for(unsigned i=0;i<3;i++)if(cpu==m->cpu[i]&&pa==m->pa[i]&&bytes==4096)return 0;return 1; }
static int page(void *c,uint64_t pa) { (void)c;return pa<0xa0000000||pa>=0xa0100000||(pa&4095); }
static int rd32(void *c,uint64_t a,uint32_t *v)
{
    struct mmu_model *m=c;
    if(a==0xc702018){*v=1;return 0;}
    CHECK(a>=0xc600000&&a<0xc700000&&!(a&3));unsigned off=(unsigned)(a-0xc600000);
    *v=off==0xa7f4&&m->busy?1:m->reg[off/4];return 0;
}
static int rd64(void *c,uint64_t a,uint64_t *v)
{ uint32_t lo,hi;CHECK(!(a&7));rd32(c,a,&lo);rd32(c,a+4,&hi);*v=lo|(uint64_t)hi<<32;return 0; }
static int wr32(void *c,uint64_t a,uint32_t v)
{
    struct mmu_model *m=c;CHECK(a>=0xc600000&&a<0xc700000&&!(a&3));unsigned off=(unsigned)(a-0xc600000);
    CHECK((off>=0xa000&&off<0xb000)||off==0x80c||off==0xc0c||off==0x1008||off==0x1808);
    CHECK(off!=0&&off!=0x10);m->reg[off/4]=v;++m->writes;return 0;
}
static int wr64(void *c,uint64_t a,uint64_t v)
{ CHECK(!(a&7));wr32(c,a,(uint32_t)v);wr32(c,a+4,(uint32_t)(v>>32));return 0; }
static void barrier(void *c,enum dre_tn_barrier b) { (void)c;(void)b; }
static uint64_t ticks(void *c) { return ((struct mmu_model *)c)->tick+=19200; }
static uint64_t hz(void *c) { (void)c;return 19200000; }
static void delay(void *c,unsigned us) { ((struct mmu_model *)c)->tick+=(uint64_t)us*192/10; }
static void mmu_init(struct mmu_model *m,struct dre_tn_smmu *s)
{
    memset(m,0,sizeof(*m));memset(s,0,sizeof(*s));m->quiet=1;
    m->reg[0x20/4]=0x48001010;m->reg[0x24/4]=0x20000008;m->reg[0x28/4]=0x1222;m->reg[0]=0x400;
    m->reg[0x804/4]=0x80000055;m->reg[0xc04/4]=0x300; /* foreign SID preserved */
    struct dre_tn_smmu_profile p={.base=0xc600000,.bytes=0x100000,.tcu=0xc702000,
        .tbu=0xc705000,.iova=0x100000,.iova_bytes=0x100000,.actlr=1,.sid=0x16,.cb_irqs=64};
    for(unsigned i=0;i<64;i++)p.cb_irq[i]=121+i;
    struct dre_tn_smmu_reservation r={.cb=2,.smr=3,.asid=7,.fault_irq=123,
        .id0=m->reg[0x20/4],.id1=m->reg[0x24/4],.id2=m->reg[0x28/4],.cr0=0x400,.tcu_version=1,.epoch=17};
    struct dre_tn_smmu_tables t={0};
    for(unsigned i=0;i<3;i++) {
        m->cpu[i]=aligned_alloc(4096,4096);CHECK(m->cpu[i]);memset(m->cpu[i],0xa5,4096);
        t.cpu[i]=m->cpu[i];t.pa[i]=m->pa[i]=0x90000000+(uint64_t)i*4096;
    }
    struct dre_tn_smmu_ops o={m,guard,nc,page,rd32,rd64,wr32,wr64,barrier,ticks,hz,delay};
    CHECK(dre_tn_smmu_begin(s,&p,&r,&t,&o)==DRE_TN_OK&&s->active);
    CHECK(m->reg[0]==0x400&&m->reg[0x804/4]==0x80000055&&m->reg[0xc04/4]==0x300);
}
static void mmu_free(struct mmu_model *m) { for(unsigned i=0;i<3;i++)free(m->cpu[i]);free(m); }
static void domains(void)
{
    struct mmu_model *m=malloc(sizeof(*m));CHECK(m);struct dre_tn_smmu s;mmu_init(m,&s);
    struct dre_tn_smmu_page p[4]={{0xa0000000},{0xa0001000},{0xa0002000},{0xa0003000}};
    struct dre_tn_smmu_token t={0};CHECK(dre_tn_smmu_map(&s,p,4,1,&t)==DRE_TN_OK);
    CHECK(t.iova==0x100000&&t.pages==4&&t.generation);
    for(unsigned i=0;i<4;i++)CHECK(m->cpu[2][256+i]==(p[i].pa|UINT64_C(0x0060000000000ee3)));
    CHECK(dre_tn_smmu_walk(&s,&t,p,4,1)==DRE_TN_OK);
    CHECK(dre_tn_smmu_walk(&s,&t,p,4,3)==DRE_TN_ARGUMENT);
    CHECK(dre_tn_smmu_end(&s)==DRE_TN_STATE);
    CHECK(dre_tn_smmu_unmap(&s,&t)==DRE_TN_OK);
    for(unsigned i=0;i<4;i++)CHECK(!m->cpu[2][256+i]);
    CHECK(dre_tn_smmu_unmap(&s,&t)==DRE_TN_ARGUMENT);
    CHECK(dre_tn_smmu_end(&s)==DRE_TN_OK&&!s.active);
    CHECK(m->reg[0]==0x400&&!m->reg[0x80c/4]&&!m->reg[0xc0c/4]&&m->reg[0x804/4]==0x80000055);mmu_free(m);
    for(unsigned mode=0;mode<3;mode++) {
        m=malloc(sizeof(*m));CHECK(m);mmu_init(m,&s);t=(struct dre_tn_smmu_token){0};
        if(mode==0)m->busy=1;
        if(mode==1)m->quiet=0;
        if(mode==2)p[1].pa=p[0].pa;
        CHECK(dre_tn_smmu_map(&s,p,4,3,&t)!=DRE_TN_OK);
        CHECK(s.unsafe&&s.active&&dre_tn_smmu_end(&s)==DRE_TN_POISONED);
        if(mode==0)CHECK(t.generation&&s.map[t.slot].used&&m->cpu[2][256]);
        else CHECK(!t.generation);
        mmu_free(m);p[1].pa=0xa0001000;
    }
}
struct pin_model { uint32_t reg[4];unsigned locked,owner,fail_write,edge,writes; };
static int pin_guard(void *c) { return !((struct pin_model *)c)->owner; }
static uintptr_t pin_lock(void *c) { struct pin_model *m=c;CHECK(!m->locked);m->locked=1;return 91; }
static void pin_unlock(void *c,uintptr_t token) { struct pin_model *m=c;CHECK(m->locked&&token==91);m->locked=0; }
static int pin_read(void *c,unsigned off,uint32_t *v)
{ struct pin_model *m=c;CHECK(m->locked&&off<=12&&!(off&3));*v=m->reg[off/4];return 0; }
static int pin_write(void *c,unsigned off,uint32_t v)
{
    struct pin_model *m=c;CHECK(m->locked&&(off==8||off==12));m->reg[off/4]=v;++m->writes;
    if(off==8&&(v&1)&&m->edge){m->reg[1]=0;m->reg[3]|=1;m->edge=0;}
    return m->fail_write?1:0;
}
static void pin_finish(void *c) { CHECK(((struct pin_model *)c)->locked); }
static void pins(void)
{
    struct pin_model m={.reg={0,1,0x400,0},.owner=1};struct dre_tn_tlmm_pin p={0};
    struct dre_tn_tlmm_profile profile={.pin=22,.pin_base=0x516000,.parent_intid=259,.parent_trigger=4};
    struct dre_tn_tlmm_ops o={&m,pin_guard,pin_lock,pin_unlock,pin_read,pin_write,pin_finish};
    CHECK(dre_tn_tlmm_begin(&p,&profile,&o)==DRE_TN_OK&&!m.writes);
    CHECK(dre_tn_tlmm_configure(&p)==DRE_TN_OK&&m.reg[2]==0x47a);
    unsigned again=9,pending=9;CHECK(dre_tn_tlmm_arm(&p,&again)==DRE_TN_OK&&!again&&m.reg[2]==0x47b);
    m.reg[1]=0;CHECK(dre_tn_tlmm_pending(&p,&pending)==DRE_TN_OK&&!pending); /* low != IRQ */
    m.reg[3]=0xa5;CHECK(dre_tn_tlmm_pending(&p,&pending)==DRE_TN_OK&&pending&&!(m.reg[2]&1));
    CHECK(m.reg[3]==0xa5);CHECK(dre_tn_tlmm_mask(&p,1)==DRE_TN_OK&&m.reg[3]==0xa4);
    CHECK(dre_tn_tlmm_arm(&p,&again)==DRE_TN_OK&&again&&!(m.reg[2]&1));
    m.reg[1]=1;m.edge=1;CHECK(dre_tn_tlmm_arm(&p,&again)==DRE_TN_OK&&again&&!(m.reg[2]&1));
    CHECK(dre_tn_tlmm_end(&p)==DRE_TN_OK&&m.reg[2]==0x400&&!p.active);
    m=(struct pin_model){.reg={0,1,0,0},.owner=1};p=(struct dre_tn_tlmm_pin){0};
    CHECK(dre_tn_tlmm_begin(&p,&profile,&o)==DRE_TN_OK);m.fail_write=1;
    CHECK(dre_tn_tlmm_configure(&p)!=DRE_TN_OK&&p.poisoned&&p.active);
    unsigned writes=m.writes;CHECK(dre_tn_tlmm_end(&p)==DRE_TN_POISONED&&m.writes==writes);
}
static void clocks_and_claims(void)
{
    struct dre_tn_qup_snapshot s={.votes=0x4c0,.geni_clock=0x21,.geni_revision=0x100,
        .fifo_disable=1,.wrapper_version=0x20000000,.pin_config={8,8,4,4,0,0}};
    struct dre_tn_qup_clock clock;CHECK(dre_tn_qup_clock_decode(&s,19200000,&clock)==DRE_TN_OK);
    CHECK(clock.serial_hz==9600000&&clock.serial.parent_hz==19200000&&clock.serial.divider==2);
    CHECK(dre_tn_qup_idle_mask(&s)==31);
    struct dre_tn_qup_registry r={0};struct dre_tn_qup_claim a={0},b={0};
    CHECK(dre_tn_qup_claim_idle(&r,1,&s,5,&a)==DRE_TN_OK&&a.gpii==0);
    CHECK(dre_tn_qup_claim_idle(&r,2,&s,5,&b)==DRE_TN_OWNERSHIP);
    CHECK(dre_tn_qup_claim_live(&r,&a)==DRE_TN_OK);
    s.gpi[0][0]=s.gpi[0][1]=3u<<20;s.gpi[0][2]=1u<<20;
    CHECK(dre_tn_qup_release_retired(&r,&a,&s)==DRE_TN_OK);
    CHECK(dre_tn_qup_claim_live(&r,&a)==DRE_TN_OWNERSHIP);
    CHECK(!(dre_tn_qup_idle_mask(&s)&1)); /* stopped/deallocated != new free proof */
    s.rcg_cfg=0x109;s.geni_source=1;s.geni_clock=0xa1;
    s.pll[0]=(struct dre_tn_qup_pll){.mode=0x80000005,.l=31,.opmode=1,.frac=0x4000};
    CHECK(dre_tn_qup_clock_decode(&s,19200000,&clock)==DRE_TN_OK&&clock.serial_hz==12000000);
    s.pll[0].frac=0x10000;CHECK(dre_tn_qup_clock_decode(&s,19200000,&clock)==DRE_TN_PROFILE);
    s.pll[0].frac=0x4000;s.rcg_command=1;CHECK(dre_tn_qup_clock_decode(&s,19200000,&clock)==DRE_TN_PROFILE);
    CHECK(!dre_tn_qup_read_allowed(0x1400001)&&!dre_tn_qup_read_allowed(0x208000));
}
int main(void)
{ domains();pins();clocks_and_claims();printf("PASS %u production SMMU/TLMM/QUP model checks\n",checks);return 0; }
