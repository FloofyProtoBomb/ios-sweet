/* SPDX-License-Identifier: MIT
 * QCOM_MDT_RELOCATABLE + PAS_MEM_SETUP protocol, pinned Lineage98a3878.
 * Pure validation; no physical read, mapping, SCM, firmware edit or device IO.
 */
#include "relocation.h"
#include "../../storage_abi.h"
static uint32_t word(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int equal(const char *a,const char *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static int low(uint64_t p,uint64_t n)
{return p>=UINT64_C(0x80000000)&&p<UINT64_C(0x100000000)&&n&&n<=UINT64_C(0x100000000)-p;}
static int inside(uint64_t p,uint64_t n,uint64_t b,uint64_t m)
{return n&&p>=b&&p-b<=m&&n<=m-(p-b);}
static int overlap(uint64_t p,uint64_t n,uint64_t b,uint64_t m)
{return n&&m&&p<b+m&&b<p+n;}
int dre_gpu_fw_relocation_slice(uint64_t storage,uint64_t bytes,uint64_t image,
    uint64_t image_bytes,uint64_t *load)
{
    if(!load||bytes!=DRE_GPU_FW_RELOCATION_STORAGE_BYTES||
       !low(storage,bytes)||!low(image,image_bytes)||
       (storage&(DRE_GPU_FW_ALIGNMENT-1))||!inside(storage,bytes,image,image_bytes))return 0;
    uint64_t chosen=(storage+DRE_GPU_FW_RELOCATION_ALIGNMENT-1)&
        ~((uint64_t)DRE_GPU_FW_RELOCATION_ALIGNMENT-1);
    if(!inside(chosen,DRE_GPU_FW_RELOCATION_BYTES,storage,bytes))return 0;
    *load=chosen;return 1;
}
const char *dre_gpu_fw_relocation_plan(const void *file,size_t bytes,
    uint64_t load,size_t load_bytes,uint64_t metadata,size_t metadata_bytes,
    struct dre_gpu_fw_relocation *out)
{
    if(!file||!out||(uintptr_t)file>UINTPTR_MAX-bytes||
       (uintptr_t)out>UINTPTR_MAX-sizeof(*out)||
       overlap((uintptr_t)file,bytes,(uintptr_t)out,sizeof(*out))||
       !low(load,load_bytes)||!low(metadata,metadata_bytes)||
       load_bytes!=DRE_GPU_FW_RELOCATION_BYTES||metadata_bytes!=DRE_GPU_FW_METADATA_BYTES||
       (load&(DRE_GPU_FW_RELOCATION_ALIGNMENT-1))||
       ((metadata|metadata_bytes)&(DRE_GPU_FW_ALIGNMENT-1))||
       overlap(load,load_bytes,metadata,metadata_bytes))return "FW_RELOCATION_DESTINATION_REJECTED";
    struct dre_gpu_fw_relocation p={0};
    const char *status=dre_a619_zap_plan(file,bytes,load,load_bytes,&p.zap);
    if(!equal(status,"ZAP_PLAN_OK"))return status;
    /* The strict existing parser proves every loadable segment relocatable.
     * PIL's actual relocated entry must additionally remain in such a segment.
     * Hash/header records are never destination contents or executable entry. */
    const uint8_t *f=file;
    uint32_t entry=word(f+24),minimum=UINT32_MAX;
    unsigned count=(unsigned)f[44]|(unsigned)f[45]<<8,entry_found=0;
    for(unsigned i=1;i<count;i++) {
        const uint8_t *h=f+52+i*32;
        if(word(h)!=1||(word(h+24)&(7u<<24))==(2u<<24))continue;
        uint32_t pa=word(h+12),n=word(h+20);
        if(pa<minimum)minimum=pa;
        if(entry>=pa&&entry-pa<n)entry_found++;
    }
    if(entry_found!=1||minimum==UINT32_MAX)return "FW_RELOCATION_ENTRY_NOT_LOADABLE";
    p.load_pa=load;p.load_bytes=load_bytes;p.metadata_pa=metadata;p.metadata_bytes=metadata_bytes;
    p.entry_offset=entry-minimum;
    *out=p;return "FW_RELOCATION_STRUCTURE_ONLY";
}
int dre_gpu_fw_relocation_handoff_valid(const uint64_t w[12],const uint64_t storage[8])
{
    if(!w||!storage||!dre_storage_abi_valid(storage)||w[0]!=DRE_GPU_FW_HANDOFF_MAGIC||
       w[1]!=(UINT64_C(96)<<32|2)||w[6]!=DRE_GPU_FW_CHIP_ID||w[10]||
       w[11]!=dre_storage_crc(w,88)||!w[3]||w[3]>DRE_GPU_FW_PACKAGE_MAX||
       w[5]!=DRE_GPU_FW_METADATA_BYTES||w[8]!=DRE_GPU_FW_RELOCATION_BYTES||
       w[9]!=DRE_GPU_FW_RELOCATION_ALIGNMENT||(w[7]&(w[9]-1))||
       ((w[2]|w[3]|w[4]|w[5]|w[7]|w[8])&(DRE_GPU_FW_ALIGNMENT-1))||
       !low(w[2],w[3])||!low(w[4],w[5])||!low(w[7],w[8]))return 0;
    const uint64_t base[]={w[2],w[4],w[7]},size[]={w[3],w[5],w[8]};
    for(unsigned i=0;i<3;i++) {
        if(!inside(base[i],size[i],storage[5],storage[6])||
           overlap(base[i],size[i],storage[1],storage[2])||
           overlap(base[i],size[i],storage[3],storage[4]))return 0;
        for(unsigned j=0;j<i;j++)if(overlap(base[i],size[i],base[j],size[j]))return 0;
    }
    return 1;
}
