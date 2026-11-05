/* SPDX-License-Identifier: MIT; independent synthetic static-register model. */
#include "a619_rbbm.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define REG(n) (DRE_A619_GPU_BASE+(uint64_t)(n)*4)
struct fixture {
    struct dre_a619_rbbm state;
    struct dre_a619_rbbm_ops ops;
    uint32_t regs[0x28000],sequence[160],values[160];
    unsigned writes,reads,barriers,write_failure,corrupt_readback,lose_after_write,fault_after_write;
    uint64_t read_failure;
    int owner,providers;
};
static int guard(void *cookie) { return ((struct fixture *)cookie)->owner; }
static int providers(void *cookie) { return ((struct fixture *)cookie)->providers; }
static int read32(void *cookie,uint64_t address,uint32_t *value)
{
    struct fixture *f=cookie;f->reads++;
    assert(address>=DRE_A619_GPU_BASE && address<REG(0x28000) && !(address&3));
    if (address==f->read_failure) { f->read_failure=0;return 0; }
    unsigned reg=(unsigned)((address-DRE_A619_GPU_BASE)/4);*value=f->regs[reg];
    if (f->writes && f->writes==f->corrupt_readback && reg==f->sequence[f->writes-1])*value^=1;
    return 1;
}
static int write32(void *cookie,uint64_t address,uint32_t value)
{
    struct fixture *f=cookie;
    assert(address>=DRE_A619_GPU_BASE && address<REG(0x28000) && !(address&3));
    unsigned reg=(unsigned)((address-DRE_A619_GPU_BASE)/4);
    assert(f->writes<160);
    /* No secure control/buffer, SMMU, rail, GPUCC/GCC, IRQ-enable or WPTR store. */
    assert((reg<0xf000 && reg!=0x38 && reg!=0x807) || reg==0x23b0e ||
        reg==0x1f841 || reg==0x1f840 || reg==0x1a880 || reg==0x27801);
    f->sequence[f->writes]=reg;f->values[f->writes]=value;f->writes++;
    f->regs[reg]=value;
    if (f->writes==f->lose_after_write)f->owner=0;
    if (f->writes==f->fault_after_write)f->regs[0x821]=2;
    return f->writes!=f->write_failure;
}
static void barrier(void *cookie) { ((struct fixture *)cookie)->barriers++; }
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));f->owner=f->providers=1;
    f->state=(struct dre_a619_rbbm){.chip_id=DRE_A619_CHIP_ID,.highest_bank_bit=14,
        .gmem_bytes=524288,.min_access_length=32,.ubwc_mode=2};
    f->ops=(struct dre_a619_rbbm_ops){.io={guard,read32,write32,barrier,0,f},.providers_ready=providers};
    f->regs[0x808]=3;f->regs[0x1f841]=0xaabbcc81;
    f->regs[0x1a880]=0x5a5a0045;f->regs[0xe3a]=0xa5f7fffc;
    f->regs[0xf400]=0xfeed1234;f->regs[0xf810]=0xfeed5678;
    f->regs[0x3c45]=0xf;f->regs[0x18]=3;
}
static const char *run(struct fixture *f) { return dre_a619_rbbm_initialize(&f->state,&f->ops); }
int main(void)
{
    static struct fixture f;unsigned checks=0;
#define CHECK(want) do {const char *got=run(&f);if (strcmp(got,want))fprintf(stderr,"expected %s got %s\n",want,got);assert(!strcmp(got,want));checks++;} while (0)
    init(&f);CHECK("RBBM_INITIALIZED");
    unsigned total=f.writes;assert(total==144 && f.state.active && !f.state.unsafe && f.state.changed);
    assert(f.state.writes==total && f.barriers==total);
    assert(f.regs[0x842]==1 && f.regs[0xe0b]==0x100000 && f.regs[0xe0d]==0x17ffff);
    assert(f.regs[0x8c1]==0x8040362c && f.regs[0x8c2]==0x010000c0 && f.regs[0x8c3]==128);
    assert(f.regs[0x84f]==11 && f.regs[0x850]==0x13fc0000 && f.regs[0x86f]==0x80011c00);
    assert(f.regs[0x8e08]==2 && f.regs[0xb604]==2 && f.regs[0xae02]==2 && f.regs[0xe01]==0x200000);
    assert(f.regs[0x23b0e]==0xff000000 && f.regs[0x1f841]==0xa1 && f.regs[0x1f840]==1);
    assert(f.regs[0x27801]==3 && f.regs[0x1a880]==0x5a5a0045 && f.regs[0xe3a]==0xa5f0fffe);
    assert(f.regs[0xb0]==0x02222222 && f.regs[0x118]==0x222 && f.regs[0xae]==0x8aa8aa82);
    assert(!f.regs[0x3c45] && !f.regs[0x18] && !f.regs[0x38]);
    assert(f.regs[0xf400]==0xfeed1234 && f.regs[0xf810]==0xfeed5678);
    assert(f.sequence[75]==0x1a880 && f.values[75]==0x5a5a0044);
    assert(f.sequence[139]==0xe3a && f.sequence[140]==0x1a880 && f.sequence[141]==0xae);
    CHECK("RESET_REQUIRED");assert(f.writes==total);
    init(&f);f.state.chip_id=0;CHECK("WRONG_GPU");assert(!f.reads && !f.writes);
    for (unsigned field=0;field<4;field++) {
        init(&f);
        if (!field)f.state.highest_bank_bit=15;
        if (field==1)f.state.gmem_bytes=1048576;
        if (field==2)f.state.min_access_length=64;
        if (field==3)f.state.ubwc_mode=3;
        CHECK("UNSUPPORTED_BOARD_PROFILE");assert(!f.writes && !f.reads);
    }
    init(&f);f.providers=0;CHECK("PLATFORM_PROVIDERS_UNAVAILABLE");assert(!f.reads && !f.writes);
    init(&f);f.owner=0;CHECK("PLATFORM_PROVIDERS_UNAVAILABLE");
    init(&f);f.ops.io.read32=0;CHECK("INVALID_ARGUMENT");
    const unsigned idle_regs[]={0x210,0x201,0x821,0x806,0x807,0x808,0x38};
    for (unsigned i=0;i<7;i++) {
        init(&f);f.regs[idle_regs[i]]=i==5?1:2;CHECK("GPU_NOT_COLD_IDLE");assert(!f.writes && !f.state.unsafe);
        init(&f);f.read_failure=REG(idle_regs[i]);CHECK("GPU_NOT_COLD_IDLE");assert(!f.writes);
    }
    /* Every MMIO write failure and every immediately corrupted readback must
     * fail closed without rollback or a later initialization-success claim. */
    for (unsigned n=1;n<=total;n++) {
        init(&f);f.write_failure=n;assert(strcmp(run(&f),"RBBM_INITIALIZED"));checks++;
        assert(f.state.unsafe && !f.state.active && f.writes==n);
        uint64_t last=f.state.last_register;CHECK("RESET_REQUIRED");
        assert(f.writes==n && f.state.last_register==last);
        init(&f);f.corrupt_readback=n;assert(strcmp(run(&f),"RBBM_INITIALIZED"));checks++;
        assert(f.state.unsafe && !f.state.active && f.writes==n);
        init(&f);f.lose_after_write=n;assert(strcmp(run(&f),"RBBM_INITIALIZED"));checks++;
        assert(f.state.unsafe && !f.state.active && f.writes==n);
    }
    init(&f);f.fault_after_write=total;CHECK("FINAL_IDLE_UNVERIFIED");assert(f.state.unsafe && !f.state.active);
    printf("PASS %u static Holi RBBM profile/ordering/permission/uncertain-store cases; synthetic hardware only\n",checks);
}
