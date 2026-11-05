/* SPDX-License-Identifier: MIT
 * Hardware register facts for the restricted A619-Holi nonsecure profile.
 * Checked against pinned 98a38783 adreno_a6xx.c, adreno-gpulist.h,
 * a6xx_reg.h and holi-gpu.dtsi. No firmware or private identifiers. */
#include "a619_rbbm.h"
#define REG(n) (DRE_A619_GPU_BASE+(uint64_t)(n)*4)
struct setting { uint32_t reg,value; };
static const struct setting base_settings[]={
    {0x0842,0x00000001}, /* A6XX_CP_ADDR_MODE_CNTL */
    {0x0c01,0x00000001}, /* A6XX_VSC_ADDR_MODE_CNTL */
    {0x8601,0x00000001}, /* A6XX_GRAS_ADDR_MODE_CNTL */
    {0x8e05,0x00000001}, /* A6XX_RB_ADDR_MODE_CNTL */
    {0x9e01,0x00000001}, /* A6XX_PC_ADDR_MODE_CNTL */
    {0xbe05,0x00000001}, /* A6XX_HLSQ_ADDR_MODE_CNTL */
    {0xa601,0x00000001}, /* A6XX_VFD_ADDR_MODE_CNTL */
    {0x9601,0x00000001}, /* A6XX_VPC_ADDR_MODE_CNTL */
    {0x0e00,0x00000001}, /* A6XX_UCHE_ADDR_MODE_CNTL */
    {0xae01,0x00000001}, /* A6XX_SP_ADDR_MODE_CNTL */
    {0xb601,0x00000001}, /* A6XX_TPL1_ADDR_MODE_CNTL */
    {0x0010,0x00000003}, /* A6XX_RBBM_VBIF_CLIENT_QOS_CNTL */
    {0x050b,0xffffffff}, /* A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED */
    {0x0e05,0xffffffc0}, /* A6XX_UCHE_WRITE_RANGE_MAX_LO */
    {0x0e06,0x0001ffff}, /* A6XX_UCHE_WRITE_RANGE_MAX_HI */
    {0x0e09,0xfffff000}, /* A6XX_UCHE_TRAP_BASE_LO */
    {0x0e0a,0x0001ffff}, /* A6XX_UCHE_TRAP_BASE_HI */
    {0x0e07,0xfffff000}, /* A6XX_UCHE_WRITE_THRU_BASE_LO */
    {0x0e08,0x0001ffff}, /* A6XX_UCHE_WRITE_THRU_BASE_HI */
    {0x0e0b,0x00100000}, /* A6XX_UCHE_GMEM_RANGE_MIN_LO */
    {0x0e0c,0x00000000}, /* A6XX_UCHE_GMEM_RANGE_MIN_HI */
    {0x0e0d,0x0017ffff}, /* A6XX_UCHE_GMEM_RANGE_MAX_LO */
    {0x0e0e,0x00000000}, /* A6XX_UCHE_GMEM_RANGE_MAX_HI */
    {0x0e18,0x00000804}, /* A6XX_UCHE_FILTER_CNTL */
    {0x0e17,0x00000004}, /* A6XX_UCHE_CACHE_WAYS */
    {0x08c2,0x010000c0}, /* A6XX_CP_ROQ_THRESHOLDS_2 */
    {0x08c1,0x8040362c}, /* A6XX_CP_ROQ_THRESHOLDS_1 */
    {0x08c3,0x00000080}, /* A6XX_CP_MEM_POOL_SIZE */
    {0x9e00,0x00018000}, /* A6XX_PC_DBG_ECO_CNTL */
    {0x098d,0x00000001}, /* A6XX_CP_AHB_CNTL */
    {0x0500,0x00000001}, /* A6XX_RBBM_PERFCTR_CNTL */
    {0x8e08,0x00000002}, /* A6XX_RB_NC_MODE_CNTL */
    {0xb604,0x00000002}, /* A6XX_TPL1_NC_MODE_CNTL */
    {0xae02,0x00000002}, /* A6XX_SP_NC_MODE_CNTL */
    {0x0e01,0x00200000}, /* A6XX_UCHE_MODE_CNTL */
    {0x001f,0x403fffff}, /* A6XX_RBBM_INTERFACE_HANG_INT_CNTL */
    {0x0e19,0x00000001}, /* A6XX_UCHE_CLIENT_PF */
    {0x8e50,0x00000000}, /* A6XX_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE */
};
static const struct setting hwcg_settings[]={
    {0x00b0,0x02222222}, /* A6XX_RBBM_CLOCK_CNTL_SP0 */
    {0x00b4,0x02222220}, /* A6XX_RBBM_CLOCK_CNTL2_SP0 */
    {0x00b8,0x00000080}, /* A6XX_RBBM_CLOCK_DELAY_SP0 */
    {0x00bc,0x0000f3cf}, /* A6XX_RBBM_CLOCK_HYST_SP0 */
    {0x00c0,0x02222222}, /* A6XX_RBBM_CLOCK_CNTL_TP0 */
    {0x00c1,0x02222222}, /* A6XX_RBBM_CLOCK_CNTL_TP1 */
    {0x00c4,0x22222222}, /* A6XX_RBBM_CLOCK_CNTL2_TP0 */
    {0x00c5,0x22222222}, /* A6XX_RBBM_CLOCK_CNTL2_TP1 */
    {0x00c8,0x22222222}, /* A6XX_RBBM_CLOCK_CNTL3_TP0 */
    {0x00c9,0x22222222}, /* A6XX_RBBM_CLOCK_CNTL3_TP1 */
    {0x00cc,0x00022222}, /* A6XX_RBBM_CLOCK_CNTL4_TP0 */
    {0x00cd,0x00022222}, /* A6XX_RBBM_CLOCK_CNTL4_TP1 */
    {0x00e0,0x77777777}, /* A6XX_RBBM_CLOCK_HYST_TP0 */
    {0x00e1,0x77777777}, /* A6XX_RBBM_CLOCK_HYST_TP1 */
    {0x00e4,0x77777777}, /* A6XX_RBBM_CLOCK_HYST2_TP0 */
    {0x00e5,0x77777777}, /* A6XX_RBBM_CLOCK_HYST2_TP1 */
    {0x00e8,0x77777777}, /* A6XX_RBBM_CLOCK_HYST3_TP0 */
    {0x00e9,0x77777777}, /* A6XX_RBBM_CLOCK_HYST3_TP1 */
    {0x00ec,0x00077777}, /* A6XX_RBBM_CLOCK_HYST4_TP0 */
    {0x00ed,0x00077777}, /* A6XX_RBBM_CLOCK_HYST4_TP1 */
    {0x00d0,0x11111111}, /* A6XX_RBBM_CLOCK_DELAY_TP0 */
    {0x00d1,0x11111111}, /* A6XX_RBBM_CLOCK_DELAY_TP1 */
    {0x00d4,0x11111111}, /* A6XX_RBBM_CLOCK_DELAY2_TP0 */
    {0x00d5,0x11111111}, /* A6XX_RBBM_CLOCK_DELAY2_TP1 */
    {0x00d8,0x11111111}, /* A6XX_RBBM_CLOCK_DELAY3_TP0 */
    {0x00d9,0x11111111}, /* A6XX_RBBM_CLOCK_DELAY3_TP1 */
    {0x00dc,0x00011111}, /* A6XX_RBBM_CLOCK_DELAY4_TP0 */
    {0x00dd,0x00011111}, /* A6XX_RBBM_CLOCK_DELAY4_TP1 */
    {0x010b,0x22222222}, /* A6XX_RBBM_CLOCK_CNTL_UCHE */
    {0x010c,0x22222222}, /* A6XX_RBBM_CLOCK_CNTL2_UCHE */
    {0x010d,0x22222222}, /* A6XX_RBBM_CLOCK_CNTL3_UCHE */
    {0x010e,0x00222222}, /* A6XX_RBBM_CLOCK_CNTL4_UCHE */
    {0x0110,0x00000004}, /* A6XX_RBBM_CLOCK_HYST_UCHE */
    {0x010f,0x00000002}, /* A6XX_RBBM_CLOCK_DELAY_UCHE */
    {0x00f0,0x22222222}, /* A6XX_RBBM_CLOCK_CNTL_RB0 */
    {0x00f4,0x00002222}, /* A6XX_RBBM_CLOCK_CNTL2_RB0 */
    {0x00f8,0x00002220}, /* A6XX_RBBM_CLOCK_CNTL_CCU0 */
    {0x00f9,0x00002220}, /* A6XX_RBBM_CLOCK_CNTL_CCU1 */
    {0x00fa,0x00002220}, /* A6XX_RBBM_CLOCK_CNTL_CCU2 */
    {0x00fb,0x00002220}, /* A6XX_RBBM_CLOCK_CNTL_CCU3 */
    {0x0100,0x00040f00}, /* A6XX_RBBM_CLOCK_HYST_RB_CCU0 */
    {0x0101,0x00040f00}, /* A6XX_RBBM_CLOCK_HYST_RB_CCU1 */
    {0x0102,0x00040f00}, /* A6XX_RBBM_CLOCK_HYST_RB_CCU2 */
    {0x0103,0x00040f00}, /* A6XX_RBBM_CLOCK_HYST_RB_CCU3 */
    {0x0104,0x05022022}, /* A6XX_RBBM_CLOCK_CNTL_RAC */
    {0x0105,0x00005555}, /* A6XX_RBBM_CLOCK_CNTL2_RAC */
    {0x0106,0x00000011}, /* A6XX_RBBM_CLOCK_DELAY_RAC */
    {0x0107,0x00445044}, /* A6XX_RBBM_CLOCK_HYST_RAC */
    {0x0108,0x04222222}, /* A6XX_RBBM_CLOCK_CNTL_TSE_RAS_RBBM */
    {0x0114,0x00222222}, /* A6XX_RBBM_CLOCK_MODE_GPC */
    {0x0111,0x00002222}, /* A6XX_RBBM_CLOCK_MODE_VFD */
    {0x010a,0x00000000}, /* A6XX_RBBM_CLOCK_HYST_TSE_RAS_RBBM */
    {0x0116,0x04104004}, /* A6XX_RBBM_CLOCK_HYST_GPC */
    {0x0113,0x00000000}, /* A6XX_RBBM_CLOCK_HYST_VFD */
    {0x011c,0x00000000}, /* A6XX_RBBM_CLOCK_DELAY_HLSQ */
    {0x0109,0x00004000}, /* A6XX_RBBM_CLOCK_DELAY_TSE_RAS_RBBM */
    {0x0115,0x00000200}, /* A6XX_RBBM_CLOCK_DELAY_GPC */
    {0x0112,0x00002222}, /* A6XX_RBBM_CLOCK_DELAY_VFD */
    {0x0117,0x00000002}, /* A6XX_RBBM_CLOCK_DELAY_HLSQ_2 */
    {0x011b,0x00002222}, /* A6XX_RBBM_CLOCK_MODE_HLSQ */
    {0x0118,0x00000222}, /* A6XX_RBBM_CLOCK_CNTL_GMU_GX */
    {0x0119,0x00000111}, /* A6XX_RBBM_CLOCK_DELAY_GMU_GX */
    {0x011a,0x00000555}, /* A6XX_RBBM_CLOCK_HYST_GMU_GX */
};
static const uint32_t protection_settings[32]={
    0x13fc0000, /* span0: 0x0..0x4ff, noaccess=0 */
    0x00140501, /* span1: 0x501..0x506, noaccess=0 */
    0x0bd0050b, /* span2: 0x50b..0x7ff, noaccess=0 */
    0x8000050e, /* span3: 0x50e..0x50e, noaccess=1 */
    0x80000510, /* span4: 0x510..0x510, noaccess=1 */
    0x80000534, /* span5: 0x534..0x534, noaccess=1 */
    0x82080800, /* span6: 0x800..0x882, noaccess=1 */
    0x802008a0, /* span7: 0x8a0..0x8a8, noaccess=1 */
    0x809008ab, /* span8: 0x8ab..0x8cf, noaccess=1 */
    0x02f008d0, /* span9: 0x8d0..0x98c, noaccess=0 */
    0x81340900, /* span10: 0x900..0x94d, noaccess=1 */
    0x89c8098d, /* span11: 0x98d..0xbff, noaccess=1 */
    0x80040e00, /* span12: 0xe00..0xe01, noaccess=1 */
    0x80300e03, /* span13: 0xe03..0xe0f, noaccess=1 */
    0x830c3c00, /* span14: 0x3c00..0x3cc3, noaccess=1 */
    0x7ffc3cc4, /* span15: 0x3cc4..0x5cc3, noaccess=0 */
    0x873c8630, /* span16: 0x8630..0x87ff, noaccess=1 */
    0x80008e00, /* span17: 0x8e00..0x8e00, noaccess=1 */
    0x80008e08, /* span18: 0x8e08..0x8e08, noaccess=1 */
    0x807c8e50, /* span19: 0x8e50..0x8e6f, noaccess=1 */
    0x876c9624, /* span20: 0x9624..0x97ff, noaccess=1 */
    0x80049e70, /* span21: 0x9e70..0x9e71, noaccess=1 */
    0x861c9e78, /* span22: 0x9e78..0x9fff, noaccess=1 */
    0x873ca630, /* span23: 0xa630..0xa7ff, noaccess=1 */
    0x8000ae02, /* span24: 0xae02..0xae02, noaccess=1 */
    0x8cbcae50, /* span25: 0xae50..0xb17f, noaccess=1 */
    0x8000b604, /* span26: 0xb604..0xb604, noaccess=1 */
    0x8004be02, /* span27: 0xbe02..0xbe03, noaccess=1 */
    0xdf7cbe20, /* span28: 0xbe20..0xd5ff, noaccess=1 */
    0xaffcf000, /* span29: 0xf000..0xfbff, noaccess=1 */
    0x7ffcfc00, /* span30: 0xfc00..0x11bff, noaccess=0 */
    0x80011c00, /* span31: 0x11c00..0x11c00, noaccess=1 */
};

static int ready(const struct dre_a619_rbbm_ops *o)
{ return o->io.guard(o->io.cookie) && o->providers_ready(o->io.cookie); }
static int read_reg(const struct dre_a619_rbbm_ops *o,uint32_t reg,uint32_t *value)
{ return ready(o) && o->io.read32(o->io.cookie,REG(reg),value); }
static int set(struct dre_a619_rbbm *s,const struct dre_a619_rbbm_ops *o,uint32_t reg,uint32_t value)
{
    if (!ready(o))return 0;
    s->last_register=REG(reg);s->last_expected=value;s->last_observed=0;
    s->changed=1;s->writes++;
    if (!o->io.write32(o->io.cookie,REG(reg),value))return 0;
    o->io.barrier(o->io.cookie);
    return read_reg(o,reg,&s->last_observed) && s->last_observed==value;
}
static int modify(struct dre_a619_rbbm *s,const struct dre_a619_rbbm_ops *o,
    uint32_t reg,uint32_t clear,uint32_t bits)
{
    uint32_t value;
    return read_reg(o,reg,&value) && set(s,o,reg,(value&~clear)|bits);
}
static int apply(struct dre_a619_rbbm *s,const struct dre_a619_rbbm_ops *o,
    const struct setting *settings,size_t count)
{
    for (size_t i=0;i<count;i++)if (!set(s,o,settings[i].reg,settings[i].value))return 0;
    return 1;
}
static const char *failed(struct dre_a619_rbbm *s,const char *reason)
{ if (s->changed) { s->unsafe=1;s->active=0; }return reason; }
static int idle(const struct dre_a619_rbbm_ops *o)
{
    uint32_t status,irq,fault,rptr,wptr,control,mask;
    return read_reg(o,0x210,&status) && !(status&0xfffffffeu) &&
        read_reg(o,0x201,&irq) && !irq && read_reg(o,0x821,&fault) && !fault &&
        read_reg(o,0x806,&rptr) && !rptr && read_reg(o,0x807,&wptr) && !wptr &&
        read_reg(o,0x808,&control) && (control&2) &&
        read_reg(o,0x38,&mask) && !mask; /* no IRQ handler is installed here */
}
const char *dre_a619_rbbm_initialize(struct dre_a619_rbbm *s,const struct dre_a619_rbbm_ops *o)
{
    if (!s || !o || !o->providers_ready || !o->io.guard || !o->io.read32 ||
        !o->io.write32 || !o->io.barrier)return "INVALID_ARGUMENT";
    if (s->unsafe || s->changed || s->active)return "RESET_REQUIRED";
    if (s->chip_id!=DRE_A619_CHIP_ID)return "WRONG_GPU";
    if (s->highest_bank_bit!=14 || s->gmem_bytes!=524288 ||
        s->min_access_length!=32 || s->ubwc_mode!=2)return "UNSUPPORTED_BOARD_PROFILE";
    if (!ready(o))return "PLATFORM_PROVIDERS_UNAVAILABLE";
    if (!idle(o))return "GPU_NOT_COLD_IDLE";
    if (!apply(s,o,base_settings,sizeof(base_settings)/sizeof(base_settings[0])))
        return failed(s,"BASE_REGISTER_PROGRAM_FAILED");
    /* Holi's GMU wrapper exposes the busy mask/counters despite no GMU/HFI. */
    uint32_t counter;
    if (!set(s,o,0x23b0e,0xff000000) || !read_reg(o,0x1f841,&counter) ||
        !set(s,o,0x1f841,(counter&0xff)|0x20) || !set(s,o,0x1f840,1))
        return failed(s,"WRAPPER_COUNTER_PROGRAM_FAILED");
    if (!set(s,o,0x84f,11))return failed(s,"PROTECTION_PROGRAM_FAILED");
    for (unsigned i=0;i<32;i++)
        if (!set(s,o,0x850+i,protection_settings[i]))return failed(s,"PROTECTION_PROGRAM_FAILED");
    /* No LLCC slice is allocated in this restricted SMMUv2 profile. The
     * pinned driver still applies its non-V500 no-write-allocation override. */
    if (!set(s,o,0x27801,3))return failed(s,"CACHE_OVERRIDE_PROGRAM_FAILED");
    /* Published Holi clock-gating order: gate SP, write per-block settings,
     * set UCHE/GBIF L2 CGC, restore SP, then enable the top-level clock gate. */
    if (!modify(s,o,0x1a880,1,0) ||
        !apply(s,o,hwcg_settings,sizeof(hwcg_settings)/sizeof(hwcg_settings[0])) ||
        !modify(s,o,0xe3a,0x70000,2) || !modify(s,o,0x1a880,0,1) ||
        !set(s,o,0xae,0x8aa8aa82))return failed(s,"CLOCK_GATING_PROGRAM_FAILED");
    if (!set(s,o,0x3c45,0) || !set(s,o,0x18,0))return failed(s,"BUS_UNHALT_FAILED");
    if (!idle(o) || !ready(o))return failed(s,"FINAL_IDLE_UNVERIFIED");
    s->active=1;
    return "RBBM_INITIALIZED";
}
