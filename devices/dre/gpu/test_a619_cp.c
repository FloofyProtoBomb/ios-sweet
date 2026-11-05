/* SPDX-License-Identifier: MIT; independent synthetic CP/MMIO model only. */
#include "a619_cp.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define IDX(a) ((unsigned)(((a)-DRE_A619_GPU_BASE)/4))
struct fixture {
    struct dre_a619_cp state;
    struct dre_a619_queue queue;
    struct dre_a619_cp_ops ops;
    uint32_t regs[65536];
    _Alignas(4096) uint32_t ring[1024],sqe[8192],scratch[1024],powerup[1024],result[1024];
    uint32_t firmware[16];
    uint64_t ticks,read_failure;
    unsigned writes,reads,mappings,auth_calls,barriers,guards,publishes;
    unsigned write_failure,lose_on_write,bad_readback,timeout_stage,bad_shadow_stage,fault_stage,irq_stage;
    int owner,platform,verified,mapping_failure,zap_failure,secure_failure,clock_stuck;
};
static int guard(void *cookie)
{ struct fixture *f=cookie;f->guards++;return f->owner; }
static int platform(void *cookie)
{ return ((struct fixture *)cookie)->platform; }
static int read32(void *cookie,uint64_t a,uint32_t *v)
{
    struct fixture *f=cookie;f->reads++;
    assert(a>=DRE_A619_GPU_BASE && a<DRE_A619_GPU_BASE+0x40000 && !(a&3));
    if (a==f->read_failure) { f->read_failure=0;return 0; }
    *v=f->regs[IDX(a)];
    if (f->bad_readback==IDX(a) && f->writes)*v^=1;
    return 1;
}
static uint64_t gpu_address(const volatile uint32_t *p)
{ return (uint64_t)p[1]<<32|p[0]; }
static void consume(struct fixture *f,uint32_t wptr)
{
    f->publishes++;
    unsigned stage=f->publishes;
    if (stage==1) {
        assert(wptr==15 && f->ring[0]==0x70c8000b && f->ring[1]==0x16f);
        assert(f->ring[2]==3 && f->ring[3]==0x20000000 && !f->ring[4] && !f->ring[5]);
        assert(!f->ring[6] && f->ring[7]==2 && gpu_address(f->ring+8)==f->state.powerup.region.gpu);
        assert(!f->ring[10] && !f->ring[11]);
        assert(f->ring[12]==0x70620002 && gpu_address(f->ring+13)==f->state.scratch.region.gpu);
        assert(!memcmp(f->sqe,(const uint8_t *)f->state.firmware+4,f->state.firmware_bytes-4));
        for (size_t i=(f->state.firmware_bytes-4)/4;i<f->state.sqe.region.bytes/4;i++)assert(!f->sqe[i]);
        assert(f->powerup[0]==0 && f->powerup[1]==0 && f->powerup[2]==0);
        assert(f->powerup[3]==(136u|(72u<<16)));
        assert(f->powerup[4]==0x841 && f->powerup[4+35*2]==0x98d);
        assert(f->powerup[4+36*2]==0xc01 && f->powerup[4+65*2]==0xe3a);
        assert(f->powerup[4+66*2]==0x11 && f->powerup[4+67*2]==0x500 && f->powerup[5+67*2]==1);
        for (unsigned i=0;i<67;i++)assert(f->powerup[5+i*2]==f->regs[f->powerup[4+i*2]]);
        assert(gpu_address(f->regs+0x800)==f->queue.ring_region.gpu);
        assert(gpu_address(f->regs+0x804)==f->state.scratch.region.gpu);
        assert(gpu_address(f->regs+0x830)==f->state.sqe.region.gpu);
    } else if (stage==2) {
        assert(f->auth_calls==1 && !f->zap_failure && wptr==20);
        assert(f->ring[15]==0x70e60001 && !f->ring[16] && f->ring[17]==0x70620002);
        assert(gpu_address(f->ring+18)==f->state.scratch.region.gpu);
        if (!f->secure_failure)f->regs[0xf400]=0;
    } else {
        assert(stage==3 && wptr==26 && !f->regs[0xf400]);
        assert(f->ring[20]==0x703d8003 && gpu_address(f->ring+21)==f->queue.result_region.gpu);
        assert(f->ring[23]==0x619c001d);
        assert(f->ring[24]==0x70928000 && f->ring[25]==0x70268000);
        if (f->timeout_stage!=stage)f->result[0]=f->ring[23];
    }
    if (f->timeout_stage!=stage) {
        f->regs[0x806]=wptr;
        if (f->bad_shadow_stage!=stage)f->scratch[0]=wptr;
    }
    if (f->fault_stage==stage)f->regs[0x821]=2;
    if (f->irq_stage==stage)f->regs[0x201]=0x40;
}
static int write32(void *cookie,uint64_t a,uint32_t v)
{
    struct fixture *f=cookie;f->writes++;
    unsigned r=IDX(a);
    assert(a>=DRE_A619_GPU_BASE && a<DRE_A619_GPU_BASE+0x40000 && !(a&3));
    assert(r==0x804 || r==0x805 || r==0x802 || r==0x800 || r==0x801 ||
        r==0x830 || r==0x831 || r==0x808 || r==0x807);
    f->regs[r]=v;
    if (r==0x807)consume(f,v);
    if (f->lose_on_write==f->writes)f->owner=0;
    return f->write_failure!=f->writes;
}
static void barrier(void *cookie) { ((struct fixture *)cookie)->barriers++; }
static uint64_t now(void *cookie)
{ struct fixture *f=cookie;if (!f->clock_stuck)f->ticks++;return f->ticks; }
static int mapping(void *cookie,const struct dre_a619_cp_buffer *b,uint32_t permissions)
{
    struct fixture *f=cookie;unsigned n=++f->mappings;
    const volatile uint32_t *cpus[]={f->ring,f->sqe,f->scratch,f->powerup,f->result};
    assert(n<=5 && b->cpu==cpus[n-1]);
    /* Ring packets are GPU-readable data, not writable shader storage and
     * not SQE executable instructions. Only the anchored microcode is RX. */
    assert(permissions==(n==1?1u:n==2?5u:3u));
    return f->mapping_failure!=(int)n;
}
static int firmware_verified(void *cookie,const void *file,size_t bytes)
{ struct fixture *f=cookie;assert(file==f->state.firmware && bytes==f->state.firmware_bytes);return f->verified; }
static int authenticate(void *cookie)
{
    struct fixture *f=cookie;f->auth_calls++;
    assert(f->regs[0x806]==15 && f->scratch[0]==15 && f->state.stage==3);
    return !f->zap_failure;
}
static void init(struct fixture *f)
{
    memset(f,0,sizeof(*f));
    f->owner=f->platform=f->verified=1;
    f->queue=(struct dre_a619_queue){.chip_id=DRE_A619_CHIP_ID,.ring=f->ring,
        .ring_region={0x100000,4096},.result_region={0x132000,4096},.result=f->result,.ring_dwords=1024};
    f->state=(struct dre_a619_cp){.chip_id=DRE_A619_CHIP_ID,.queue=&f->queue,
        .firmware=f->firmware,.firmware_bytes=sizeof(f->firmware),
        .sqe={{0x120000,32768},f->sqe},.scratch={{0x130000,4096},f->scratch},
        .powerup={{0x131000,4096},f->powerup}};
    f->ops=(struct dre_a619_cp_ops){.io={guard,read32,write32,barrier,now,f},
        .platform_ready=platform,.mapping=mapping,.firmware_verified=firmware_verified,.authenticate_zap=authenticate};
    for (unsigned i=0;i<16;i++)f->firmware[i]=i*0x01010101;
    f->firmware[1]=0x190;
    f->regs[0x808]=3;f->regs[0x842]=1;f->regs[0x84f]=11;f->regs[0x98d]=1;f->regs[0xf400]=1;
}
static const char *start(struct fixture *f)
{ return dre_a619_cp_start(&f->state,&f->ops,100); }
int main(int argc,char **argv)
{
    static struct fixture f;unsigned checks=0;
#define CHECK(want) do { const char *got=start(&f);if (strcmp(got,want))fprintf(stderr,"expected %s, got %s\n",want,got);assert(!strcmp(got,want));checks++; } while (0)
    init(&f);CHECK("NATIVE_CP_STARTED");
    assert(f.state.active && !f.state.unsafe && f.state.stage==6 && f.publishes==3);
    assert(f.queue.submitted==1 && f.queue.completed==1 && f.queue.write_pointer==26 && f.queue.ready_mask==31);
    assert(f.result[0]==0x619c001d && f.auth_calls==1 && f.regs[0x808]==1);
    unsigned writes=f.writes;CHECK("RESET_REQUIRED");assert(f.writes==writes);
    init(&f);f.state.chip_id=0;CHECK("WRONG_GPU");assert(!f.writes && !f.reads);
    init(&f);f.queue.chip_id=0;CHECK("WRONG_GPU");
    init(&f);f.queue.ready_mask=31;CHECK("QUEUE_NOT_FRESH");
    init(&f);f.queue.submitted=1;CHECK("QUEUE_NOT_FRESH");
    init(&f);f.queue.write_pointer=1;CHECK("QUEUE_NOT_FRESH");
    init(&f);f.queue.poisoned=1;CHECK("RESET_REQUIRED");
    init(&f);f.ops.mapping=0;CHECK("INVALID_ARGUMENT");
    init(&f);f.ops.authenticate_zap=0;CHECK("INVALID_ARGUMENT");
    init(&f);f.state.firmware_bytes=12;CHECK("INVALID_FIRMWARE");
    init(&f);f.queue.ring_region.gpu+=UINT64_C(1)<<32;f.queue.result_region.gpu+=UINT64_C(1)<<32;
    f.state.sqe.region.gpu+=UINT64_C(1)<<32;f.state.scratch.region.gpu+=UINT64_C(1)<<32;
    f.state.powerup.region.gpu+=UINT64_C(1)<<32;CHECK("NATIVE_CP_STARTED");
    init(&f);f.firmware[1]=0x180;CHECK("SQE_WHEREAMI_UNSUPPORTED");
    init(&f);f.firmware[1]=0xa;f.firmware[3]=0x1001;CHECK("NATIVE_CP_STARTED");
    init(&f);f.firmware[1]=0xa;f.firmware[3]=0x1000;CHECK("SQE_WHEREAMI_UNSUPPORTED");
    init(&f);f.verified=0;CHECK("FIRMWARE_DIGEST_REJECTED");assert(!f.writes && !f.mappings);
    init(&f);f.queue.ring_dwords=0;CHECK("INVALID_BUFFER_SIZE");
    init(&f);f.queue.ring_region.bytes=4092;CHECK("INVALID_BUFFER_SIZE");
    init(&f);f.state.sqe.region.bytes=32;CHECK("INVALID_BUFFER_SIZE");
    init(&f);f.state.scratch.region.bytes=8192;CHECK("INVALID_BUFFER_SIZE");
    init(&f);f.state.scratch.region.gpu++;CHECK("INVALID_MAPPING");
    init(&f);f.state.scratch.cpu++;CHECK("INVALID_MAPPING");
    init(&f);f.state.powerup.cpu=0;CHECK("INVALID_MAPPING");
    init(&f);f.state.sqe.region.gpu=UINT64_C(1)<<48;CHECK("INVALID_MAPPING");
    init(&f);f.state.powerup.region.gpu=f.state.sqe.region.gpu;CHECK("BUFFER_ALIAS");
    init(&f);f.state.sqe.cpu=f.ring;CHECK("BUFFER_ALIAS");
    for (int n=1;n<=5;n++) { init(&f);f.mapping_failure=n;CHECK("MAPPING_REJECTED");assert(!f.writes); }
    init(&f);f.state.firmware=f.powerup;memcpy(f.powerup,f.firmware,sizeof(f.firmware));CHECK("FIRMWARE_BUFFER_ALIAS");
    init(&f);f.owner=0;CHECK("PLATFORM_NOT_READY");assert(!f.writes);
    init(&f);f.platform=0;CHECK("PLATFORM_NOT_READY");assert(!f.writes);
    init(&f);f.read_failure=DRE_A619_GPU_BASE+0x210*4;CHECK("INITIAL_REGISTER_READ_FAILED");
    unsigned cold[]={0x808,0x806,0x807,0x210,0x201,0x821};
    for (unsigned i=0;i<6;i++) {
        init(&f);f.regs[cold[i]]=i?2:1;CHECK("CP_NOT_COLD_IDLE");assert(!f.writes);
    }
    unsigned initialized[]={0x842,0x84f,0x98d,0x3c45,0x18};
    for (unsigned i=0;i<5;i++) {
        init(&f);f.regs[initialized[i]]^=1;CHECK("RBBM_NOT_INITIALIZED");assert(!f.writes);
    }
    init(&f);f.read_failure=DRE_A619_GPU_BASE+0xc01*4;CHECK("REGISTER_LIST_READ_FAILED");assert(!f.writes && !f.state.unsafe);
    for (unsigned n=1;n<=8;n++) {
        init(&f);f.write_failure=n;CHECK("CP_PROGRAM_FAILED");assert(f.state.unsafe && f.queue.poisoned && !f.queue.ready_mask);
    }
    init(&f);f.bad_readback=0x830;CHECK("CP_PROGRAM_FAILED");assert(f.state.unsafe);
    init(&f);f.lose_on_write=8;CHECK("CP_PROGRAM_FAILED");assert(f.state.unsafe && !f.auth_calls);
    init(&f);f.write_failure=9;CHECK("CP_INIT_PUBLISH_FAILED");assert(f.state.unsafe);
    init(&f);f.timeout_stage=1;CHECK("CP_COMPLETION_TIMEOUT");assert(f.state.unsafe && !f.auth_calls);
    init(&f);f.bad_shadow_stage=1;CHECK("CP_COMPLETION_TIMEOUT");assert(f.state.unsafe);
    init(&f);f.fault_stage=1;CHECK("CP_HARDWARE_FAULT");assert(f.state.unsafe && !f.auth_calls);
    init(&f);f.irq_stage=1;CHECK("CP_HARDWARE_FAULT");
    init(&f);f.zap_failure=1;CHECK("ZAP_AUTHENTICATION_FAILED");assert(f.publishes==1 && f.state.unsafe);
    init(&f);f.write_failure=10;CHECK("CP_SECURE_PUBLISH_FAILED");assert(f.state.unsafe);
    init(&f);f.timeout_stage=2;CHECK("CP_COMPLETION_TIMEOUT");assert(f.state.unsafe);
    init(&f);f.secure_failure=1;CHECK("NONSECURE_TRANSITION_UNVERIFIED");assert(f.state.unsafe);
    init(&f);f.fault_stage=2;CHECK("CP_HARDWARE_FAULT");
    init(&f);f.timeout_stage=3;CHECK("COMPLETION_TIMEOUT");assert(f.state.unsafe && f.queue.submitted==1 && !f.queue.completed);
    init(&f);f.fault_stage=3;CHECK("POST_MARKER_FAULT");assert(f.state.unsafe && !f.queue.ready_mask);
    init(&f);f.timeout_stage=1;f.clock_stuck=1;CHECK("CP_CLOCK_STALLED");assert(f.state.unsafe);
    if (argc==2) {
        FILE *file=fopen(argv[1],"rb");assert(file);
        assert(!fseek(file,0,SEEK_END));long bytes=ftell(file);
        assert(bytes>=16 && bytes<=32772 && !(bytes&3));rewind(file);
        void *firmware=malloc((size_t)bytes);assert(firmware);
        assert(fread(firmware,1,(size_t)bytes,file)==(size_t)bytes);assert(!fclose(file));
        init(&f);f.state.firmware=firmware;f.state.firmware_bytes=(size_t)bytes;
        CHECK("NATIVE_CP_STARTED");free(firmware);
        puts("PASS private SQE prefix exclusion, upload, zero padding and initialization model; no native execution");
    } else assert(argc==1);
    printf("PASS %u bounded A619 CP/SQE/authenticated-transition cases; synthetic hardware only\n",checks);
}
