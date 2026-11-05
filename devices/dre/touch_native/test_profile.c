/* SPDX-License-Identifier: MIT -- optional private captured-FDT read-only
 * qualification. Never writes/prints the FDT, firmware or device identifiers. */
#include "native.h"
#include "qup.h"
#include "smmu.h"
#include "tlmm.h"
#include <libfdt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(e) do { ++checks;if(!(e)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#e);exit(1);} } while(0)
static unsigned checks;
static void replace(void *f,const char *node,const char *key,unsigned cell,uint32_t value)
{
    int bytes;const fdt32_t *p=fdt_getprop(f,fdt_path_offset(f,node),key,&bytes);
    CHECK(p&&bytes>=(int)(cell+1)*4);((fdt32_t *)(uintptr_t)p)[cell]=cpu_to_fdt32(value);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);FILE *file=fopen(argv[1],"rb");CHECK(file);
    CHECK(!fseek(file,0,SEEK_END));long bytes=ftell(file);CHECK(bytes>=40&&bytes<=2*1024*1024);
    rewind(file);void *original=malloc((size_t)bytes),*f=malloc((size_t)bytes+4);CHECK(original&&f);
    CHECK(fread(original,1,(size_t)bytes,file)==(size_t)bytes);fclose(file);
    struct dre_tn_profile p,sentinel;struct dre_tn_smmu_profile s;struct dre_tn_tlmm_profile t;
    CHECK(dre_tn_profile_from_fdt(original,(size_t)bytes,&p)==DRE_TN_OK);
    CHECK(dre_tn_qup_profile_from_fdt(original,(size_t)bytes)==DRE_TN_OK);
    CHECK(dre_tn_smmu_profile_from_fdt(original,(size_t)bytes,&s)==DRE_TN_OK);
    CHECK(dre_tn_tlmm_profile_from_fdt(original,(size_t)bytes,&t)==DRE_TN_OK);
    CHECK(p.stream_id==0x16&&p.irq_gpio==22&&p.width==1080&&p.height==2400);
    CHECK(s.sid==0x16&&t.parent_intid==259&&t.wake_intid==229);
    const struct {const char *node,*property;unsigned cell;uint32_t value;} cases[]={
        {"/soc/qcom,gpi-dma@4a00000","iommus",1,0x17},
        {"/soc/qcom,gpi-dma@4a00000","interrupts",1,336},
        {"/soc/spi@4a80000","dmas",2,1},
        {"/soc/spi@4a80000/novatek@0","cs-gpio",1,4},
        {"/soc/spi@4a80000/novatek@0","spi-max-frequency",0,24000000}
    };
    memset(&sentinel,0xa5,sizeof(sentinel));
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        memcpy(f,original,(size_t)bytes);p=sentinel;
        replace(f,cases[i].node,cases[i].property,cases[i].cell,cases[i].value);
        CHECK(dre_tn_profile_from_fdt(f,(size_t)bytes,&p)!=DRE_TN_OK&&!memcmp(&p,&sentinel,sizeof(p)));
    }
    memcpy(f,original,(size_t)bytes);replace(f,"/soc/spi@4a80000","clocks",1,116);
    CHECK(dre_tn_qup_profile_from_fdt(f,(size_t)bytes)!=DRE_TN_OK);
    memcpy(f,original,(size_t)bytes);replace(f,"/soc/pinctrl@400000","interrupts",1,228);
    CHECK(dre_tn_tlmm_profile_from_fdt(f,(size_t)bytes,&t)!=DRE_TN_OK);
    memcpy(f,original,(size_t)bytes);((struct fdt_header *)f)->magic=0;p=sentinel;
    CHECK(dre_tn_profile_from_fdt(f,(size_t)bytes,&p)!=DRE_TN_OK&&!memcmp(&p,&sentinel,sizeof(p)));
    memcpy(f,original,(size_t)bytes);p=sentinel;
    CHECK(dre_tn_profile_from_fdt(f,(size_t)bytes-1,&p)!=DRE_TN_OK&&!memcmp(&p,&sentinel,sizeof(p)));
    memset((uint8_t *)f+bytes,0,4);p=sentinel;
    CHECK(dre_tn_profile_from_fdt(f,(size_t)bytes+4,&p)!=DRE_TN_OK&&!memcmp(&p,&sentinel,sizeof(p)));
    free(f);free(original);printf("PASS %u production private FDT profile checks; metadata only\n",checks);return 0;
}
