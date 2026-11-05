/* SPDX-License-Identifier: MIT -- UNVALIDATED, no validation executed. */
#include "package.h"
#include "../../storage_abi.h"
static uint32_t u32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const uint8_t *p)
{ return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32; }
static int zero(const uint8_t *p,size_t n)
{ while(n--)if(*p++)return 0;return 1; }
static int same(const uint8_t *a,const uint8_t *b,size_t n)
{ unsigned difference=0;while(n--)difference|=*a++^*b++;return !difference; }
static uint64_t align(uint64_t n)
{ return (n+DRE_GPU_FW_ALIGNMENT-1)&~(uint64_t)(DRE_GPU_FW_ALIGNMENT-1); }
const char *dre_gpu_fw_parse(const void *buffer,size_t bytes,struct dre_gpu_fw_package *out)
{
    if(!buffer||!out||bytes<DRE_GPU_FW_ALIGNMENT||bytes>DRE_GPU_FW_PACKAGE_MAX||
       bytes%DRE_GPU_FW_ALIGNMENT||(uintptr_t)buffer>UINTPTR_MAX-bytes||
       (uintptr_t)out>UINTPTR_MAX-sizeof(*out)||
       ((uintptr_t)out<(uintptr_t)buffer+bytes&&(uintptr_t)buffer<(uintptr_t)out+sizeof(*out)))
        return "FW_INVALID_ARGUMENT";
    const uint8_t *p=buffer;
    if(u64(p)!=DRE_GPU_FW_MAGIC||u32(p+8)!=1||u32(p+12)!=DRE_GPU_FW_HEADER_BYTES||
       u64(p+16)!=bytes||u32(p+24)!=DRE_GPU_FW_CHIP_ID||u32(p+28)!=2||!zero(p+32,32))
        return "FW_HEADER_REJECTED";
    uint64_t offsets[2],sizes[2];
    for(unsigned i=0;i<2;i++) {
        const uint8_t *e=p+64+i*80;
        offsets[i]=u64(e+8);sizes[i]=u64(e+16);
        if(u32(e)!=i+1||u32(e+4)||!sizes[i]||sizes[i]>DRE_GPU_FW_FILE_MAX||
           offsets[i]%DRE_GPU_FW_ALIGNMENT||offsets[i]>bytes||sizes[i]>bytes-offsets[i]||
           zero(e+24,32)||!zero(e+56,24))return "FW_ENTRY_REJECTED";
    }
    if(offsets[0]!=DRE_GPU_FW_ALIGNMENT||offsets[1]!=align(offsets[0]+sizes[0])||
       bytes!=align(offsets[1]+sizes[1])||!zero(p+224,DRE_GPU_FW_ALIGNMENT-224)||
       !zero(p+offsets[0]+sizes[0],offsets[1]-offsets[0]-sizes[0])||
       !zero(p+offsets[1]+sizes[1],bytes-offsets[1]-sizes[1]))return "FW_NONCANONICAL_LAYOUT";
    *out=(struct dre_gpu_fw_package){
        {p,bytes},{p+offsets[0],sizes[0]},{p+offsets[1],sizes[1]},offsets[0],offsets[1]};
    return "FW_STRUCTURE_ONLY";
}
static int equal(const char *a,const char *b)
{ while(*a&&*a==*b){a++;b++;}return *a==*b; }
const char *dre_gpu_fw_verify(const void *buffer,size_t bytes,const struct dre_gpu_fw_trust *trust,
    int (*hash)(void *,const void *,size_t,const uint8_t[32]),void *cookie,
    struct dre_gpu_fw_package *out)
{
    if(!trust||!hash||!out||trust->magic!=DRE_GPU_FW_TRUST_MAGIC||trust->version!=1||
       trust->struct_bytes!=sizeof(*trust)||trust->chip_id!=DRE_GPU_FW_CHIP_ID||trust->flags||
       trust->package_bytes!=bytes||zero(trust->package_sha256,32)||
       zero(trust->sqe_sha256,32)||zero(trust->zap_sha256,32))return "FW_TRUST_ANCHOR_MISSING_OR_INVALID";
    struct dre_gpu_fw_package view;
    const char *status=dre_gpu_fw_parse(buffer,bytes,&view);
    if(!equal(status,"FW_STRUCTURE_ONLY"))return status;
    const uint8_t *p=buffer;
    if(trust->sqe_bytes!=view.sqe.size||trust->zap_bytes!=view.zap.size||
       !same(p+64+24,trust->sqe_sha256,32)||!same(p+144+24,trust->zap_sha256,32)||
       hash(cookie,buffer,bytes,trust->package_sha256)!=1||
       hash(cookie,view.sqe.bytes,view.sqe.size,trust->sqe_sha256)!=1||
       hash(cookie,view.zap.bytes,view.zap.size,trust->zap_sha256)!=1)
        return "FW_PRIVATE_DIGEST_MISMATCH";
    *out=view;return "FW_PRIVATE_DIGESTS_VERIFIED";
}
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{ return n&&m&&a<b+m&&b<a+n; }
static int inside(uint64_t p,uint64_t n,uint64_t base,uint64_t size)
{ return n&&p>=base&&p-base<=size&&n<=size-(p-base); }
int dre_gpu_fw_handoff_valid(const uint64_t w[8],const uint64_t storage[8])
{
    if(!w||!storage||!dre_storage_abi_valid(storage)||w[0]!=DRE_GPU_FW_HANDOFF_MAGIC||
       w[1]!=(UINT64_C(64)<<32|1)||w[6]!=DRE_GPU_FW_CHIP_ID||
       w[7]!=dre_storage_crc(w,56)||!w[3]||w[3]>DRE_GPU_FW_PACKAGE_MAX||
       w[5]!=DRE_GPU_FW_METADATA_BYTES||((w[2]|w[3]|w[4]|w[5])&(DRE_GPU_FW_ALIGNMENT-1))||
       !inside(w[2],w[3],storage[5],storage[6])||!inside(w[4],w[5],storage[5],storage[6])||
       overlap(w[2],w[3],w[4],w[5])||overlap(w[2],w[3],storage[1],storage[2])||
       overlap(w[4],w[5],storage[1],storage[2])||overlap(w[2],w[3],storage[3],storage[4])||
       overlap(w[4],w[5],storage[3],storage[4]))return 0;
    return 1;
}
