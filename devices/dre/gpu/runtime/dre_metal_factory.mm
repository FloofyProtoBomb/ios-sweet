/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source draft; no SDK compile, factory registration
 * test or native Metal execution has run. No service is published here.
 * This is a bundle entry for the inspected Metal306 class/init ABI, not a
 * replacement MTLCreateSystemDefaultDevice or an AGX compatibility object.
 * Compile this file without ARC: init returns an owned +1 device reference.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach/mach.h>
#include <mutex>
#include "dre_gpu_uapi.h"

extern "C" id native_bridge_metal_open(const char *, const char *, uint64_t,
    void (*)(const char *, const char *));

@interface DREMetalBootstrap : NSObject
@end
static std::mutex factory_lock;
static thread_local bool constructing;

static bool native_contract(io_service_t accelerator, uint64_t *registry_id)
{
    if (!accelerator || !IOObjectConformsTo(accelerator, DRE_GPU_SERVICE)) return false;
    io_service_t direct = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(DRE_GPU_SERVICE));
    if (!direct) return false;
    uint64_t expected = 0, supplied = 0;
    bool same = IORegistryEntryGetRegistryEntryID(direct, &expected) == KERN_SUCCESS &&
        IORegistryEntryGetRegistryEntryID(accelerator, &supplied) == KERN_SUCCESS && expected && expected == supplied;
    IOObjectRelease(direct);
    if (!same) return false;
    io_connect_t connection = IO_OBJECT_NULL;
    if (IOServiceOpen(accelerator, mach_task_self(), 0, &connection) != KERN_SUCCESS) return false;
    struct dre_gpu_info_request request = {};
    request.header.abi_version = DRE_GPU_ABI_VERSION;
    request.header.struct_bytes = sizeof(request);
    struct dre_gpu_info_reply reply = {};
    size_t bytes = sizeof(reply);
    kern_return_t kr = IOConnectCallStructMethod(connection, DRE_GPU_GET_INFO,
        &request, sizeof(request), &reply, &bytes);
    kern_return_t closed = IOServiceClose(connection);
    if (kr != KERN_SUCCESS || closed != KERN_SUCCESS || bytes != sizeof(reply) ||
        reply.header.abi_version != DRE_GPU_ABI_VERSION || reply.header.struct_bytes != sizeof(reply) ||
        reply.header.flags || reply.header.status != DRE_GPU_OK || !reply.header.session_generation ||
        reply.chip_id != 0x06010900u ||
        (reply.capabilities & DRE_GPU_VULKAN_REQUIRED_CAPS) != DRE_GPU_VULKAN_REQUIRED_CAPS ||
        reply.cache_contract != DRE_GPU_CACHE_OWNED_NORMAL_NC_PERSISTENT)
        return false;
    *registry_id = expected;
    return true;
}
static id init_with_accelerator(id self, SEL, io_service_t accelerator)
{
    /* The factory owns this initial allocation. Its returned object is the
     * actual adapted frontend's _MTLDevice subclass, with the original queried
     * registryID. No inherited abstract init is reported as successful. */
    [self release];
    if (constructing) return nil;
    std::lock_guard<std::mutex> lock(factory_lock);
    struct Scope { Scope() { constructing = true; } ~Scope() { constructing = false; } } scope;
    uint64_t registry_id = 0;
    if (!native_contract(accelerator, &registry_id)) return nil;
    NSBundle *bundle = [NSBundle bundleForClass:[DREMetalBootstrap class]];
    NSString *loader = [bundle pathForResource:@"libvulkan_freedreno" ofType:@"dylib"];
    if (!loader || ![loader isAbsolutePath]) return nil;
    NSArray<NSString *> *caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    if (![caches count]) return nil;
    NSString *cache = [caches[0] stringByAppendingPathComponent:@"DREMetal306"];
    NSError *error = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:cache withIntermediateDirectories:YES
        attributes:nil error:&error]) return nil;
    id device = native_bridge_metal_open([loader fileSystemRepresentation],
        [cache fileSystemRepresentation], registry_id, nullptr);
    Class base = objc_getClass("_MTLDevice");
    Protocol *public_protocol = objc_getProtocol("MTLDevice"), *private_protocol = objc_getProtocol("MTLDeviceSPI");
    if (!device || !base || ![device isKindOfClass:base] || !public_protocol || !private_protocol ||
        ![device conformsToProtocol:public_protocol] || ![device conformsToProtocol:private_protocol])
        return nil;
    return [device retain];
}
@implementation DREMetalBootstrap
+ (void)load
{
    Class base = objc_getClass("_MTLDevice");
    if (!base || class_getInstanceSize(base) != 528 || objc_getClass("DREMetalDevice")) return;
    Class plugin = objc_allocateClassPair(base, "DREMetalDevice", 0);
    if (!plugin) return;
    /* io_service_t is the original factory's 32-bit Mach port argument. No
     * private Apple header is copied, and this signature is not guessed from
     * an NSObject object-return forwarding fallback. */
    if (!class_addMethod(plugin, sel_registerName("initWithAcceleratorPort:"),
        (IMP)init_with_accelerator, "@@:I")) { objc_disposeClassPair(plugin); return; }
    objc_registerClassPair(plugin);
}
@end
