/* SPDX-License-Identifier: MIT -- synthetic records and ELF, no firmware. */
#include "relocation.h"
#include "../a619_pas13.h"
#include "../../storage_abi.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t file[512];
static _Alignas(16384) uint8_t metadata[65536],destination[16384];
struct scm_fixture {
    struct dre_a619_pas13 state;
    uint64_t ticks;
    unsigned fail_service,fail_transport,fail_shutdown,calls;
};
static int scm_guard(void *cookie){(void)cookie;return 1;}
static void scm_barrier(void *cookie){(void)cookie;}
static uint64_t scm_now(void *cookie){return ++((struct scm_fixture *)cookie)->ticks;}
static int scm_call(void *cookie,struct dre_a619_smc_regs *r)
{
    struct scm_fixture *f=cookie;unsigned command=(unsigned)r->x[0]&255;
    assert(r->x[2]==13&&++f->calls<=5);
    if(command==1)assert(r->x[3]==0x90100000);
    if(command==2)assert(r->x[3]==0x90000000&&r->x[4]==4096);
    if(command==1||command==2)assert(destination[0]==0xa5); /* Before any code copy. */
    if(command==5)assert(!memcmp(destination,file+256,16));
    if(command==f->fail_transport)return 0;
    r->x[0]=0;r->x[1]=command==7?1u:
        command==f->fail_service||(command==6&&f->fail_shutdown)?99u:0u;
    return 1;
}
static void scm_init(struct scm_fixture *f)
{
    memset(f,0,sizeof(*f));memset(metadata,0xcc,sizeof(metadata));memset(destination,0xa5,sizeof(destination));
    f->state=(struct dre_a619_pas13){.chip_id=DRE_GPU_FW_CHIP_ID,
        .prerequisites=DRE_A619_PAS13_PREREQUISITES,.tick_hz=19200000,
        .firmware=file,.firmware_bytes=sizeof(file),.metadata_cpu=metadata,
        .metadata_pa=0x90100000,.metadata_capacity=sizeof(metadata),
        .reserved_cpu=destination,.reserved_pa=0x90000000,.reserved_bytes=sizeof(destination)};
}
static const char *scm_auth(struct scm_fixture *f)
{
    struct dre_a619_pas13_ops ops={scm_guard,scm_call,scm_barrier,scm_now,f};
    return dre_a619_pas13_authenticate(&f->state,&ops);
}
static void word(unsigned off,uint32_t n)
{for(unsigned i=0;i<4;i++)file[off+i]=(uint8_t)(n>>(i*8));}
static void half(unsigned off,uint16_t n){file[off]=(uint8_t)n;file[off+1]=(uint8_t)(n>>8);}
static void init(void)
{
    memset(file,0,sizeof(file));memcpy(file,"\177ELF\1\1\1",7);
    half(16,2);half(18,164);word(20,1);word(24,0x5000);word(28,52);
    half(40,52);half(42,32);half(44,3);
    word(52+16,148);word(52+24,0x07000000);
    word(84+4,160);word(84+12,0x6000);word(84+16,32);word(84+20,4096);
    word(84+24,0x02200000);word(84+28,4096);
    word(116,1);word(116+4,256);word(116+12,0x5000);word(116+16,16);
    word(116+20,64);word(116+24,0x08000007);word(116+28,0x100000);
}
static const char *plan(struct dre_gpu_fw_relocation *p)
{return dre_gpu_fw_relocation_plan(file,sizeof(file),0x90000000,16384,0x90100000,65536,p);}
static void records(uint64_t w[12],uint64_t s[8])
{
    const uint64_t storage[]={DRE_STORAGE_MAGIC,0x90200000,0x4000,0x92000000,0x8000,0x90000000,0x1000000,0};
    const uint64_t firmware[]={DRE_GPU_FW_HANDOFF_MAGIC,(UINT64_C(96)<<32)|2,0x90400000,0x10000,
        0x90100000,65536,DRE_GPU_FW_CHIP_ID,0x90000000,16384,1048576,0,0};
    memcpy(w,firmware,sizeof(firmware));memcpy(s,storage,sizeof(storage));
    s[7]=dre_storage_crc(s,56);w[11]=dre_storage_crc(w,88);
}
int main(int argc,char **argv)
{
    unsigned cases=0;struct dre_gpu_fw_relocation p;uint64_t w[12],s[8];
    /* Android load-head offsets: section-relative alignment alone is false. */
    for(uint64_t offset=0;offset<1048576;offset+=16384) {
        uint64_t load=0,storage=UINT64_C(0x90000000)+offset;
        assert(dre_gpu_fw_relocation_slice(storage,DRE_GPU_FW_RELOCATION_STORAGE_BYTES,
            0x90000000,0x1000000,&load)==1);
        assert(!(load&1048575)&&load>=storage&&load-storage<=1048576&&
            load+16384<=storage+DRE_GPU_FW_RELOCATION_STORAGE_BYTES);cases++;
    }
    uint64_t load=123;
    assert(!dre_gpu_fw_relocation_slice(0x90080000,DRE_GPU_FW_RELOCATION_STORAGE_BYTES,
        0x90080000,16384,&load)&&load==123);cases++;
    assert(!dre_gpu_fw_relocation_slice(UINT64_MAX-16383,DRE_GPU_FW_RELOCATION_STORAGE_BYTES,
        0x90000000,0x1000000,&load)&&load==123);cases++;
#define EXPECT(t) do{assert(!strcmp(plan(&p),(t)));cases++;}while(0)
    init();EXPECT("FW_RELOCATION_STRUCTURE_ONLY");
    assert(p.entry_offset==0&&p.zap.memory_bytes==4096&&p.zap.alignment==1048576);
    init();word(24,0x503f);EXPECT("FW_RELOCATION_STRUCTURE_ONLY");assert(p.entry_offset==63);
    init();word(24,0x5040);EXPECT("FW_RELOCATION_ENTRY_NOT_LOADABLE");
    init();word(24,0x6000);EXPECT("FW_RELOCATION_ENTRY_NOT_LOADABLE");
    init();word(116+24,7);EXPECT("INVALID_LOAD_SEGMENT");
    init();word(116+12,0xfffff000);word(116+20,0x2000);EXPECT("INVALID_LOAD_SEGMENT");
    init();word(116+28,0x200000); /* A target aligned1MiB but not2MiB must refuse. */
    assert(!strcmp(dre_gpu_fw_relocation_plan(file,sizeof(file),0x90100000,16384,0x90300000,65536,&p),"RESERVED_ALIGNMENT"));cases++;
    init();word(116+20,0x5000);EXPECT("INVALID_IMAGE_SPAN");
    init();
    const uint64_t bad_load[]={0,0x7ff00000,0x90004000,UINT64_C(0x100000000),UINT64_MAX-3};
    for(unsigned i=0;i<sizeof(bad_load)/sizeof(*bad_load);i++) {
        assert(!strcmp(dre_gpu_fw_relocation_plan(file,sizeof(file),bad_load[i],16384,0x90100000,65536,&p),"FW_RELOCATION_DESTINATION_REJECTED"));cases++;
    }
    assert(!strcmp(dre_gpu_fw_relocation_plan(file,sizeof(file),0x90000000,16384,0x90000000,65536,&p),"FW_RELOCATION_DESTINATION_REJECTED"));cases++;
    assert(!strcmp(dre_gpu_fw_relocation_plan(file,sizeof(file),0x90000000,16384,0x90100000,65536,(void *)file),"FW_RELOCATION_DESTINATION_REJECTED"));cases++;
    records(w,s);assert(dre_gpu_fw_relocation_handoff_valid(w,s)==1);cases++;
    for(unsigned i=0;i<12;i++) {
        records(w,s);w[i]^=1;assert(!dre_gpu_fw_relocation_handoff_valid(w,s));cases++;
    }
    const unsigned coords[]={2,4,7};
    for(unsigned i=0;i<3;i++) {
        records(w,s);w[coords[i]]=0x91000000;w[11]=dre_storage_crc(w,88);
        assert(!dre_gpu_fw_relocation_handoff_valid(w,s));cases++;
    }
    records(w,s);w[2]=w[7];w[11]=dre_storage_crc(w,88);assert(!dre_gpu_fw_relocation_handoff_valid(w,s));cases++;
    records(w,s);w[4]=s[1];w[11]=dre_storage_crc(w,88);assert(!dre_gpu_fw_relocation_handoff_valid(w,s));cases++;
    records(w,s);w[4]=s[3];w[11]=dre_storage_crc(w,88);assert(!dre_gpu_fw_relocation_handoff_valid(w,s));cases++;
    records(w,s);w[7]=UINT64_MAX-16383;w[11]=dre_storage_crc(w,88);assert(!dre_gpu_fw_relocation_handoff_valid(w,s));cases++;
    /* Exercise actual PAS engine with relocated buffers, not a success flag.
     * Every uncertain secure publication retains initialized state and memory. */
    init();for(unsigned i=0;i<16;i++)file[256+i]=(uint8_t)(0x80+i);
    struct scm_fixture scm;
    scm_init(&scm);assert(!strcmp(scm_auth(&scm),"PAS13_AUTHENTICATED"));
    assert(scm.state.initialized&&scm.state.authenticated&&!scm.state.unsafe&&scm.calls==4);cases++;
    const char *failures[]={"PAS_INIT_REJECTED","PAS_MEMORY_SETUP_REJECTED","PAS_AUTH_REJECTED"};
    const unsigned commands[]={1,2,5};
    for(unsigned i=0;i<3;i++) {
        scm_init(&scm);scm.fail_service=commands[i];assert(!strcmp(scm_auth(&scm),failures[i]));
        assert(!scm.state.initialized&&!scm.state.authenticated&&!scm.state.unsafe);
        assert(destination[0]==(i<2?0xa5:0x80));cases++;
        scm_init(&scm);scm.fail_transport=commands[i];assert(!strcmp(scm_auth(&scm),"PAS_RESET_REQUIRED"));
        assert(scm.state.initialized&&!scm.state.authenticated&&scm.state.unsafe);
        assert(destination[0]==(i<2?0xa5:0x80));cases++;
    }
    scm_init(&scm);scm.fail_service=2;scm.fail_shutdown=1;
    assert(!strcmp(scm_auth(&scm),"PAS_RESET_REQUIRED")&&scm.state.initialized&&scm.state.unsafe&&destination[0]==0xa5);cases++;
    printf("PASS %u relocation/entry/owned-handoff refusal cases; no SCM or hardware execution\n",cases);
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);
        assert(n>0&&n<=DRE_GPU_FW_FILE_MAX);rewind(f);void *bytes=malloc((size_t)n);assert(bytes);
        assert(fread(bytes,1,(size_t)n,f)==(size_t)n);assert(!fclose(f));
        assert(!strcmp(dre_gpu_fw_relocation_plan(bytes,(size_t)n,0x90000000,16384,0x90100000,65536,&p),"FW_RELOCATION_STRUCTURE_ONLY"));
        printf("PASS private image structure: span=%u alignment=%u entry_offset=%u; not authentication\n",p.zap.memory_bytes,p.zap.alignment,p.entry_offset);
        free(bytes);
    }else assert(argc==1);
    return 0;
}
