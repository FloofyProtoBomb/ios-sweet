/* SPDX-License-Identifier: MIT
 * UNVALIDATED source-only policy. No SDK compile/runtime/factory/GPU test.
 * Actual queried limits and explicit refusals, never inherited Apple tables.
 */
#include "device_policy.h"
#include <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
/* One official-SDK policy object includes this bounded metadata implementation;
 * do not additionally link a standalone base_metadata.o into the same bundle. */
#include "base_init/base_metadata.mm"
#include "base_init/base_environment.h"

#if !defined(__arm64__) || defined(__arm64e__)
#error "Selected20H392 policy is plain ARM64 only"
#endif
@interface DREMetalDevicePolicy : NSObject {
@public
    DreMetalDeviceFacts facts;
    NmbDevice *device; /* frontend's existing process-lifetime global owner */
    NSSet<NSString *> *refused;
}
@end
@implementation DREMetalDevicePolicy
@end
static char policyKey;
static DREMetalDevicePolicy *policy(Class cls)
{
    for(unsigned i=0;cls&&i<32;i++,cls=class_getSuperclass(cls))
        if(id value=objc_getAssociatedObject(cls,&policyKey))return (DREMetalDevicePolicy *)value;
    return nil;
}
[[noreturn]] static void refuse(id self,SEL command)
{
    [NSException raise:NSInvalidArgumentException format:@"DRE Metal306: %@ has no qualified implementation of %s",
        NSStringFromClass(object_getClass(self)),sel_getName(command)];
    __builtin_trap();
}
static DREMetalDevicePolicy *required(id self,SEL command)
{
    DREMetalDevicePolicy *p=policy(object_getClass(self));
    if(!p)refuse(self,command);
    return p;
}
static NSUInteger count(id self,SEL command)
{
    const DreMetalDeviceFacts &p=required(self,command)->facts;
    const char *name=sel_getName(command);
    if(!strcmp(name,"maxBufferLength"))return (NSUInteger)p.max_buffer;
    if(!strcmp(name,"maxThreadgroupMemoryLength"))return (NSUInteger)p.threadgroup_memory;
    if(!strcmp(name,"maxTotalComputeThreadsPerThreadgroup"))return (NSUInteger)p.workgroup_invocations;
    if(!strcmp(name,"maxColorAttachments"))return p.color_attachments;
    /* Deliberately conservative port recommendation of one actual maximum
     * allocation, not a fabricated whole-GPU memory budget or CPU RAM/4. */
    if(!strcmp(name,"recommendedMaxWorkingSetSize"))return (NSUInteger)p.max_buffer;
    if(!strcmp(name,"dedicatedMemorySize")&&p.kind==1&&(p.flags&1))return 0;
    refuse(self,command);
}
static MTLSize threads(id self,SEL command)
{
    const DreMetalDeviceFacts &p=required(self,command)->facts;
    return MTLSizeMake(p.axes[0],p.axes[1],p.axes[2]);
}
static BOOL unified(id self,SEL command)
{return (required(self,command)->facts.flags&1)!=0;}
static BOOL noFlag(id self,SEL command)
{(void)required(self,command);return NO;}
static BOOL noFamily(id self,SEL command,MTLGPUFamily)
{(void)required(self,command);return NO;}
static BOOL noFeatureSet(id self,SEL command,MTLFeatureSet)
{(void)required(self,command);return NO;}
static BOOL queriedFlag(id self,SEL command)
{
    uint64_t value=0;DREMetalDevicePolicy *p=required(self,command);
    if(!nmb_device_answer(p->device,sel_getName(command),0,&value)||value>1)refuse(self,command);
    return value!=0;
}
static BOOL queriedFlagArg(id self,SEL command,NSUInteger argument)
{
    uint64_t value=0;DREMetalDevicePolicy *p=required(self,command);
    if(!nmb_device_answer(p->device,sel_getName(command),argument,&value)||value>1)refuse(self,command);
    return value!=0;
}
static void registration(id self,SEL command)
{
    /* This checks our real facts/refusal state. It neither runs inherited
     * Apple-table writers nor claims to initialize their private storage. */
    (void)required(self,command);
}
static bool sameType(const char *actual,const char *expected)
{return actual&&expected&&!strcmp(actual,expected);}
static bool replace(Class cls,const char *name,IMP implementation,const char *result,
    const char *argument=nullptr,bool mandatory=false)
{
    SEL sel=sel_registerName(name);Method method=class_getInstanceMethod(cls,sel);
    if(!method)return !mandatory; /* No ABI invented for a newer absent SPI. */
    unsigned n=method_getNumberOfArguments(method);
    char returned[128]={},arg[128]={};method_getReturnType(method,returned,sizeof(returned));
    if(argument)method_getArgumentType(method,2,arg,sizeof(arg));
    if(n!=(argument?3u:2u)||!sameType(returned,result)||(argument&&!sameType(arg,argument)))return false;
    class_replaceMethod(cls,sel,implementation,method_getTypeEncoding(method));return true;
}
static bool own(Class cls,SEL sel)
{
    unsigned n=0;Method *methods=class_copyMethodList(cls,&n);bool found=false;
    for(unsigned i=0;i<n;i++)if(method_getName(methods[i])==sel)found=true;
    free(methods);return found;
}
static bool foundational(SEL sel)
{
    /* Real selected base -init sends this helper to self. Its body/selector
     * were inspected on20H392; blanket forwarding here breaks actual init.
     * This does not qualify the private limits/family/feature tables. */
    static const char *names[]={"init","dealloc","retain","release","autorelease","retainCount",
        "class","superclass","self","zone",".cxx_construct",".cxx_destruct","_tryRetain","_isDeallocating",
        "allowsWeakReference","retainWeakReference","initSerializationPaths"};
    const char *name=sel_getName(sel);
    for(const char *value:names)if(!strcmp(name,value))return true;
    return false;
}
bool dre_metal306_selector_refused(Class cls,SEL sel)
{
    DREMetalDevicePolicy *p=policy(cls);
    return p&&[p->refused containsObject:NSStringFromSelector(sel)];
}
bool dre_metal306_install_device_policy(Class cls,NmbDevice *device)
{
    if(!cls||!device||objc_getAssociatedObject(cls,&policyKey))return false;
    struct dre_metal306_base_metadata baseMetadata={};
    if(!dre_metal306_base_environment_allowed()||
       dre_metal306_inspect_base_metadata(&baseMetadata)!=DRE_METAL306_METADATA_INSPECTED)return false;
    DREMetalDevicePolicy *p=[DREMetalDevicePolicy new];
    if(!p)return false;p->device=device;
    p->facts={};p->facts.version=1;p->facts.bytes=sizeof(p->facts);
    if(!nmb_dre_device_facts(device,&p->facts)||p->facts.version!=1||p->facts.bytes!=sizeof(p->facts)||
       p->facts.vendor!=0x5143||p->facts.chip!=0x06010900||p->facts.kind!=1||p->facts.flags!=1||
       !p->facts.max_buffer||p->facts.max_buffer>p->facts.max_allocation||!p->facts.threadgroup_memory||
       !p->facts.workgroup_invocations||!p->facts.axes[0]||!p->facts.axes[1]||!p->facts.axes[2]||!p->facts.color_attachments)
        return false;
    /* The real ObjC ARM64 forwarding entry preserves each original method's
     * type encoding, including structures. It never returns a generic object
     * signature or a fabricated zero-valued result. Missing forwarding refuses
     * construction. No -undefined dynamic_lookup linker waiver is involved. */
    IMP forward=(IMP)dlsym(RTLD_DEFAULT,"_objc_msgForward");if(!forward)return false;
    NSMutableSet<NSString *> *blocked=[NSMutableSet set];
    if(!blocked)return false;
    Class base=class_getSuperclass(cls),root=objc_getClass("NSObject");unsigned total=0,depth=0;
    if(!root||base!=objc_getClass("_MTLDevice")||class_getInstanceSize(base)!=528)return false;
    for(Class at=base;at&&at!=root;at=class_getSuperclass(at)) {
        if(++depth>16)return false;
        unsigned n=0;Method *methods=class_copyMethodList(at,&n);
        if(n>4096||n>8192-total) {free(methods);return false;}total+=n;
        for(unsigned i=0;i<n;i++) {
            SEL sel=method_getName(methods[i]);
            if(own(cls,sel)||foundational(sel)||class_getInstanceMethod(root,sel))continue;
            const char *types=method_getTypeEncoding(methods[i]);
            if(!types||!class_addMethod(cls,sel,forward,types)) {free(methods);return false;}
            [blocked addObject:NSStringFromSelector(sel)];
        }
        free(methods);
    }
    /* Existing frontend methods whose native semantics are not implemented
     * must not retain a misleading CPU-copy or in-process sharing route. */
    static const char *unsupported[]={"gpuCoreCount","sharedMemorySize","isLowPower","isHeadless","targetDeviceInfo",
        "iosurfaceReadOnlyTextureAlignmentBytes","iosurfaceTextureAlignmentBytes",
        "minimumTextureBufferAlignmentForPixelFormat:","minimumLinearTextureAlignmentForPixelFormat:",
        "requiredLinearTextureBytesPerRowForDescriptor:","newSharedEvent","newSharedEventWithOptions:",
        "newTextureWithDescriptor:iosurface:plane:","newBufferWithBytesNoCopy:length:options:deallocator:",
        "newTextureWithBytesNoCopy:length:descriptor:deallocator:","newTextureLayoutWithDescriptor:isHeapOrBufferBacked:"};
    for(const char *name:unsupported) {
        SEL sel=sel_registerName(name);Method method=class_getInstanceMethod(cls,sel);
        if(method)class_replaceMethod(cls,sel,forward,method_getTypeEncoding(method));
        [blocked addObject:NSStringFromSelector(sel)];
    }
    const char *counts[]={"maxBufferLength","maxThreadgroupMemoryLength","maxTotalComputeThreadsPerThreadgroup",
        "maxColorAttachments","recommendedMaxWorkingSetSize","dedicatedMemorySize"};
    for(const char *name:counts) {
        if(!replace(cls,name,(IMP)count,@encode(NSUInteger),nullptr,!strcmp(name,"maxBufferLength")))return false;
        [blocked removeObject:NSStringFromSelector(sel_registerName(name))];
    }
    if(!replace(cls,"maxThreadsPerThreadgroup",(IMP)threads,@encode(MTLSize),nullptr,true)||
       !replace(cls,"hasUnifiedMemory",(IMP)unified,@encode(BOOL),nullptr,true)||
       !replace(cls,"supportsFamily:",(IMP)noFamily,@encode(BOOL),@encode(MTLGPUFamily),true)||
       !replace(cls,"supportsFeatureSet:",(IMP)noFeatureSet,@encode(BOOL),@encode(MTLFeatureSet),true))return false;
    const char *safe[]={"maxThreadsPerThreadgroup","hasUnifiedMemory","supportsFamily:","supportsFeatureSet:"};
    for(const char *name:safe)[blocked removeObject:NSStringFromSelector(sel_registerName(name))];
    /* These selected runtime branches read enabled Vulkan features/formats or
     * typed RuntimeGap results. No fallback from a refused answer to0/NO. */
    const char *queried[]={"supports32BitFloatFiltering","supportsRasterOrderGroups","areRasterOrderGroupsSupported",
        "isDepth24Stencil8PixelFormatSupported","areProgrammableSamplePositionsSupported","supportsSamplerReductionMode"};
    for(const char *name:queried) {
        if(!replace(cls,name,(IMP)queriedFlag,@encode(BOOL)))return false;
        [blocked removeObject:NSStringFromSelector(sel_registerName(name))];
    }
    const char *queriedArgs[]={"supportsTextureSampleCount:","supportsPrimitiveType:"};
    for(const char *name:queriedArgs) {
        if(!replace(cls,name,(IMP)queriedFlagArg,@encode(BOOL),@encode(NSUInteger)))return false;
        [blocked removeObject:NSStringFromSelector(sel_registerName(name))];
    }
    /* These absent paths answer false only where the selected real class
     * provides the exact BOOL/no-argument ABI. Unknown SPI is never invented. */
    const char *unavailable[]={"supportsSharedTextureHandles","supportsResourceDetachBacking","supportsBufferWithIosurface",
        "supportsBufferWithAddressRanges","supportsHeapWithAddressRanges","supportsExtendedXR10Formats",
        "supportsYCBCRFormats12","supportsYCBCRPackedFormats12","supportsMemorylessRenderTargets"};
    for(const char *name:unavailable) {
        if(!replace(cls,name,(IMP)noFlag,@encode(BOOL)))return false;
        [blocked removeObject:NSStringFromSelector(sel_registerName(name))];
    }
    const char *registrations[]={"initLimits","initFeatureQueries","initGPUFamilySupport","initWorkarounds"};
    for(const char *name:registrations) {
        if(!replace(cls,name,(IMP)registration,@encode(void),nullptr,true))return false;
        [blocked removeObject:NSStringFromSelector(sel_registerName(name))];
    }
    p->refused=[blocked copy];objc_setAssociatedObject(cls,&policyKey,p,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return true;
}
