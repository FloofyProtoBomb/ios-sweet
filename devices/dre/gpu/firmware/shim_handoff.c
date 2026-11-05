/* SPDX-License-Identifier: MIT -- UNVALIDATED source; no device writes.
 * Transfers an immutable package and separate low-DDR SCM metadata arena.
 * Never writes GPU microcode RAM, IPA memory, SCM, clocks or GPU registers. */
#include <libfdt.h>
#include "shim_handoff.h"
#include "package.h"
#if defined(DRE_GPU_FW_RELOCATED) && DRE_GPU_FW_RELOCATED
#include "relocation.h"
#define HANDOFF_WORDS 12u
/* Origin0 linker alignment cannot prove physical alignment after Android
 * loads the image. Retain actual initialized bytes and choose a physical
 * aligned slice INSIDE this dedicated object at runtime. .data.* is included
 * by the selected image.ld, before bss/stack/payload. Never use a NOLOAD area. */
static uint8_t load_storage[DRE_GPU_FW_RELOCATION_STORAGE_BYTES]
    __attribute__((used,section(".data.dre_gpu_fw_load"),aligned(DRE_GPU_FW_ALIGNMENT)))={1};
#else
#define HANDOFF_WORDS 8u
#endif
#include "../../storage_live.h"
#include "../../ufs_read.h"
extern const char _head[],_end[],_dre_gpu_firmware_start[],_dre_gpu_firmware_end[];
static uint8_t metadata[DRE_GPU_FW_METADATA_BYTES] __attribute__((aligned(DRE_GPU_FW_ALIGNMENT)));
static uint64_t handoff[HANDOFF_WORDS],prepared_storage[8];
static int prepared;
static int equal(const char *a,const char *b)
{ while(*a&&*a==*b){a++;b++;}return *a==*b; }
const char *dre_gpu_fw_prepare(const void *dt)
{
    prepared=0;
    uint64_t image=(uintptr_t)_head,image_bytes=(uintptr_t)_end-image;
    uint64_t pa=(uintptr_t)_dre_gpu_firmware_start;
    uint64_t bytes=(uintptr_t)_dre_gpu_firmware_end-pa;
    uint64_t w[8]={DRE_STORAGE_MAGIC,dre_ufs_probe_arena(),dre_ufs_probe_size(),
        (uintptr_t)dt,fdt_totalsize(dt),image,image_bytes,0};
    w[7]=dre_storage_crc(w,56);
    handoff[0]=DRE_GPU_FW_HANDOFF_MAGIC;handoff[1]=(UINT64_C(64)<<32)|1;
    handoff[2]=pa;handoff[3]=bytes;handoff[4]=(uintptr_t)metadata;
    handoff[5]=sizeof(metadata);handoff[6]=DRE_GPU_FW_CHIP_ID;
    handoff[7]=dre_storage_crc(handoff,56);
#if defined(DRE_GPU_FW_RELOCATED) && DRE_GPU_FW_RELOCATED
    uint64_t start=(uintptr_t)load_storage,length=sizeof(load_storage);
    uint64_t chosen=0;
    if(!dre_gpu_fw_relocation_slice(start,length,image,image_bytes,&chosen)||
       !dre_dma_boot_owned_ram_allowed(dt,start,length)||
       !dre_dma_boot_owned_ram_allowed(dt,chosen,DRE_GPU_FW_RELOCATION_BYTES))
        return "GPU relocated firmware storage ownership unavailable";
    handoff[1]=(UINT64_C(96)<<32)|2;handoff[7]=chosen;
    handoff[8]=DRE_GPU_FW_RELOCATION_BYTES;handoff[9]=DRE_GPU_FW_RELOCATION_ALIGNMENT;
    handoff[10]=0;handoff[11]=dre_storage_crc(handoff,88);
    if(!dre_storage_live_valid(dt,w)||!dre_gpu_fw_relocation_handoff_valid(handoff,w)||
       !dre_dma_boot_owned_ram_allowed(dt,pa,bytes)||
       !dre_dma_boot_owned_ram_allowed(dt,(uintptr_t)metadata,sizeof(metadata)))
        return "GPU relocated firmware boot ownership unavailable";
#else
    if(!dre_storage_live_valid(dt,w)||!dre_gpu_fw_handoff_valid(handoff,w)||
       !dre_dma_boot_owned_ram_allowed(dt,pa,bytes)||
       !dre_dma_boot_owned_ram_allowed(dt,(uintptr_t)metadata,sizeof(metadata)))
        return "GPU firmware boot ownership unavailable";
#endif
    struct dre_gpu_fw_package view;
    if(!equal(dre_gpu_fw_parse((const void *)(uintptr_t)pa,bytes,&view),"FW_STRUCTURE_ONLY"))
        return "GPU firmware package structure rejected";
    for(unsigned i=0;i<8;i++)prepared_storage[i]=w[i];
    prepared=1;return 0;
}
static uint32_t le32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int walk(uint8_t *adt,uint32_t size,uint32_t *off,unsigned depth,
    unsigned *nodes,uint8_t **slot)
{
    if(depth>16||++*nodes>256||*off>size||size-*off<8)return 0;
    unsigned props=le32(adt+*off),children=le32(adt+*off+4);*off+=8;
    if(props>256||children>256)return 0;
    uint8_t *name=0,*value=0;
    for(unsigned i=0;i<props;i++) {
        if(size-*off<36)return 0;
        uint8_t *key=adt+*off;
        if(!memchr(key,0,32))return 0;
        uint32_t len=le32(key+32)&0x7fffffff;*off+=36;
        uint32_t aligned=(len+3)&~3u;
        if(aligned>size-*off)return 0;
        if(!strcmp((const char *)key,"name")) {
            if(name||!len||!memchr(adt+*off,0,len))return 0;name=adt+*off;
        }
        if(!strcmp((const char *)key,"dre,firmware-handoff")) {
            if(value||len!=sizeof(handoff))return 0;value=adt+*off;
        }
        *off+=aligned;
    }
    if(!name)return 0;
    if(value) {
        if(depth!=1||strcmp((const char *)name,"dre-gpu-firmware")||*slot)return 0;
        *slot=value;
    }
    for(unsigned i=0;i<children;i++)if(!walk(adt,size,off,depth+1,nodes,slot))return 0;
    return 1;
}
const char *dre_gpu_fw_fixup(void *adt,uint32_t size)
{
    uint32_t off=0;unsigned nodes=0;uint8_t *slot=0;
    if(!prepared||
#if defined(DRE_GPU_FW_RELOCATED) && DRE_GPU_FW_RELOCATED
       !dre_gpu_fw_relocation_handoff_valid(handoff,prepared_storage)||
#else
       !dre_gpu_fw_handoff_valid(handoff,prepared_storage)||
#endif
       !walk(adt,size,&off,0,&nodes,&slot)||off!=size||!slot)
        return "GPU firmware ADT handoff rejected";
    memcpy(slot,handoff,sizeof(handoff));return 0;
}
