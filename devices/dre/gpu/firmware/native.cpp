/* SPDX-License-Identifier: MIT -- UNVALIDATED, no builds or execution.
 * Caller holds actual native GPU + SCM lifetime locks over EVERY API call. */
#include "native.h"
#include "../a619_backend.h"
#include "../../native_board.h"
#include "../../storage_live.h"
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <vm/pmap.h>
#include <vm/memory_types.h>
#include <machine/memory_types.h>
#include <pexpert/pexpert.h>
extern "C" {
int dre_ufs_get_handoff(uint64_t[8]);
vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned);
void ml_io_unmap(vm_offset_t,vm_size_t);
uint64_t kvtophys(vm_offset_t);
#if defined(DRE_GPU_FW_BOOT_NC_PROOF) && DRE_GPU_FW_BOOT_NC_PROOF
int dre_gpu_fw_pmap_verify_boot_nc(uint64_t,uint64_t,uint64_t);
#endif
}
struct dre_gpu_fw_native {
    const dre_gpu_fw_trust *trust;
    const void *fdt;
    size_t fdtBytes;
    uint64_t handoff[12],storage[8];
    void *copy;
    dre_gpu_fw_package package;
    dre_gpu_fw_region region;
    dre_gpu_fw_relocation relocation;
    dre_gpu_fw_native_ops ops;
    dre_a619_pas13 pas;
    vm_offset_t metadataMap,reservedMap;
    uint32_t sourceHolds;
    bool verified,prepared,unsafe,relocated;
};
static int same(const uint8_t *a,const uint8_t *b,size_t n)
{ unsigned difference=0;while(n--)difference|=*a++^*b++;return difference==0; }
static int hashMatches(void *,const void *p,size_t n,const uint8_t expected[32])
{
    uint8_t computed[32];SHA256_CTX ctx;
    SHA256_Init(&ctx);SHA256_Update(&ctx,p,n);SHA256_Final(computed,&ctx);
    int result=same(computed,expected,32);bzero(computed,sizeof(computed));bzero(&ctx,sizeof(ctx));
    return result;
}
static void unmapTemporary(vm_offset_t p,size_t n)
{ if(p)ml_io_unmap(trunc_page(p),round_page(n+(p&PAGE_MASK))); }
static bool getHandoff(uint64_t words[12])
{
    IORegistryEntry *entry=IORegistryEntry::fromPath("/dre-gpu-firmware",gIODTPlane);
    if(!entry)return false;
    OSData *compatible=OSDynamicCast(OSData,entry->getProperty("compatible"));
    OSData *handoff=OSDynamicCast(OSData,entry->getProperty("dre,firmware-handoff"));
    const char v1[]="oneplus,dre-gpu-firmware-v1",v2[]="oneplus,dre-gpu-firmware-v2";
    bool legacy=compatible&&compatible->getLength()==sizeof(v1)&&
        !memcmp(compatible->getBytesNoCopy(),v1,sizeof(v1))&&handoff&&handoff->getLength()==64;
    bool relocated=compatible&&compatible->getLength()==sizeof(v2)&&
        !memcmp(compatible->getBytesNoCopy(),v2,sizeof(v2))&&handoff&&handoff->getLength()==96;
    bool ok=legacy||relocated;
    if(ok) {
        bzero(words,96);bcopy(handoff->getBytesNoCopy(),words,legacy?64:96);
        ok=words[1]==((uint64_t)(legacy?64:96)<<32|(legacy?1u:2u));
    }
    entry->release();return ok;
}
static bool handoffValid(dre_gpu_fw_native *c)
{
    return c->handoff[1]==(UINT64_C(96)<<32|2)?
        dre_gpu_fw_relocation_handoff_valid(c->handoff,c->storage)==1:
        dre_gpu_fw_handoff_valid(c->handoff,c->storage)==1;
}
extern "C" const char *dre_gpu_fw_native_load(dre_gpu_fw_native **out)
{
    if(!out)return "FW_NATIVE_INVALID_ARGUMENT";*out=NULL;
    const dre_gpu_fw_trust *trust=dre_gpu_fw_trusted_anchor();
    if(!trust)return "FW_NATIVE_PRIVATE_KERNEL_ANCHOR_MISSING";
    dre_gpu_fw_native *c=(dre_gpu_fw_native *)IOMallocZero(sizeof(*c));
    if(!c)return "FW_NATIVE_NO_MEMORY";
    c->trust=trust;
    if(dre_native_board_snapshot(&c->fdt,&c->fdtBytes)||!dre_ufs_get_handoff(c->storage)||
       !getHandoff(c->handoff)||!dre_storage_live_valid(c->fdt,c->storage)||
       !handoffValid(c)||
       !dre_dma_boot_owned_ram_allowed(c->fdt,c->handoff[2],c->handoff[3])||
       !dre_dma_boot_owned_ram_allowed(c->fdt,c->handoff[4],c->handoff[5])||
       c->handoff[3]!=trust->package_bytes||
       (c->handoff[1]==(UINT64_C(96)<<32|2)&&
        !dre_dma_boot_owned_ram_allowed(c->fdt,c->handoff[7],c->handoff[8]))) {
        IOFree(c,sizeof(*c));return "FW_NATIVE_BOOT_OWNERSHIP_REJECTED";
    }
    vm_offset_t mapping=ml_io_map_unmappable(c->handoff[2],c->handoff[3],VM_WIMG_IO);
    if(!mapping){IOFree(c,sizeof(*c));return "FW_NATIVE_PACKAGE_MAP_FAILED";}
    c->copy=IOMalloc(c->handoff[3]);
    bool physical=c->copy!=NULL;
    for(uint64_t off=0;physical&&off<c->handoff[3];off+=4096)
        physical=kvtophys(mapping+off)==c->handoff[2]+off;
    if(physical)for(size_t i=0;i<c->handoff[3];i++)
        ((uint8_t *)c->copy)[i]=((volatile uint8_t *)mapping)[i];
    __asm__ volatile("dsb sy":::"memory");unmapTemporary(mapping,c->handoff[3]);
    if(!physical) {
        if(c->copy)IOFree(c->copy,c->handoff[3]);IOFree(c,sizeof(*c));
        return "FW_NATIVE_PACKAGE_PA_OR_ALLOCATION_FAILED";
    }
    const char *status=dre_gpu_fw_verify(c->copy,c->handoff[3],trust,hashMatches,NULL,&c->package);
    if(strcmp(status,"FW_PRIVATE_DIGESTS_VERIFIED")) {
        bzero(c->copy,c->handoff[3]);IOFree(c->copy,c->handoff[3]);IOFree(c,sizeof(*c));return status;
    }
    c->relocated=c->handoff[1]==(UINT64_C(96)<<32|2);
    status=dre_gpu_fw_reserved_region(c->fdt,&c->region);
    dre_a619_sqe sqe={};struct dre_a619_zap_plan plan={};
    if(strcmp(status,"FW_REGION_GEOMETRY_ONLY")||
       strcmp(dre_a619_sqe_view(DRE_GPU_FW_CHIP_ID,c->package.sqe.bytes,c->package.sqe.size,&sqe),"SQE_VIEW_READY")) {
        bzero(c->copy,c->handoff[3]);IOFree(c->copy,c->handoff[3]);IOFree(c,sizeof(*c));
        return "FW_NATIVE_PARSER_OR_RESERVED_REGION_REJECTED";
    }
    status=c->relocated?dre_gpu_fw_relocation_plan(c->package.zap.bytes,c->package.zap.size,
        c->handoff[7],c->handoff[8],c->handoff[4],c->handoff[5],&c->relocation):
        dre_a619_zap_plan(c->package.zap.bytes,c->package.zap.size,c->region.pa,c->region.bytes,&plan);
    if(strcmp(status,c->relocated?"FW_RELOCATION_STRUCTURE_ONLY":"ZAP_PLAN_OK")) {
        bzero(c->copy,c->handoff[3]);IOFree(c->copy,c->handoff[3]);IOFree(c,sizeof(*c));return status;
    }
    c->verified=true;*out=c;return "FW_NATIVE_PRIVATE_SOURCE_VERIFIED";
}
extern "C" int dre_gpu_fw_native_source(dre_gpu_fw_native *c,dre_gpu_fw_kind kind,dre_gpu_fw_view *out)
{
    if(!c||!out||!c->verified||c->unsafe)return 0;
    const dre_gpu_fw_view *v=kind==DRE_GPU_FW_SQE?&c->package.sqe:
        kind==DRE_GPU_FW_ZAP?&c->package.zap:NULL;
    const uint8_t *digest=kind==DRE_GPU_FW_SQE?c->trust->sqe_sha256:c->trust->zap_sha256;
    if(!v||!hashMatches(NULL,c->copy,c->handoff[3],c->trust->package_sha256)||
       !hashMatches(NULL,v->bytes,v->size,digest))return 0;
    *out=*v;return 1;
}
extern "C" int dre_gpu_fw_native_sqe_verified(dre_gpu_fw_native *c,const void *p,size_t n)
{
    dre_gpu_fw_view v={};return dre_gpu_fw_native_source(c,DRE_GPU_FW_SQE,&v)==1&&v.bytes==p&&v.size==n;
}
extern "C" const dre_gpu_fw_region *dre_gpu_fw_native_region(dre_gpu_fw_native *c)
{ return c&&c->verified?&c->region:NULL; }
extern "C" int dre_gpu_fw_native_relocation_snapshot(dre_gpu_fw_native *c,
    dre_gpu_fw_relocation_view *out)
{
    dre_gpu_fw_view source={};
    if(!c||!out||!c->relocated||c->unsafe||!handoffValid(c)||
       dre_gpu_fw_native_source(c,DRE_GPU_FW_ZAP,&source)!=1)return 0;
    dre_gpu_fw_relocation plan={};
    if(strcmp(dre_gpu_fw_relocation_plan(source.bytes,source.size,c->handoff[7],c->handoff[8],
       c->handoff[4],c->handoff[5],&plan),"FW_RELOCATION_STRUCTURE_ONLY"))return 0;
    *out={c->region,plan,c->storage[5],c->storage[6]};return 1;
}
extern "C" int dre_gpu_fw_native_source_hold(dre_gpu_fw_native *c)
{
    dre_gpu_fw_view source={};
    if(!c||c->sourceHolds==UINT32_MAX||dre_gpu_fw_native_source(c,DRE_GPU_FW_ZAP,&source)!=1)return 0;
    ++c->sourceHolds;return 1;
}
extern "C" int dre_gpu_fw_native_source_drop(dre_gpu_fw_native *c)
{if(!c||!c->sourceHolds)return 0;--c->sourceHolds;return 1;}
static int aliasProof(dre_gpu_fw_native *c,vm_offset_t cpu,uint64_t pa,size_t bytes)
{
    for(size_t off=0;off<bytes;off+=4096)if(kvtophys(cpu+off)!=pa+off)return 0;
    if(c->ops.normal_nc)return c->ops.normal_nc(c->ops.cookie,(void *)cpu,pa,bytes)==1;
#if defined(DRE_GPU_FW_BOOT_NC_PROOF) && DRE_GPU_FW_BOOT_NC_PROOF
    return dre_gpu_fw_pmap_verify_boot_nc(cpu,pa,bytes)==1;
#else
    return 0;
#endif
}
static int nativeGuard(void *cookie)
{
    dre_gpu_fw_native *c=(dre_gpu_fw_native *)cookie;
    if(!c||c->unsafe||!c->verified||!c->ops.lease||!handoffValid(c))return 0;
    if(c->relocated&&(c->relocation.load_pa!=c->handoff[7]||
       c->relocation.load_bytes!=c->handoff[8]||c->relocation.metadata_pa!=c->handoff[4]||
       c->relocation.metadata_bytes!=c->handoff[5]||c->relocation.reserved))return 0;
    bool owned=c->relocated?c->ops.owns_relocated_pas13&&
        c->ops.owns_relocated_pas13(c->ops.cookie,c->ops.lease,&c->region,&c->relocation,
            c->storage[5],c->storage[6])==1:
        c->ops.owns_pas13&&c->ops.owns_pas13(c->ops.cookie,c->ops.lease,&c->region)==1;
    uint64_t target=c->relocated?c->relocation.load_pa:c->region.pa;
    size_t bytes=c->relocated?c->relocation.load_bytes:c->region.bytes;
    return owned&&
        c->ops.scm64_discovered&&c->ops.scm64_discovered(c->ops.cookie)==1&&
        c->ops.scm_clocks_held&&c->ops.scm_clocks_held(c->ops.cookie,c->ops.lease)==1&&
        c->ops.scm.guard&&c->ops.scm.guard(c->ops.scm.cookie)==1&&
        (!c->prepared||(aliasProof(c,c->metadataMap,c->handoff[4],c->handoff[5])&&
                       aliasProof(c,c->reservedMap,target,bytes)));
}
static int smc(void *cookie,dre_a619_smc_regs *regs)
{
    dre_gpu_fw_native *c=(dre_gpu_fw_native *)cookie;
    return nativeGuard(c)&&c->ops.scm.smc64&&c->ops.scm.smc64(c->ops.scm.cookie,regs)==1;
}
static void barrier(void *cookie)
{
    dre_gpu_fw_native *c=(dre_gpu_fw_native *)cookie;
    __asm__ volatile("dsb sy":::"memory");c->ops.scm.barrier(c->ops.scm.cookie);
}
static uint64_t now(void *cookie)
{ dre_gpu_fw_native *c=(dre_gpu_fw_native *)cookie;return c->ops.scm.now(c->ops.scm.cookie); }
static dre_a619_pas13_ops pasOps(dre_gpu_fw_native *c)
{ return {nativeGuard,smc,barrier,now,c}; }
extern "C" const char *dre_gpu_fw_native_prepare_pas13(dre_gpu_fw_native *c,
    const dre_gpu_fw_native_ops *ops)
{
    if(!c||!ops||!c->verified||c->unsafe||c->prepared)return "FW_NATIVE_PAS_NOT_FRESH";
    /* Keep the real source-consumer retirement proof available even when a
     * source-only geometry refusal stops us before any mapping/SCM access. */
    c->ops=*ops;
    if(!c->relocated&&(c->region.limitations&DRE_GPU_FW_REGION_ADJACENT_RESERVED))
        return "FW_NATIVE_GPU_GRANULE_SHARES_OTHER_RESERVED_OWNER";
    if(!c->relocated&&(c->region.limitations&DRE_GPU_FW_REGION_SUBPAGE))
        return "FW_NATIVE_GPU_RESERVED_SUBPAGE_UNSUPPORTED";
    if(!ops->scm.smc64||!ops->scm.barrier||!ops->scm.now||!ops->consumers_retired)
        return "FW_NATIVE_SCM_OR_RETIREMENT_PROVIDER_MISSING";
    uint64_t hz=0;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
    if(hz!=19200000||page_size!=16384||!nativeGuard(c))return "FW_NATIVE_PAS_POLICY_UNQUALIFIED";
    uint64_t target=c->relocated?c->relocation.load_pa:c->region.pa;
    size_t targetBytes=c->relocated?c->relocation.load_bytes:c->region.bytes;
    c->metadataMap=ml_io_map_unmappable(c->handoff[4],c->handoff[5],VM_WIMG_WCOMB);
    c->reservedMap=ml_io_map_unmappable(target,targetBytes,VM_WIMG_WCOMB);
    if(!c->metadataMap||!c->reservedMap||
       !aliasProof(c,c->metadataMap,c->handoff[4],c->handoff[5])||
       !aliasProof(c,c->reservedMap,target,targetBytes)) {
        c->unsafe=true;return "FW_NATIVE_PAS_MAPPING_UNCERTAIN_RETAINED";
    }
    dre_gpu_fw_view zap={};if(!dre_gpu_fw_native_source(c,DRE_GPU_FW_ZAP,&zap)) {
        c->unsafe=true;return "FW_NATIVE_ZAP_DIGEST_CHANGED";
    }
    c->pas.chip_id=DRE_GPU_FW_CHIP_ID;c->pas.tick_hz=19200000;
    c->pas.firmware=zap.bytes;c->pas.firmware_bytes=zap.size;
    c->pas.metadata_cpu=(void *)c->metadataMap;c->pas.metadata_pa=c->handoff[4];
    c->pas.metadata_capacity=c->handoff[5];
    c->pas.reserved_cpu=(void *)c->reservedMap;c->pas.reserved_pa=target;c->pas.reserved_bytes=targetBytes;
    /* These follow actual qualified provider/mapping/hash checks above.
     * No caller readiness mask or FDT identity alone reaches this point. */
    c->pas.prerequisites=DRE_A619_PAS13_PREREQUISITES;c->prepared=true;
    if(!nativeGuard(c)){c->unsafe=true;return "FW_NATIVE_PAS_HOLD_LOST";}
    return "FW_NATIVE_PAS_BUFFERS_PREPARED_ONLY";
}
extern "C" const char *dre_gpu_fw_native_authenticate(dre_gpu_fw_native *c)
{
    if(!c||!c->prepared||!nativeGuard(c))return "FW_NATIVE_PAS_PROVIDER_UNAVAILABLE";
    dre_gpu_fw_view zap={};if(!dre_gpu_fw_native_source(c,DRE_GPU_FW_ZAP,&zap))return "FW_NATIVE_ZAP_DIGEST_CHANGED";
    dre_a619_pas13_ops ops=pasOps(c);
    const char *status=dre_a619_pas13_authenticate(&c->pas,&ops);
    if(c->pas.unsafe)c->unsafe=true;return status;
}
extern "C" const char *dre_gpu_fw_native_shutdown(dre_gpu_fw_native *c)
{
    if(!c||!c->prepared||!nativeGuard(c))return "FW_NATIVE_PAS_PROVIDER_UNAVAILABLE";
    dre_a619_pas13_ops ops=pasOps(c);
    const char *status=dre_a619_pas13_shutdown(&c->pas,&ops);
    if(c->pas.unsafe)c->unsafe=true;return status;
}
extern "C" const char *dre_gpu_fw_native_release(dre_gpu_fw_native *c)
{
    if(!c||c->sourceHolds||!c->ops.lease||c->unsafe||c->pas.unsafe||c->pas.initialized||c->pas.authenticated||
       !c->ops.consumers_retired||c->ops.consumers_retired(c->ops.cookie,c->ops.lease)!=1)
        return "FW_NATIVE_FIRMWARE_CONSUMERS_NOT_RETIRED";
    unmapTemporary(c->reservedMap,c->relocated?c->relocation.load_bytes:c->region.bytes);
    unmapTemporary(c->metadataMap,c->handoff[5]);
    bzero(c->copy,c->handoff[3]);IOFree(c->copy,c->handoff[3]);bzero(c,sizeof(*c));IOFree(c,sizeof(*c));
    return "FW_NATIVE_PRIVATE_COPY_RETIRED";
}
