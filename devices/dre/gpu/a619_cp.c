/* SPDX-License-Identifier: MIT
 * Restricted native A619-Holi CP initialization. Register/packet facts and
 * ordering checked against LineageOS android_kernel_oneplus_sm4350 commit
 * 98a38783f42f5588bf00cd933e299aedc589bc10 adreno_a6xx.c/h, adreno.h,
 * adreno_ringbuffer.c, a6xx_reg.h and adreno_pm4types.h. SQE WHERE_AM_I minimum
 * version additionally checked against Linux v6.12 a6xx_gpu.c. No Linux code,
 * firmware, secure-authentication bypass, or Metal implementation is included. */
#include "a619_cp.h"

#define REG(n) (DRE_A619_GPU_BASE+(uint64_t)(n)*4)
#define SQE_CNTL REG(0x808)
#define TRUST_CNTL REG(0xf400)
#define INIT_DWORDS 15u /* pinned 12-dword ME_INIT + WHERE_AM_I */
#define SECURE_DWORDS 5u /* SET_SECURE_MODE + WHERE_AM_I */
#define IFPC_PAIRS 36u
#define RESTORE_PAIRS 32u
#define LIST_DWORDS ((IFPC_PAIRS+RESTORE_PAIRS)*2u)

/* GPU register indices, not arbitrary user-supplied register writes. The
 * second 0xe3a is intentional: a615-family's extra restore entry. */
static const uint16_t restore_registers[RESTORE_PAIRS]={
    0x0c01,0x8601,0x8e05,0x9e01,0xbe05,0xa601,0x9601,0x0e00,
    0xae01,0xb601,0x0e05,0x0e06,0x0e09,0x0e0a,0x0e07,0x0e08,
    0x0e0b,0x0e0c,0x0e0d,0x0e0e,0x0e18,0x0e17,0x0e01,0x8e08,
    0xb604,0xae02,0x9e00,0x8e50,0x0e3a,0x0e3a,0x0011,0x0500
};
static uint32_t little(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int equal(const char *a,const char *b)
{ while (*a && *a==*b) { a++;b++; }return *a==*b; }
static int region_valid(struct dre_a619_region r)
{ return r.bytes && r.bytes<=1048576 && r.gpu<(UINT64_C(1)<<48) &&
    r.bytes<=(UINT64_C(1)<<48)-r.gpu; }
static int intersects(struct dre_a619_region a,struct dre_a619_region b)
{ return a.gpu<b.gpu+b.bytes && b.gpu<a.gpu+a.bytes; }
static int cpu_intersects(const struct dre_a619_cp_buffer *a,const struct dre_a619_cp_buffer *b)
{
    uintptr_t ap=(uintptr_t)a->cpu,bp=(uintptr_t)b->cpu;
    if (a->region.bytes>UINTPTR_MAX-ap || b->region.bytes>UINTPTR_MAX-bp)return 1;
    return ap<bp+b->region.bytes && bp<ap+a->region.bytes;
}
static int live(const struct dre_a619_cp_ops *o)
{ return o->io.guard(o->io.cookie) && o->platform_ready(o->io.cookie); }
static int read_reg(const struct dre_a619_cp_ops *o,uint64_t address,uint32_t *value)
{ return live(o) && o->io.read32(o->io.cookie,address,value); }
static int write_reg(struct dre_a619_cp *s,const struct dre_a619_cp_ops *o,uint64_t address,uint32_t value)
{
    if (!live(o))return 0;
    /* Even an MMIO callback reporting failure may have reached hardware. */
    s->changed=1;
    if (!o->io.write32(o->io.cookie,address,value))return 0;
    o->io.barrier(o->io.cookie);
    return live(o);
}
static const char *failure(struct dre_a619_cp *s,const char *reason)
{
    if (s->changed) {
        s->unsafe=1;s->active=0;
        s->queue->ready_mask=0;s->queue->poisoned=1;
    }
    return reason;
}
static int snapshot(struct dre_a619_cp *s,const struct dre_a619_cp_ops *o)
{
    return read_reg(o,REG(0x210),&s->status) &&
        read_reg(o,REG(0x201),&s->irq_status) &&
        read_reg(o,REG(0x821),&s->hw_fault) &&
        read_reg(o,DRE_A619_RB_RPTR,&s->observed_rptr) &&
        read_reg(o,DRE_A619_RB_WPTR,&s->observed_wptr);
}
static const char *wait_idle(struct dre_a619_cp *s,const struct dre_a619_cp_ops *o,
    uint32_t expected,uint64_t budget)
{
    uint64_t start=o->io.now(o->io.cookie);
    unsigned polls=0;
    do {
        if (!snapshot(s,o))return "COMPLETION_READ_FAILED";
        if (s->hw_fault || s->irq_status)return "CP_HARDWARE_FAULT";
        o->io.barrier(o->io.cookie);
        if (s->observed_rptr==expected && s->observed_wptr==expected &&
            !(s->status&0xfffffffeu) && *s->scratch.cpu==expected)
            return live(o)?(const char *)0:"OWNERSHIP_LOST";
        if (++polls==1000000)return "CP_CLOCK_STALLED";
    } while (!dre_a619_deadline_expired(start, o->io.now(o->io.cookie), budget));
    return "CP_COMPLETION_TIMEOUT";
}
static int program_checked(struct dre_a619_cp *s,const struct dre_a619_cp_ops *o,
    uint64_t address,uint32_t value)
{
    uint32_t actual;
    return write_reg(s,o,address,value) && read_reg(o,address,&actual) && actual==value;
}
static uint32_t ifpc_register(unsigned i)
{
    if (i==0)return 0x841;
    if (i==1)return 0x843;
    if (i==2)return 0x84f;
    if (i<35)return 0x850+i-3;
    return 0x98d;
}
static const char *prepare(struct dre_a619_cp *s,const struct dre_a619_cp_ops *o,
    const struct dre_a619_sqe *fw)
{
    volatile uint32_t *p=s->powerup.cpu;
    for (size_t i=0;i<s->powerup.region.bytes/4;i++)p[i]=0;
    for (unsigned i=0;i<IFPC_PAIRS+RESTORE_PAIRS;i++) {
        uint32_t r=i<IFPC_PAIRS?ifpc_register(i):restore_registers[i-IFPC_PAIRS],v;
        /* The driver's performance-counter restore value is explicitly one. */
        if (i==IFPC_PAIRS+RESTORE_PAIRS-1)v=1;
        else if (!read_reg(o,REG(r),&v))return "REGISTER_LIST_READ_FAILED";
        p[4+i*2]=r;p[5+i*2]=v;
    }
    p[3]=LIST_DWORDS|((IFPC_PAIRS*2u)<<16);
    for (size_t i=0;i<s->sqe.region.bytes/4;i++)
        s->sqe.cpu[i]=i<fw->bytes/4?little(fw->code+i*4):0;
    for (unsigned i=0;i<s->queue->ring_dwords;i++)s->queue->ring[i]=0xaaaaaaaau;
    *s->scratch.cpu=0;
    volatile uint32_t *r=s->queue->ring;
    r[0]=dre_a619_type7(0x48,11);r[1]=0x16f;r[2]=3;r[3]=0x20000000;
    r[4]=0;r[5]=0;r[6]=0;r[7]=2;
    r[8]=(uint32_t)s->powerup.region.gpu;r[9]=(uint32_t)(s->powerup.region.gpu>>32);
    r[10]=0;r[11]=0; /* pinned kzalloc trailing ordinal/padding */
    r[12]=dre_a619_type7(0x62,2);
    r[13]=(uint32_t)s->scratch.region.gpu;r[14]=(uint32_t)(s->scratch.region.gpu>>32);
    o->io.barrier(o->io.cookie);
    return live(o)?(const char *)0:"OWNERSHIP_LOST";
}
const char *dre_a619_cp_start(struct dre_a619_cp *s,
    const struct dre_a619_cp_ops *o,uint64_t budget)
{
    if (!s || !o || !o->io.guard || !o->io.read32 || !o->io.write32 ||
        !o->io.barrier || !o->io.now || !o->platform_ready || !o->mapping ||
        !o->firmware_verified || !o->authenticate_zap || !s->queue ||
        !budget || budget>UINT64_MAX/2)return "INVALID_ARGUMENT";
    struct dre_a619_queue *q=s->queue;
    if (s->unsafe || s->changed || q->poisoned)return "RESET_REQUIRED";
    if (s->chip_id!=DRE_A619_CHIP_ID || q->chip_id!=s->chip_id)return "WRONG_GPU";
    if (s->active || q->ready_mask || q->submitted || q->completed || q->write_pointer)
        return "QUEUE_NOT_FRESH";
    struct dre_a619_sqe fw;
    const char *error=dre_a619_sqe_view(s->chip_id,s->firmware,s->firmware_bytes,&fw);
    if (!equal(error,"SQE_VIEW_READY"))return "INVALID_FIRMWARE";
    uint32_t v=little(fw.code);
    if (!(((v&15)==10 && (little(fw.code+8)&15)>=1) || (v&0xfff)>=0x190))
        return "SQE_WHEREAMI_UNSUPPORTED";
    if (!o->firmware_verified(o->io.cookie,s->firmware,s->firmware_bytes))
        return "FIRMWARE_DIGEST_REJECTED";
    struct dre_a619_cp_buffer b[5]={
        {q->ring_region,q->ring},s->sqe,s->scratch,s->powerup,{q->result_region,q->result}};
    if (q->ring_dwords<1024 || q->ring_dwords>16384 ||
        (q->ring_dwords&(q->ring_dwords-1)) || q->ring_region.bytes!=(uint64_t)q->ring_dwords*4 ||
        s->sqe.region.bytes<fw.bytes || (s->sqe.region.bytes&4095) ||
        s->scratch.region.bytes!=4096 || s->powerup.region.bytes!=4096 || q->result_region.bytes!=4096)
        return "INVALID_BUFFER_SIZE";
    for (unsigned i=0;i<5;i++) {
        if (!region_valid(b[i].region) || (b[i].region.gpu&4095) ||
            !b[i].cpu || ((uintptr_t)b[i].cpu&4095))return "INVALID_MAPPING";
        for (unsigned j=0;j<i;j++)
            if (intersects(b[i].region,b[j].region) || cpu_intersects(&b[i],&b[j]))return "BUFFER_ALIAS";
        /* Holi has no ADRENO_APRIV. Its ring is ordinary GPU-readonly/XN;
         * normal command fetch does not require SMMU instruction execute.
         * Only actual SQE microcode carries execute permission. */
        uint32_t permissions=DRE_A619_CP_READ;
        if(i==1)permissions|=DRE_A619_CP_EXECUTE;
        else if(i!=0)permissions|=DRE_A619_CP_WRITE;
        if (!o->mapping(o->io.cookie,&b[i],permissions))return "MAPPING_REJECTED";
    }
    struct dre_a619_cp_buffer input={{0,s->firmware_bytes},(volatile uint32_t *)s->firmware};
    for (unsigned i=0;i<5;i++)if (cpu_intersects(&input,&b[i]))return "FIRMWARE_BUFFER_ALIAS";
    if (!live(o))return "PLATFORM_NOT_READY";
    uint32_t cntl,mode,protection,ahb,halt,gpr;
    if (!snapshot(s,o) || !read_reg(o,SQE_CNTL,&cntl) ||
        !read_reg(o,REG(0x842),&mode) || !read_reg(o,REG(0x84f),&protection) ||
        !read_reg(o,REG(0x98d),&ahb) || !read_reg(o,REG(0x3c45),&halt) ||
        !read_reg(o,REG(0x18),&gpr))return "INITIAL_REGISTER_READ_FAILED";
    if (!(cntl&2) || s->observed_rptr || s->observed_wptr ||
        (s->status&0xfffffffeu) || s->irq_status || s->hw_fault)return "CP_NOT_COLD_IDLE";
    if (mode!=1 || protection!=11 || ahb!=1 || halt || gpr)return "RBBM_NOT_INITIALIZED";
    s->stage=1;
    error=prepare(s,o,&fw);
    if (error)return error; /* no device register has yet been written */
    unsigned size=0;while ((2u<<size)!=q->ring_dwords)size++;
    if (!program_checked(s,o,REG(0x804),(uint32_t)s->scratch.region.gpu) ||
        !program_checked(s,o,REG(0x805),(uint32_t)(s->scratch.region.gpu>>32)) ||
        !program_checked(s,o,DRE_A619_RB_CNTL,(1u<<27)|(2u<<8)|size) ||
        !program_checked(s,o,DRE_A619_RB_BASE,(uint32_t)q->ring_region.gpu) ||
        !program_checked(s,o,DRE_A619_RB_BASE_HI,(uint32_t)(q->ring_region.gpu>>32)) ||
        !program_checked(s,o,REG(0x830),(uint32_t)s->sqe.region.gpu) ||
        !program_checked(s,o,REG(0x831),(uint32_t)(s->sqe.region.gpu>>32)) ||
        !program_checked(s,o,SQE_CNTL,1))return failure(s,"CP_PROGRAM_FAILED");
    s->stage=2;
    if (!write_reg(s,o,DRE_A619_RB_WPTR,INIT_DWORDS))return failure(s,"CP_INIT_PUBLISH_FAILED");
    error=wait_idle(s,o,INIT_DWORDS,budget);
    if (error)return failure(s,error);
    s->stage=3;
    if (!live(o) || !o->authenticate_zap(o->io.cookie) || !live(o))
        return failure(s,"ZAP_AUTHENTICATION_FAILED");
    s->stage=4;
    unsigned p=INIT_DWORDS;
    q->ring[p++]=dre_a619_type7(0x66,1);q->ring[p++]=0;
    q->ring[p++]=dre_a619_type7(0x62,2);
    q->ring[p++]=(uint32_t)s->scratch.region.gpu;q->ring[p++]=(uint32_t)(s->scratch.region.gpu>>32);
    *s->scratch.cpu=0xffffffffu;o->io.barrier(o->io.cookie);
    if (!write_reg(s,o,DRE_A619_RB_WPTR,p))return failure(s,"CP_SECURE_PUBLISH_FAILED");
    error=wait_idle(s,o,p,budget);
    if (error)return failure(s,error);
    uint32_t trust;
    if (!read_reg(o,TRUST_CNTL,&trust) || trust)return failure(s,"NONSECURE_TRANSITION_UNVERIFIED");
    s->stage=5;q->write_pointer=p;q->ready_mask=DRE_A619_READY_MASK;
    error=dre_a619_submit_marker(q,&o->io,q->result_region.gpu,0x619c001du,budget);
    if (!equal(error,"MARKER_COMPLETED"))return failure(s,error);
    if (!live(o) || !snapshot(s,o) || s->hw_fault || s->irq_status || (s->status&0xfffffffeu))
        return failure(s,"POST_MARKER_FAULT");
    s->stage=6;s->active=1;
    return "NATIVE_CP_STARTED";
}
