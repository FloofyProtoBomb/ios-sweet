/* SPDX-License-Identifier: MIT -- controlled host register/DMA model only. */
#include "gpi_spi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(e) do { ++checks;if(!(e)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#e);exit(1);} } while(0)
static unsigned checks;
struct model {
    struct dre_tn_spi spi;
    uint32_t regs[0x24000/4];
    struct dre_tn_memory allocated[5];
    unsigned allocations,frees,releases,writes,doorbells,fail_write,fail_verify;
    unsigned rx_before_tx,producer,omit_rx,bad_pointer,duplicate,stale_ack,no_ack;
    uint64_t tick;
};
static const uint64_t ee=UINT64_C(0x049f0000)+2*0x4000;
static int reserve(void *c,const struct dre_tn_profile *p,void **l,unsigned *g)
{ CHECK(p->stream_id==0x16);*l=c;*g=2;return 0; }
static int lease(void *c,void *l) { return c!=l; }
static int clock_get(void *c,void *l,struct dre_tn_clock *clock)
{ CHECK(c==l);*clock=(struct dre_tn_clock){1,2,19200000};return 0; }
static int allocate(void *c,void *l,size_t bytes,size_t align,struct dre_tn_memory *out)
{
    struct model *m=c;CHECK(c==l&&m->allocations<5&&align<=4096);
    void *cpu=aligned_alloc(4096,4096);CHECK(cpu);memset(cpu,0xa5,4096);
    *out=(struct dre_tn_memory){cpu,0x100000+(uint64_t)m->allocations*0x4000,bytes,cpu};
    m->allocated[m->allocations++]=*out;return 0;
}
static int verify(void *c,void *l,const struct dre_tn_memory *memory,unsigned sid)
{
    struct model *m=c;CHECK(c==l&&sid==0x16&&memory->cpu&&memory->allocation);
    return m->fail_verify&&m->allocations==m->fail_verify;
}
static int release_mem(void *c,void *l,struct dre_tn_memory *memory)
{ struct model *m=c;CHECK(c==l&&memory->allocation);free(memory->allocation);memory->allocation=0;++m->frees;return 0; }
static int read32(void *c,void *l,uint64_t a,uint32_t *v)
{
    struct model *m=c;CHECK(c==l);
    if(a>=0x04a80000&&a<0x04a84000) {
        switch((unsigned)(a-0x04a80000)) {
        case 0x7c:*v=1;return 0;case 0x48:*v=0x21;return 0;
        case 0x68:*v=0x100;return 0;case 0x64:*v=1;return 0;
        case 0x40:case 0x610:*v=0;return 0;default:CHECK(0);
        }
    }
    CHECK(a>=ee&&a-ee<sizeof(m->regs)&&!(a&3));unsigned o=(unsigned)(a-ee);
    if(o==0x23080)*v=(m->regs[0x23090/4]?1u:0)|(m->regs[0x23094/4]?2u:0);
    else if(o==0x21010)*v=(uint32_t)m->allocated[DRE_TN_EVENT_RING].iova+m->producer;
    else *v=m->regs[o/4];
    return 0;
}
static void put(uint8_t *p,uint32_t value)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(value>>(8*i)); }
static void completion(struct model *m,unsigned ch,uint32_t pointer,unsigned bytes)
{
    uint8_t *p=(uint8_t *)m->allocated[DRE_TN_EVENT_RING].cpu+m->producer;
    put(p,pointer);put(p+4,0);put(p+8,0x02000000u|bytes);put(p+12,ch<<24|0x22u<<16);
    m->producer=(m->producer+16)&4095;
}
static int write32(void *c,void *l,uint64_t a,uint32_t v)
{
    struct model *m=c;CHECK(c==l&&a>=ee&&a-ee<sizeof(m->regs)&&!(a&3));
    unsigned o=(unsigned)(a-ee);++m->writes;
    if(o==0x23008||o==0x23010) {
        unsigned ch=v&255,op=v>>24,event=o==0x23010;CHECK(ch<2);
        unsigned state=op==0?1:op==1?2:op==2?3:0;
        unsigned ctx=event?0x21000:0x20000+ch*0x80;
        m->regs[ctx/4]=(m->regs[ctx/4]&~0xf00000u)|(state<<20);
        if(!m->no_ack)m->regs[(event?0x23094:0x23090)/4]|=event?1u:1u<<ch;
    } else if(o==0x230a0)m->regs[0x23090/4]&=~v;
    else if(o==0x230a4)m->regs[0x23094/4]&=~v;
    else if(o==0x22008&&m->spi.state==DRE_TN_INFLIGHT) { ++m->doorbells;m->rx_before_tx=1; }
    else if(o==0x22000&&m->spi.state==DRE_TN_INFLIGHT) {
        ++m->doorbells;CHECK(m->rx_before_tx);m->rx_before_tx=0;
        unsigned bytes=m->spi.transfer_bytes;
        for(unsigned i=0;i<bytes;i++)m->allocated[DRE_TN_RX_BYTES].cpu[i]=m->allocated[DRE_TN_TX_BYTES].cpu[i]^0x5a;
        uint32_t tx=(uint32_t)m->allocated[DRE_TN_TX_RING].iova+((m->spi.tx_wp+1024-16)&1023);
        uint32_t rx=(uint32_t)m->allocated[DRE_TN_RX_RING].iova+((m->spi.rx_wp+1024-16)&1023);
        completion(m,0,tx+(m->bad_pointer?16:0),bytes);
        if(m->duplicate)completion(m,0,tx,bytes);
        if(!m->omit_rx)completion(m,1,rx,bytes);
    } else m->regs[o/4]=v;
    return m->fail_write==m->writes; /* A failing posted store already happened. */
}
static void barrier(void *c,enum dre_tn_barrier b) { (void)c;(void)b; }
static uint64_t ticks(void *c) { return ((struct model *)c)->tick+=19200; }
static uint64_t hz(void *c) { (void)c;return 19200000; }
static void pause_us(void *c,unsigned us) { ((struct model *)c)->tick+=(uint64_t)us*192/10; }
static int mask_irq(void *c,void *l) { CHECK(c==l);return 0; }
static int release(void *c,void *l) { CHECK(c==l);++((struct model *)c)->releases;return 0; }
static enum dre_tn_result open_model(struct model *m)
{
    struct dre_tn_profile p={.spi_base=0x4a80000,.gpi_base=0x4a00000,.gpi_ee_base=0x49f0000,
        .iova_base=0x100000,.iova_size=0x100000,.gpi_irq={367,368,369,370,371},.spi_irq=359,
        .gpi_mask=31,.event_factor=2,.stream_id=0x16,.irq_gpio=22,.reset_gpio=21,.cs_gpio=3,
        .width=1080,.height=2400,.max_hz=12000000};
    struct dre_tn_ops ops={m,reserve,lease,clock_get,allocate,verify,release_mem,read32,write32,
        barrier,ticks,hz,pause_us,mask_irq,release};
    return dre_tn_spi_open(&m->spi,&p,&ops);
}
static void dispose(struct model *m)
{
    /* Host fixture process owns its synthetic heap after testing retained
     * production lifetime. This is not a production DMA retirement/free. */
    for(unsigned i=m->frees;i<m->allocations;i++)free(m->allocated[i].allocation);
}
int main(void)
{
    struct model *m=calloc(1,sizeof(*m));CHECK(m);CHECK(open_model(m)==DRE_TN_OK);
    CHECK(m->spi.gpii==2&&m->spi.blocks==5&&m->spi.allocated_channels==3&&m->spi.event_allocated);
    for(unsigned n=1;n<=70;n++) {
        uint8_t tx[31],rx[31];for(unsigned i=0;i<31;i++)tx[i]=(uint8_t)(n+i);
        memset(rx,0xa5,sizeof(rx));CHECK(dre_tn_spi_transfer(&m->spi,tx,rx,sizeof(tx))==DRE_TN_OK);
        for(unsigned i=0;i<31;i++)CHECK(rx[i]==(uint8_t)(tx[i]^0x5a));
    }
    CHECK(m->doorbells==140&&m->spi.state==DRE_TN_RUNNING);
    CHECK(dre_tn_spi_close(&m->spi)==DRE_TN_OK&&m->frees==5&&m->releases==1);free(m);
    for(unsigned mode=0;mode<4;mode++) {
        m=calloc(1,sizeof(*m));CHECK(m&&open_model(m)==DRE_TN_OK);
        if(mode==0)m->omit_rx=1;
        if(mode==1)m->bad_pointer=1;
        if(mode==2)m->duplicate=1;
        if(mode==3)m->fail_write=m->writes+2;
        uint8_t tx[17]={0},rx[17];memset(rx,0xa5,sizeof(rx));
        CHECK(dre_tn_spi_transfer(&m->spi,tx,rx,sizeof(tx))!=DRE_TN_OK);
        CHECK(m->spi.state==DRE_TN_QUARANTINED&&m->spi.blocks==5&&!m->frees&&!m->releases);
        for(unsigned i=0;i<sizeof(rx);i++)CHECK(rx[i]==0xa5);
        CHECK(dre_tn_spi_close(&m->spi)==DRE_TN_POISONED&&!m->frees);
        dispose(m);free(m);
    }
    for(unsigned fail=1;fail<=5;fail++) {
        m=calloc(1,sizeof(*m));CHECK(m);m->fail_verify=fail;
        CHECK(open_model(m)==DRE_TN_DMA);CHECK(!m->writes&&m->frees==fail&&m->releases==1);
        free(m);
    }
    m=calloc(1,sizeof(*m));CHECK(m);m->fail_write=1;
    CHECK(open_model(m)!=DRE_TN_OK&&m->spi.state==DRE_TN_QUARANTINED);
    CHECK(m->spi.blocks==5&&!m->frees&&!m->releases);dispose(m);free(m);
    m=calloc(1,sizeof(*m));CHECK(m);m->regs[0x23090/4]=1;
    CHECK(open_model(m)==DRE_TN_OWNERSHIP&&!m->writes&&m->frees==5&&m->releases==1);free(m);
    m=calloc(1,sizeof(*m));CHECK(m);m->no_ack=1;
    CHECK(open_model(m)==DRE_TN_TIMEOUT&&m->spi.state==DRE_TN_QUARANTINED);
    CHECK(m->spi.blocks==5&&!m->frees&&!m->releases);dispose(m);free(m);
    printf("PASS %u production GPI/SPI model checks\n",checks);return 0;
}
