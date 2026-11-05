/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Include at EOF of selected pexpert/arm/pe_fiq.c. Uses its real
 * initialized static mapping; never creates a second GIC map or resets it.
 */
#include "spi_low.h"
#if defined(DRE) && HAS_GIC_V3
static uint64_t dre_spi_gicd_pa,dre_spi_gicd_bytes,dre_spi_gicr_pa,dre_spi_gicr_bytes;
static int dre_spi_pe_bound;
void dre_spi_pe_bind(uint64_t dpa,uint64_t db,uint64_t rpa,uint64_t rb)
{
    /* Only call at the existing pe_init_fiq mapping site. Refuse a second
     * mapping/generation rather than silently retargeting live IRQ leases. */
    if(dre_spi_pe_bound||dpa!=UINT64_C(0x0f200000)||db!=0x10000||
       rpa!=UINT64_C(0x0f240000)||rb!=0x100000||!gicd_base||!gicr_base) {
        dre_spi_pe_bound=-1;return;
    }
    dre_spi_gicd_pa=dpa;dre_spi_gicd_bytes=db;
    dre_spi_gicr_pa=rpa;dre_spi_gicr_bytes=rb;
    dre_spi_pe_bound=1;
}
uint64_t dre_spi_counter(void)
{ return __builtin_arm_rsr64("CNTPCT_EL0"); }
uint64_t dre_spi_counter_frequency(void)
{ return __builtin_arm_rsr64("CNTFRQ_EL0"); }
int dre_spi_pe_read(uint32_t id,struct dre_spi_hw *o)
{
    if(!o||id<32||id>=1020)return DRE_SPI_ARGUMENT;
    if(dre_spi_pe_bound!=1||!gicd_base||!gicr_base||
       dre_spi_counter_frequency()!=19200000)return DRE_SPI_PLATFORM;
    uint32_t ctl=gicd_read32(0),typer=gicd_read32(4);
    uint32_t implemented=((typer&31)+1)*32;
    if(!(ctl&0x10)||!(ctl&3)||(ctl&0x80000000)||id>=implemented)
        return DRE_SPI_PLATFORM;
    uint32_t bank=(id/32)*4,bit=UINT32_C(1)<<(id&31);
    o->distributor_pa=dre_spi_gicd_pa;o->distributor_bytes=dre_spi_gicd_bytes;
    o->redistributor_pa=dre_spi_gicr_pa;o->redistributor_bytes=dre_spi_gicr_bytes;
    o->intid=id;o->ctlr=ctl;o->typer=typer;o->pidr2=gicd_read32(0xffe8);
    if((o->pidr2&0xf0)!=0x30)return DRE_SPI_PLATFORM;
    o->group=!!(gicd_read32(0x80+bank)&bit);
    o->groupmod=!!(gicd_read32(0xd00+bank)&bit);
    o->configuration=(gicd_read32(0xc00+(id/16)*4)>>((id&15)*2))&3;
    uint32_t priorities=gicd_read32(0x400+(id&~3u));
    o->priority=(priorities>>((id&3)*8))&255;
    o->enabled=!!(gicd_read32(0x100+bank)&bit);
    o->pending=!!(gicd_read32(0x200+bank)&bit);
    o->active=!!(gicd_read32(0x300+bank)&bit);
    o->irouter=_gic_read64(gicd_base+0x6000+(uint64_t)id*8);
    o->reserved=0;
    return DRE_SPI_OK;
}
int dre_spi_pe_enable(uint32_t id,uint64_t token,int enable)
{
    struct dre_spi_hw hw;
    if((enable!=0&&enable!=1)||dre_spi_summary_live(id,token)||
       dre_spi_pe_read(id,&hw))return DRE_SPI_LOST;
    /* Exactly one owned W1S/W1C bit. Never clear pending/active, change group,
     * priority/trigger/route, or write ICC_IAR/EOIR/DIR or global GIC control. */
    uint32_t bit=UINT32_C(1)<<(id&31),offset=(enable?0x100:0x180)+(id/32)*4;
    gicd_write32(offset,bit);
    __asm__ volatile("dsb sy" ::: "memory");
    uint64_t start=dre_spi_counter();
    for(unsigned poll=0;poll<4096;poll++) {
        uint32_t ctl=gicd_read32(0);
        uint32_t observed=!!(gicd_read32(0x100+(id/32)*4)&bit);
        if(!(ctl&0x80000000)&&observed==(uint32_t)enable)return DRE_SPI_OK;
        if(dre_spi_counter()-start>19200)break; /* max 1ms, also iteration bound */
    }
    return DRE_SPI_TIMEOUT;
}
#endif
