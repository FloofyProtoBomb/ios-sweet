/* SPDX-License-Identifier: MIT -- synthetic controller memory, real production
 * firmware/report framing and actual host OpenSSL SHA256. No private blob. */
#include "nt36672c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern unsigned char *SHA256(const unsigned char *,size_t,unsigned char *);
#define CHECK(e) do { ++checks;if(!(e)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#e);exit(1);} } while(0)
static unsigned checks;
struct model { uint8_t memory[0x40000];uint32_t page;unsigned calls,writes,fail_at,wrong_crc,bad_trim; };
static struct model *active;
static void put(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i)); }
static int hash(void *c,const uint8_t *p,size_t n,uint8_t out[32])
{ (void)c;return SHA256(p,n,out)?0:1; }
static void pause_us(void *c,unsigned us) { (void)c;CHECK(us==1000); }
enum dre_tn_result dre_tn_spi_transfer(struct dre_tn_spi *spi,const uint8_t *tx,uint8_t *rx,size_t n)
{
    struct model *m=active;CHECK(spi->state==DRE_TN_RUNNING&&n>=2&&n<=130);
    if(++m->calls==m->fail_at)return DRE_TN_TIMEOUT;
    if(n==3&&tx[0]==0xff){m->page=(uint32_t)tx[1]<<15|(uint32_t)tx[2]<<7;return DRE_TN_OK;}
    uint32_t address=m->page+(tx[0]&127);CHECK(address<sizeof(m->memory));
    if(tx[0]&128) {
        CHECK(n<=129&&(address&127)+n-1<=128&&n-1<=sizeof(m->memory)-address);
        memcpy(m->memory+address,tx+1,n-1);++m->writes;
        if(address==0x3f10d&&tx[1]==1) {
            m->memory[0x22d60]=0xa3;m->memory[0x3f133]=4;
            memcpy(m->memory+0x3f120,m->memory+0x3f100,8);
            if(m->wrong_crc)m->memory[0x3f120]^=1;
        }
    } else {
        CHECK(n>=3&&(address&127)+n-2<=128&&n-2<=sizeof(m->memory)-address);
        CHECK(tx[1]==0);memset(rx,0,n);memcpy(rx+2,m->memory+address,n-2);
    }
    return DRE_TN_OK;
}
static void init(struct model *m,struct dre_tn_spi *spi,uint8_t *fw)
{
    memset(m,0,sizeof(*m));memset(spi,0,sizeof(*spi));memset(fw,0,139264);active=m;
    put(fw,0x30);put(fw+4,0x10000);put(fw+8,191);put(fw+12,0x180);
    put(fw+16,0x20000);put(fw+20,95);put(fw+24,0x12345678);put(fw+28,0x87654321);
    for(unsigned i=0x30;i<0x180;i++)fw[i]=(uint8_t)(i*3);
    for(unsigned i=0x180;i<0x200;i++)fw[i]=(uint8_t)(i*7);
    fw[139264-4096]=9;fw[139264-4096+1]=246;memcpy(fw+139264-3,"NVT",3);
    m->memory[0x1f64e]=0x0c;m->memory[0x1f651]=0x72;m->memory[0x1f652]=0x66;m->memory[0x1f653]=3;
    m->memory[0x22d78]=9;m->memory[0x22d79]=246;
    spi->state=DRE_TN_RUNNING;spi->profile.width=1080;spi->profile.height=2400;
    spi->profile.firmware_size=139264;CHECK(!hash(0,fw,139264,spi->profile.firmware_sha256));
    spi->ops.delay_us=pause_us;
}
int main(void)
{
    struct model *m=malloc(sizeof(*m));uint8_t *fw=malloc(139264);CHECK(m&&fw);
    struct dre_tn_spi spi;struct dre_tn_controller c={0};init(m,&spi,fw);
    CHECK(dre_tn_controller_boot(&c,&spi,fw,139264,hash,0)==DRE_TN_OK);
    CHECK(c.state==DRE_TN_CONTROLLER_READY&&c.generation==1&&c.firmware_version==9);
    CHECK(!memcmp(m->memory+0x10000,fw+0x30,192)&&!memcmp(m->memory+0x20000,fw+0x180,96));
    CHECK(m->memory[0x3f118]==191&&m->memory[0x3f130]==95);
    uint8_t *report=m->memory+0x22d00;memset(report,0,65);
    report[0]=9;report[1]=1;report[2]=2;report[3]=0x34;report[4]=1;report[5]=20;
    unsigned sum=0;for(unsigned i=0;i<64;i++)sum+=report[i];report[64]=(uint8_t)-sum;
    struct dre_touch_frame out;CHECK(dre_tn_controller_report(&c,&out)==DRE_TN_OK&&out.active_mask==1);
    struct dre_touch_frame before=out;report[64]^=1;
    CHECK(dre_tn_controller_report(&c,&out)==DRE_TN_CHECKSUM&&!memcmp(&out,&before,sizeof(out)));
    memset(report,0xfd,6);
    for(unsigned i=0;i<4;i++)CHECK(dre_tn_controller_report(&c,&out)==DRE_TN_RECOVERY);
    CHECK(c.state==DRE_TN_CONTROLLER_FAULT&&!memcmp(&out,&before,sizeof(out)));
    init(m,&spi,fw);c=(struct dre_tn_controller){0};fw[100]^=1;
    CHECK(dre_tn_controller_boot(&c,&spi,fw,139264,hash,0)==DRE_TN_FIRMWARE&&!m->calls);
    init(m,&spi,fw);c=(struct dre_tn_controller){0};put(fw+4,0x3f000);hash(0,fw,139264,spi.profile.firmware_sha256);
    CHECK(dre_tn_controller_boot(&c,&spi,fw,139264,hash,0)==DRE_TN_FIRMWARE&&!m->calls);
    init(m,&spi,fw);c=(struct dre_tn_controller){0};m->memory[0x1f64e]=0;
    CHECK(dre_tn_controller_boot(&c,&spi,fw,139264,hash,0)==DRE_TN_PROFILE&&c.state==DRE_TN_CONTROLLER_FAULT);
    init(m,&spi,fw);c=(struct dre_tn_controller){0};m->wrong_crc=1;
    CHECK(dre_tn_controller_boot(&c,&spi,fw,139264,hash,0)==DRE_TN_FIRMWARE&&c.state==DRE_TN_CONTROLLER_FAULT);
    for(unsigned at=1;at<=12;at++) {
        init(m,&spi,fw);c=(struct dre_tn_controller){0};m->fail_at=at;
        CHECK(dre_tn_controller_boot(&c,&spi,fw,139264,hash,0)==DRE_TN_TIMEOUT);
        CHECK(c.state==DRE_TN_CONTROLLER_FAULT&&m->calls==at);
    }
    free(fw);free(m);printf("PASS %u production controller wire/firmware checks\n",checks);return 0;
}
