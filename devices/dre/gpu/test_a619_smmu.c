/* SPDX-License-Identifier: MIT; synthetic SMMUv2 model, no physical accesses. */
#include "a619_smmu.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define BASE DRE_A619_SMMU_BASE
#define CB 0x8000u
static _Alignas(4096) uint64_t table[1536];
struct fixture {
    struct dre_a619_smmu state;
    struct dre_a619_smmu_page pages[5];
    uint32_t regs[16384],saved[16384];
    unsigned writes,reads,barriers,guards,fail_write,ignore_write,lose_write;
    unsigned tlb_count,timeouts;
    uint64_t ticks,fail_read;
    int owner,unowned,clock_stuck;
};
static const uint32_t impl_offsets[]={0x6000,0x6060,0x678c,0x6794,0x6800,0x6900,0x6924,0x6928,0x6930,0x6960,0x6b64,0x6b68};
static const uint32_t impl_values[]={0x2378,0x1055,8,0x28,6,0x3ff,0x204,0x11000,0x800,0xffffffff,0x1a5551,0x9a82a382};
static unsigned register_index(uint64_t address)
{ assert(address>=BASE && address<BASE+0x10000 && !(address&3)); return (unsigned)((address-BASE)/4); }
static int guard(void *cookie)
{ struct fixture *f=cookie; f->guards++; return f->owner; }
static int owns(void *cookie,uint64_t pa,size_t bytes)
{
    struct fixture *f=cookie;
    return !f->unowned && pa>=0x90000000 && pa<0x90100000 && bytes<=0x90100000-pa;
}
static int read32(void *cookie,uint64_t address,uint32_t *value)
{
    struct fixture *f=cookie; f->reads++;
    if (f->fail_read==address) { f->fail_read=0; return 0; }
    *value=f->regs[register_index(address)]; return 1;
}
static int read64(void *cookie,uint64_t address,uint64_t *value)
{
    uint32_t lo,hi;
    if (!read32(cookie,address,&lo) || !read32(cookie,address+4,&hi)) return 0;
    *value=lo|((uint64_t)hi<<32); return 1;
}
static int allowed(unsigned a,int wide)
{
    if (wide) return a==CB+0x20 || a==CB+0x28;
    const unsigned offsets[]={0,4,0x10,0x30,0x38,0x3c,0x610,0x7f0};
    if (a==0xc00 || a==0x800 || a==0x1000 || a==0x1800) return 1;
    for (unsigned i=0;i<sizeof(offsets)/sizeof(*offsets);i++) if (a==CB+offsets[i]) return 1;
    return 0;
}
static int write32(void *cookie,uint64_t address,uint32_t value)
{
    struct fixture *f=cookie; unsigned a=(unsigned)(address-BASE); assert(allowed(a,0));
    f->writes++;
    if (a==CB && !(value&1)) assert((f->regs[0xc00/4]&0x30000)==0x20000);
    if (a==0xc00 && !(value&0x30000)) {
        /* A translating SID is only published with a configured enabled CB. */
        if (f->regs[CB/4]&1) {
            assert(f->regs[(CB+0x30)/4]==0x802019 && f->regs[(CB+0x38)/4]==0x44);
            assert(f->regs[(CB+0x7f4)/4]==0);
        }
    }
    if (f->writes!=f->ignore_write) f->regs[register_index(address)]=value;
    if (a==CB+0x7f0) {
        f->tlb_count++;
        f->regs[(CB+0x7f4)/4]=(f->tlb_count<=f->timeouts);
    }
    if (f->writes==f->lose_write) f->owner=0;
    return f->writes!=f->fail_write;
}
static int write64(void *cookie,uint64_t address,uint64_t value)
{
    struct fixture *f=cookie; assert(allowed((unsigned)(address-BASE),1));
    f->writes++;
    assert(!(f->regs[CB/4]&1) && (f->regs[0xc00/4]&0x30000)==0x20000);
    if (f->writes!=f->ignore_write) {
        unsigned i=register_index(address); f->regs[i]=(uint32_t)value; f->regs[i+1]=(uint32_t)(value>>32);
    }
    if (f->writes==f->lose_write) f->owner=0;
    return f->writes!=f->fail_write;
}
static void barrier(void *cookie) { ((struct fixture *)cookie)->barriers++; }
static uint64_t now(void *cookie)
{ struct fixture *f=cookie; if (!f->clock_stuck) f->ticks+=48000; return f->ticks; }
static struct dre_a619_smmu_ops ops(struct fixture *f)
{ return (struct dre_a619_smmu_ops){guard,owns,read32,write32,read64,write64,barrier,now,f}; }
static const char *activate(struct fixture *f)
{ struct dre_a619_smmu_ops o=ops(f); return dre_a619_smmu_activate(&f->state,&o); }
static const char *release(struct fixture *f)
{ struct dre_a619_smmu_ops o=ops(f); return dre_a619_smmu_release(&f->state,&o); }
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f)); memset(table,0xaa,sizeof(table)); f->owner=1;
    f->state=(struct dre_a619_smmu){.chip_id=DRE_A619_CHIP_ID,
        .prerequisites=DRE_A619_SMMU_PREREQUISITES,.tick_hz=19200000,
        .context_bank=0,.smr_index=0,.asid=1,.va_base=0xfc000000,.table_pa=0x90000000,
        .tables=table,.pages=f->pages,.page_count=5};
    f->pages[0]=(struct dre_a619_smmu_page){0x90010000,DRE_A619_SMMU_READ|DRE_A619_SMMU_WRITE};
    f->pages[1]=(struct dre_a619_smmu_page){0x90011000,DRE_A619_SMMU_READ|DRE_A619_SMMU_EXECUTE};
    f->pages[3]=(struct dre_a619_smmu_page){0x90013000,DRE_A619_SMMU_READ};
    f->pages[4]=(struct dre_a619_smmu_page){0x90014000,DRE_A619_SMMU_READ|DRE_A619_SMMU_WRITE};
    f->regs[0x20/4]=(1u<<30)|(1u<<27)|8;
    f->regs[0x24/4]=(2u<<28)|8; f->regs[0x28/4]=0x1222;
    f->regs[0]=1u<<10;
    for (unsigned i=0;i<12;i++) f->regs[impl_offsets[i]/4]=impl_values[i];
    f->regs[0x808/4]=0x80000002; f->regs[0xc08/4]=0x302; /* secure SID2/CB2 */
    f->regs[0xa000/4]=0xdeadbeef; f->regs[0xa020/4]=0xcafef000;
    f->regs[CB/4]=0x1020; f->regs[(CB+4)/4]=0x1234;
    f->regs[(CB+0x10)/4]=0x12; f->regs[(CB+0x30)/4]=0xabcdef;
    f->regs[(CB+0x38)/4]=0xf4f4ff44; f->regs[(CB+0x3c)/4]=0xe4;
    f->regs[(CB+0x20)/4]=0x90100000; f->regs[(CB+0x24)/4]=0x70000;
    f->regs[0x1000/4]=0x12345; f->regs[0x1800/4]=0x34567;
    memcpy(f->saved,f->regs,sizeof(f->saved));
}
static void restored(const struct fixture *f)
{
    for (unsigned i=0;i<16384;i++) {
        if (i==(CB+0x610)/4 || i==(CB+0x7f0)/4 || i==(CB+0x7f4)/4) continue;
        assert(f->regs[i]==f->saved[i]);
    }
    assert(!f->state.active && !f->state.changed && !f->state.unsafe);
}
/* Independent table walk, decoding descriptor permissions and addresses. */
static uint64_t translate(uint64_t va,int write,int execute)
{
    uint64_t pointer=0x90000000;
    for (unsigned level=1;level<=3;level++) {
        unsigned shift=level==1 ? 30 : level==2 ? 21 : 12;
        assert(pointer>=0x90000000 && pointer<0x90003000);
        uint64_t pte=table[(pointer-0x90000000)/8+((va>>shift)&511)];
        if ((pte&3)!=3) return 0;
        if (level<3) { assert(pte&(UINT64_C(1)<<63)); pointer=pte&UINT64_C(0x000000fffffff000); continue; }
        assert((pte&(1u<<10)) && (pte&(1u<<5)) && ((pte>>8)&3)==2 && ((pte>>2)&7)==0);
        if (write && ((pte>>6)&3)==3) return 0;
        if (execute && ((pte>>53)&3)) return 0;
        return (pte&UINT64_C(0x000000fffffff000))|(va&4095);
    }
    return 0;
}
int main(void)
{
    static struct fixture f; unsigned checks=0;
#define CHECK(s) do { assert(!strcmp(activate(&f),(s))); checks++; } while(0)
#define NOWRITE(s) do { CHECK(s); assert(!f.writes && table[0]==UINT64_C(0xaaaaaaaaaaaaaaaa)); } while(0)
    init(&f); CHECK("SMMU_SID0_ACTIVE"); assert(f.state.active && f.state.changed);
    assert(translate(0xfc000123,1,0)==0x90010123);
    assert(!translate(0xfc000123,0,1)); assert(translate(0xfc001456,0,1)==0x90011456);
    assert(!translate(0xfc001456,1,0)); assert(!translate(0xfc002000,0,0));
    assert(translate(0xfc003fff,0,0)==0x90013fff && !translate(0xfc003fff,1,0));
    assert(!translate(0xfc005000,0,0) && !translate(0xfbffffff,0,0));
    assert(f.regs[0x808/4]==0x80000002 && f.regs[0xc08/4]==0x302 && f.regs[0xa000/4]==0xdeadbeef);
    unsigned activation_writes=f.writes;
    assert(!strcmp(release(&f),"SMMU_SID0_RESTORED")); checks++; restored(&f);
    assert(!strcmp(release(&f),"SMMU_NO_OWNED_CONTEXT")); checks++;
    init(&f); f.state.chip_id=0; NOWRITE("WRONG_GPU");
    for (unsigned i=0;i<6;i++) { init(&f); f.state.prerequisites&=~(1u<<i); NOWRITE("SMMU_PLATFORM_UNAVAILABLE"); }
    init(&f); f.state.tick_hz=1; NOWRITE("UNSUPPORTED_TIMER");
    init(&f); f.owner=0; NOWRITE("SMMU_OWNERSHIP_REJECTED");
    init(&f); f.state.active=1; NOWRITE("SMMU_ALREADY_ACTIVE");
    init(&f); f.state.tables++; NOWRITE("SMMU_INVALID_MEMORY");
    init(&f); f.state.table_pa++; NOWRITE("SMMU_INVALID_MEMORY");
    init(&f); f.state.va_base++; NOWRITE("SMMU_INVALID_MEMORY");
    init(&f); f.state.va_base=UINT64_C(1)<<39; NOWRITE("SMMU_INVALID_MEMORY");
    init(&f); f.state.asid=0; NOWRITE("SMMU_INVALID_MEMORY");
    init(&f); f.state.page_count=513; NOWRITE("SMMU_INVALID_MEMORY");
    init(&f); f.unowned=1; NOWRITE("SMMU_UNOWNED_MEMORY");
    init(&f); f.pages[0].permissions=7; NOWRITE("SMMU_INVALID_PAGE");
    init(&f); f.pages[0].permissions=2; NOWRITE("SMMU_INVALID_PAGE");
    init(&f); f.pages[0].pa++; NOWRITE("SMMU_INVALID_PAGE");
    init(&f); f.pages[0].pa=f.state.table_pa; NOWRITE("SMMU_INVALID_PAGE");
    init(&f); f.pages[1].pa=f.pages[0].pa; NOWRITE("SMMU_PHYSICAL_ALIAS");
    init(&f); memset(f.pages,0,sizeof(f.pages)); NOWRITE("SMMU_EMPTY_MAP");
    init(&f); f.regs[0x20/4]&=~(1u<<30); NOWRITE("SMMU_UNSUPPORTED_CAPABILITIES");
    init(&f); f.regs[0x20/4]|=1u<<8; NOWRITE("SMMU_UNSUPPORTED_CAPABILITIES");
    init(&f); f.regs[0x24/4]|=1u<<31; NOWRITE("SMMU_UNSUPPORTED_CAPABILITIES");
    init(&f); f.regs[0x28/4]=0x1111; NOWRITE("SMMU_UNSUPPORTED_CAPABILITIES");
    init(&f); f.regs[0]=0; NOWRITE("SMMU_GLOBAL_POLICY_REQUIRED");
    init(&f); f.regs[0]|=1; NOWRITE("SMMU_GLOBAL_POLICY_REQUIRED");
    init(&f); f.state.context_bank=8; NOWRITE("SMMU_INVALID_CONTEXT");
    init(&f); f.regs[0x6060/4]++; NOWRITE("SMMU_GLOBAL_INIT_REQUIRED");
    init(&f); f.regs[0x6000/4]|=4; NOWRITE("SMMU_GLOBAL_INIT_REQUIRED");
    init(&f); f.regs[0x800/4]=0x80000002; NOWRITE("SMMU_TARGET_STREAM_NOT_SID0");
    init(&f); f.regs[0x800/4]=0x80008000; NOWRITE("SMMU_TARGET_STREAM_NOT_SID0");
    init(&f); f.regs[0x804/4]=0x80000000; NOWRITE("SMMU_SID0_ALREADY_MATCHED");
    init(&f); f.regs[0x804/4]=0x80020002; NOWRITE("SMMU_SID0_ALREADY_MATCHED");
    init(&f); f.regs[0xc08/4]=0x300; NOWRITE("SMMU_CONTEXT_SHARED");
    init(&f); f.regs[CB/4]|=1; NOWRITE("SMMU_CONTEXT_ACTIVE");
    init(&f); f.regs[(CB+0x58)/4]=2; NOWRITE("SMMU_CONTEXT_FAULT");
    init(&f); f.regs[(CB+0x7f4)/4]=1; NOWRITE("SMMU_CONTEXT_TLB_BUSY");
    const uint64_t failures[]={BASE+0x20,BASE+0x6060,BASE+0x800,BASE+CB,BASE+CB+0x20};
    const char *errors[]={"SMMU_ID_READ_FAILED","SMMU_GLOBAL_STATE_READ_FAILED","SMMU_STREAM_READ_FAILED","SMMU_CONTEXT_READ_FAILED","SMMU_CONTEXT_READ_FAILED"};
    for (unsigned i=0;i<5;i++) { init(&f); f.fail_read=failures[i]; NOWRITE(errors[i]); }
    for (unsigned i=1;i<=activation_writes;i++) {
        init(&f); f.fail_write=i; CHECK("SMMU_ACTIVATE_FAILED_RESTORED"); restored(&f);
    }
    init(&f); f.ignore_write=9; CHECK("SMMU_ACTIVATE_FAILED_RESTORED"); restored(&f);
    init(&f); f.timeouts=1; CHECK("SMMU_ACTIVATE_FAILED_RESTORED"); restored(&f);
    init(&f); f.timeouts=99; CHECK("SMMU_RESET_REQUIRED"); assert(f.state.unsafe && f.state.changed);
    init(&f); f.lose_write=5; CHECK("SMMU_RESET_REQUIRED"); assert(f.writes==5 && f.state.unsafe);
    unsigned writes=f.writes; CHECK("SMMU_RESET_REQUIRED"); assert(f.writes==writes);
    init(&f); f.timeouts=1; f.clock_stuck=1; CHECK("SMMU_ACTIVATE_FAILED_RESTORED"); restored(&f);
    init(&f); CHECK("SMMU_SID0_ACTIVE"); f.state.prerequisites=0; writes=f.writes;
    assert(!strcmp(release(&f),"SMMU_RESET_REQUIRED") && f.writes==writes && f.state.unsafe); checks++;
    init(&f); CHECK("SMMU_SID0_ACTIVE"); f.state.smr_index=2; writes=f.writes;
    assert(!strcmp(release(&f),"SMMU_RESET_REQUIRED") && f.writes==writes && f.state.unsafe); checks++;
    printf("PASS %u SID0 SMMUv2 activation/tablewalk/permission/secure-isolation/rollback checks; synthetic hardware only\n",checks);
    return 0;
}
