/* SPDX-License-Identifier: MIT
 * UNVALIDATED: not compiled or run. Inspects existing loaded runtime metadata
 * only; does not construct _MTLDevice or reproduce Apple's private implementation.
 */
#import <Foundation/Foundation.h>
#include "base_metadata.h"
#include <objc/runtime.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if !defined(__arm64__) || defined(__arm64e__)
#error "Metal306 base metadata profile requires plain ARM64"
#endif
namespace {
constexpr char metalPath[]="/System/Library/Frameworks/Metal.framework/Metal";
constexpr uint8_t metalUUID[16]={0x0f,0x0b,0x7b,0x96,0x90,0xbf,0x3a,0x07,
    0xb7,0xe1,0x85,0xe2,0xfb,0x61,0x60,0xe6};
struct MethodSpec {const char *name;char result;uint32_t count;uint64_t address;};
/* Addresses are inventoried selected-image facts, never callable constants.
 * Captured construction bodies do not qualify all private table semantics. */
constexpr MethodSpec methods[]={
    {"init",'@',2,0x181a3634cull},
    {"initWithAcceleratorPort:",'@',3,0x181aab968ull},
    {"initLimits",'v',2,0x181a1b4bcull},
    {"initFeatureQueries",'v',2,0x181a1bd38ull},
    {"initGPUFamilySupport",'v',2,0x181a1ea28ull},
    {"dealloc",'v',2,0x181aab9d8ull},
    {"initSerializationPaths",'v',2,0x181a1a3a4ull},
    {".cxx_construct",'@',2,0x181a47964ull}
};
struct MemberSpec {const char *name;char kind;};
constexpr MemberSpec members[]={
    {"_limits",'{'},{"_supportedGPUFamilies",'^'},{"_featureQueries",'@'},
    {"_compilerConnectionManager",'^'},{"_libraryBuilder",'^'},
    {"_pipelineLibraryBuilder",'^'},{"_serialQueue",'@'},
    {"_pipelineSerializationQueue",'@'},{"_concurrentQueue",'@'},
    {"_initProgressTrackingOnceToken",'{'},{"_initConnectionOnceToken",'{'},
    {"_architecture",'@'},{"_pipelineCollection",'^'}
};
static_assert(sizeof(methods)/sizeof(methods[0])==DRE_METAL306_BASE_METHODS,"method metadata count");
static_assert(sizeof(members)/sizeof(members[0])==DRE_METAL306_BASE_MEMBERS,"member metadata count");
bool selectedImage(const mach_header_64 *h)
{
    if(!h||h->magic!=MH_MAGIC_64||h->cputype!=CPU_TYPE_ARM64||
       h->cpusubtype!=CPU_SUBTYPE_ARM64_ALL||h->filetype!=MH_DYLIB||
       !h->ncmds||h->ncmds>4096||h->sizeofcmds>(4u<<20))return false;
    const uint8_t *p=(const uint8_t *)(h+1);size_t at=0;bool found=false;
    for(uint32_t i=0;i<h->ncmds;i++) {
        if(at>h->sizeofcmds||h->sizeofcmds-at<sizeof(load_command))return false;
        const load_command *c=(const load_command *)(p+at);
        if(c->cmdsize<sizeof(*c)||(c->cmdsize&3u)||c->cmdsize>h->sizeofcmds-at)return false;
        if(c->cmd==LC_UUID) {
            if(found||c->cmdsize!=sizeof(uuid_command)||
               memcmp(((const uuid_command *)c)->uuid,metalUUID,16))return false;
            found=true;
        }
        at+=c->cmdsize;
    }
    return found&&at==h->sizeofcmds;
}
bool scalarType(Method m,unsigned index,char kind,bool returned=false)
{
    char *type=returned?method_copyReturnType(m):method_copyArgumentType(m,index);
    bool good=type&&type[0]==kind&&!type[1];free(type);return good;
}
bool unslid(const void *value,const mach_header_64 *image,intptr_t slide,uint64_t *out)
{
    Dl_info info={};
    if(!value||!dladdr(value,&info)||info.dli_fbase!=image)return false;
    uint64_t address=(uint64_t)(uintptr_t)value;
    if(slide>=0) {if(address<(uint64_t)slide)return false;address-=(uint64_t)slide;}
    else {
        uint64_t amount=(uint64_t)(-(slide+1))+1;
        if(amount>UINT64_MAX-address)return false;address+=amount;
    }
    *out=address;return true;
}
}
enum dre_metal306_metadata_status dre_metal306_inspect_base_metadata(
    struct dre_metal306_base_metadata *out)
{
    if(!out)return DRE_METAL306_METADATA_INVALID;
    memset(out,0,sizeof(*out));
    const mach_header_64 *image=nullptr;intptr_t slide=0;
    uint32_t imageCount=_dyld_image_count();
    if(imageCount>16384)return DRE_METAL306_METADATA_IMAGE_MISMATCH;
    for(uint32_t i=0;i<imageCount;i++) {
        const char *name=_dyld_get_image_name(i);
        if(!name||strcmp(name,metalPath))continue;
        if(image)return DRE_METAL306_METADATA_IMAGE_MISMATCH;
        image=(const mach_header_64 *)_dyld_get_image_header(i);slide=_dyld_get_image_vmaddr_slide(i);
    }
    if(!selectedImage(image))return DRE_METAL306_METADATA_IMAGE_MISMATCH;
    Class base=objc_lookUpClass("_MTLDevice"),root=objc_lookUpClass("NSObject");
    const char *owner=base?class_getImageName(base):nullptr;
    if(!base||!root||!owner||strcmp(owner,metalPath)||class_getSuperclass(base)!=root||
       class_getInstanceSize(base)!=528||class_getInstanceSize(root)!=8)
        return DRE_METAL306_METADATA_CLASS_MISMATCH;
    dre_metal306_base_metadata result={};
    result.version=DRE_METAL306_BASE_METADATA_VERSION;result.struct_bytes=sizeof(result);
    result.instance_bytes=528;result.super_instance_bytes=8;
    unsigned count=0;Method *declared=class_copyMethodList(base,&count);
    if(!declared||count>4096) {free(declared);return DRE_METAL306_METADATA_METHOD_MISMATCH;}
    bool good=true;
    for(uint32_t i=0;i<DRE_METAL306_BASE_METHODS&&good;i++) {
        Method found=nullptr;
        for(unsigned j=0;j<count;j++) {
            const char *name=sel_getName(method_getName(declared[j]));
            if(!name||strcmp(name,methods[i].name))continue;
            if(found) {good=false;break;}found=declared[j];
        }
        uint64_t address=0;
        if(!good||!found||method_getNumberOfArguments(found)!=methods[i].count||
           !scalarType(found,0,methods[i].result,true)||!scalarType(found,0,'@')||
           !scalarType(found,1,':')||(methods[i].count==3&&!scalarType(found,2,'I'))||
           !unslid((const void *)method_getImplementation(found),image,slide,&address)||address!=methods[i].address)
            {good=false;break;}
        result.methods[i]={i,methods[i].count,address};result.method_count++;
    }
    free(declared);if(!good)return DRE_METAL306_METADATA_METHOD_MISMATCH;
    unsigned ivarCount=0;Ivar *ivars=class_copyIvarList(base,&ivarCount);
    if(!ivars||ivarCount>128) {free(ivars);return DRE_METAL306_METADATA_MEMBER_MISMATCH;}
    for(uint32_t i=0;i<DRE_METAL306_BASE_MEMBERS&&good;i++) {
        Ivar found=nullptr;
        for(unsigned j=0;j<ivarCount;j++) {
            const char *name=ivar_getName(ivars[j]);
            if(!name||strcmp(name,members[i].name))continue;
            if(found) {good=false;break;}found=ivars[j];
        }
        const char *type=found?ivar_getTypeEncoding(found):nullptr;
        if(!good||!type||type[0]!=members[i].kind||strnlen(type,8193)>8192) {good=false;break;}
        NSUInteger bytes=0,alignment=0;const char *end=nullptr;
        @try {end=NSGetSizeAndAlignment(type,&bytes,&alignment);}
        @catch(NSException *error) {(void)error;good=false;break;}
        ptrdiff_t offset=ivar_getOffset(found);
        if(!end||*end||!bytes||!alignment||alignment>16||(alignment&(alignment-1))||
           offset<8||(uint64_t)offset>528||bytes>528-(uint64_t)offset||((uint64_t)offset%alignment)||
           ((members[i].kind=='@'||members[i].kind=='^')&&(bytes!=8||alignment!=8))) {good=false;break;}
        for(uint32_t j=0;j<i;j++) {
            const auto &p=result.members[j];
            if((uint64_t)offset<(uint64_t)p.offset+p.bytes&&p.offset<(uint64_t)offset+bytes)good=false;
        }
        if(!good)break;
        result.members[i]={i,(uint32_t)offset,(uint32_t)bytes,(uint32_t)alignment,(uint32_t)type[0],0};
        result.member_count++;
    }
    free(ivars);if(!good)return DRE_METAL306_METADATA_MEMBER_MISMATCH;
    *out=result;return DRE_METAL306_METADATA_INSPECTED;
}
