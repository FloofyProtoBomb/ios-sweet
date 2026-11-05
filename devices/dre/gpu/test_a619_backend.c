/* SPDX-License-Identifier: MIT
 * Synthetic device test of the production backend. No phone or Metal runtime. */
#include "a619_backend.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
    struct dre_a619_queue q;
    uint32_t ring[64], result[32];
    uint32_t low, high, control, rptr, wptr;
    unsigned guards, guard_fail, reads, read_fail, writes, barriers;
    int write_fail, completion, clock_stuck;
    uint64_t clock;
};
static int guard(void *cookie)
{ struct fixture *f=cookie; return ++f->guards != f->guard_fail; }
static int read32(void *cookie, uint64_t address, uint32_t *value)
{
    struct fixture *f=cookie;
    if (++f->reads == f->read_fail) return 0;
    switch (address) {
    case DRE_A619_RB_BASE: *value=f->low; break;
    case DRE_A619_RB_BASE_HI: *value=f->high; break;
    case DRE_A619_RB_CNTL: *value=f->control; break;
    case DRE_A619_RB_RPTR: *value=f->rptr; break;
    case DRE_A619_RB_WPTR: *value=f->wptr; break;
    default: assert(!"unexpected register read");
    }
    return 1;
}
static int write32(void *cookie, uint64_t address, uint32_t value)
{
    struct fixture *f=cookie;
    assert(address==DRE_A619_RB_WPTR && f->barriers);
    f->writes++;
    unsigned p=f->q.write_pointer;
    /* Decode the emitted stream independently of the production builders. */
    assert(f->ring[p]==0x703d8003);
    assert(f->ring[p+4]==0x70928000); /* opcode0x12 has even popcount, parity1 */
    assert(f->ring[p+5]==0x70268000); /* opcode0x26 has odd popcount, parity0 */
    uint64_t target=(uint64_t)f->ring[p+2]<<32 | f->ring[p+1];
    assert(target>=f->q.result_region.gpu && target+4<=f->q.result_region.gpu+f->q.result_region.bytes);
    unsigned offset=(unsigned)((target-f->q.result_region.gpu)/4);
    uint32_t marker=f->ring[p+3];
    assert(f->result[offset]==(marker^1));
    assert(value==((p+6)&63));
    f->wptr=value;
    if (f->completion==1 || f->completion==2) f->rptr=value;
    if (f->completion==1 || f->completion==3) f->result[offset]=marker;
    return !f->write_fail;
}
static void barrier(void *cookie) { ((struct fixture *)cookie)->barriers++; }
static uint64_t now(void *cookie)
{ struct fixture *f=cookie; if (!f->clock_stuck) f->clock++; return f->clock; }
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));
    for (unsigned i=0;i<64;i++) f->ring[i]=0xaaaaaaaa;
    for (unsigned i=0;i<32;i++) f->result[i]=0x77777777;
    f->q=(struct dre_a619_queue){.chip_id=DRE_A619_CHIP_ID,
        .ready_mask=DRE_A619_READY_MASK,.ring=f->ring,.result=f->result,
        .ring_region={0x200000,256},.result_region={0x400000,128},.ring_dwords=64};
    f->low=0x200000; f->control=5; f->completion=1;
}
static struct dre_a619_ops operations(struct fixture *f)
{ return (struct dre_a619_ops){guard,read32,write32,barrier,now,f}; }
static const char *submit(struct fixture *f)
{
    struct dre_a619_ops ops=operations(f);
    return dre_a619_submit_marker(&f->q,&ops,0x400010,0x44524531,4);
}
static void unchanged(struct fixture *f)
{
    assert(!f->writes && !f->q.submitted && !f->q.completed);
    for (unsigned i=0;i<64;i++) assert(f->ring[i]==0xaaaaaaaa);
    for (unsigned i=0;i<32;i++) assert(f->result[i]==0x77777777);
}
int main(int argc, char **argv)
{
    struct fixture f;
    unsigned checks=0;
#define CHECK_RESULT(label) assert(!strcmp(submit(&f),(label))); checks++
    init(&f); CHECK_RESULT("MARKER_COMPLETED");
    assert(f.q.submitted==1 && f.q.completed==1 && !f.q.poisoned && f.writes==1);
    assert(f.result[4]==0x44524531 && f.result[0]==0x77777777);
    CHECK_RESULT("MARKER_COMPLETED"); assert(f.q.submitted==2 && f.q.completed==2 && f.wptr==12);
    init(&f); f.q.chip_id=0; CHECK_RESULT("WRONG_GPU"); unchanged(&f);
    for (unsigned bit=0;bit<5;bit++) {
        init(&f); f.q.ready_mask&=~(1u<<bit); CHECK_RESULT("INITIALIZATION_INCOMPLETE"); unchanged(&f);
    }
    init(&f); f.q.ready_mask|=32; CHECK_RESULT("INITIALIZATION_INCOMPLETE"); unchanged(&f);
    init(&f); f.q.ring_dwords=63; CHECK_RESULT("INVALID_MAPPING"); unchanged(&f);
    init(&f); f.q.ring_region.gpu++; CHECK_RESULT("INVALID_MAPPING"); unchanged(&f);
    init(&f); f.q.result_region.gpu=0x200000; CHECK_RESULT("INVALID_MAPPING"); unchanged(&f);
    init(&f); f.q.result_region.gpu=UINT64_MAX-3; CHECK_RESULT("INVALID_MAPPING"); unchanged(&f);
    init(&f); f.low=0x300000; CHECK_RESULT("RING_MAPPING_MISMATCH"); unchanged(&f);
    init(&f); f.control=6; CHECK_RESULT("RING_SIZE_MISMATCH"); unchanged(&f);
    init(&f); f.control=63; CHECK_RESULT("RING_SIZE_MISMATCH"); unchanged(&f);
    init(&f); f.wptr=1; CHECK_RESULT("RING_BUSY"); unchanged(&f);
    init(&f); f.read_fail=1; CHECK_RESULT("REGISTER_READ_FAILED"); unchanged(&f);
    init(&f); f.guard_fail=1; CHECK_RESULT("OWNERSHIP_REJECTED"); unchanged(&f);
    init(&f); f.guard_fail=2; CHECK_RESULT("OWNERSHIP_CHANGED"); unchanged(&f);
    init(&f); f.q.write_pointer=f.rptr=f.wptr=60; CHECK_RESULT("RING_NEEDS_REWIND"); unchanged(&f);
    init(&f); f.q.submitted=UINT64_MAX; CHECK_RESULT("INVALID_COUNTER_STATE");
    init(&f); f.q.completed=1; CHECK_RESULT("INVALID_COUNTER_STATE");
    init(&f); f.guard_fail=3; CHECK_RESULT("OWNERSHIP_CHANGED"); assert(f.q.poisoned && !f.writes);
    init(&f); f.write_fail=1; CHECK_RESULT("PUBLISH_FAILED"); assert(f.q.poisoned && !f.q.completed);
    init(&f); f.guard_fail=4; CHECK_RESULT("OWNERSHIP_LOST"); assert(f.q.poisoned && !f.q.completed);
    init(&f); f.read_fail=6; CHECK_RESULT("COMPLETION_READ_FAILED"); assert(f.q.poisoned && !f.q.completed);
    for (unsigned mode=0;mode<4;mode+=2) {
        init(&f); f.completion=mode; CHECK_RESULT("COMPLETION_TIMEOUT");
        assert(f.q.poisoned && f.q.submitted==1 && !f.q.completed);
        unsigned writes=f.writes;
        CHECK_RESULT("RESET_REQUIRED"); assert(f.writes==writes);
    }
    init(&f); f.completion=3; CHECK_RESULT("COMPLETION_TIMEOUT"); assert(f.q.poisoned && !f.q.completed);
    init(&f); f.completion=0; f.clock_stuck=1; CHECK_RESULT("CLOCK_STALLED"); assert(f.q.poisoned);
    for (unsigned op=0;op<128;op++) {
        for (unsigned count=0;count<128;count++) {
            uint32_t packet=dre_a619_type7(op,count);
            assert((packet>>28)==7 && ((packet>>16)&127)==op && (packet&0x3fff)==count);
            assert((__builtin_popcount(op)+((packet>>23)&1))%2==1);
            assert((__builtin_popcount(count)+((packet>>15)&1))%2==1);
        }
    }
    assert(!dre_a619_type7(128,0) && !dre_a619_type7(0,16384)); checks++;
    uint8_t fw[20]={0}; fw[4]=0xab; fw[5]=0xcd;
    struct dre_a619_sqe view;
    assert(!strcmp(dre_a619_sqe_view(DRE_A619_CHIP_ID,fw,sizeof(fw),&view),"SQE_VIEW_READY"));
    assert(view.code==fw+4 && view.bytes==16 && view.version==0xcdab); checks++;
    fw[4]=0x0a; fw[5]=0x80; fw[13]=0x50;
    assert(!strcmp(dre_a619_sqe_view(DRE_A619_CHIP_ID,fw,sizeof(fw),&view),"SQE_VIEW_READY"));
    assert(view.version==0x8005); checks++;
    for (unsigned size=0;size<20;size++) {
        if (size==16) continue;
        assert(!strcmp(dre_a619_sqe_view(DRE_A619_CHIP_ID,fw,size,&view),"INVALID_FIRMWARE"));
        assert(!view.code && !view.bytes); checks++;
    }
    assert(!strcmp(dre_a619_sqe_view(0,fw,sizeof(fw),&view),"WRONG_GPU")); checks++;
    printf("PASS %u A619 queue/firmware cases and 16384 independent PM4 parity checks; synthetic device only\n",checks);
    if (argc==2) {
        FILE *input=fopen(argv[1],"rb"); assert(input);
        assert(!fseek(input,0,SEEK_END)); long size=ftell(input);
        assert(size>=16 && size<=1048576); rewind(input);
        uint8_t *data=malloc((size_t)size); assert(data);
        assert(fread(data,1,(size_t)size,input)==(size_t)size); fclose(input);
        assert(!strcmp(dre_a619_sqe_view(DRE_A619_CHIP_ID,data,(size_t)size,&view),"SQE_VIEW_READY"));
        assert(view.code==data+4 && view.bytes==(size_t)size-4);
        printf("PASS private SQE layout: file=%ld code=%zu version=%08x; no hardware access\n",size,view.bytes,view.version);
        free(data);
    }
}
