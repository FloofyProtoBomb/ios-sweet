/* SPDX-License-Identifier: MIT
 * Narrow real IOKitUser ABI, using public SDK Mach/CoreFoundation types.
 * iPhoneOS SDKs do not consistently ship the macOS IOKitLib.h header/stub.
 * Exact declarations: Apple IOKitUser323ead896d04424f87184d8f6ff0cce811aab106,
 * IOKitLib.h SHA256d97b87a7d557a4281436c1a92e26abf6651242786887f03d6baa6eaff4b20e86.
 * No library/header copied, fake connection, weak import or unresolved-link waiver.
 */
#ifndef DRE_IOKIT_LOADER_H
#define DRE_IOKIT_LOADER_H
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
static_assert(sizeof(mach_port_t)==4,"IOKitUser port ABI");
#else
_Static_assert(sizeof(mach_port_t)==4,"IOKitUser port ABI");
#endif
typedef CFMutableDictionaryRef (*dre_iokit_matching_fn)(const char *);
typedef mach_port_t (*dre_iokit_service_fn)(mach_port_t,CFDictionaryRef);
typedef kern_return_t (*dre_iokit_release_fn)(mach_port_t);
typedef boolean_t (*dre_iokit_conforms_fn)(mach_port_t,const char *);
typedef kern_return_t (*dre_iokit_id_fn)(mach_port_t,uint64_t *);
typedef kern_return_t (*dre_iokit_open_fn)(mach_port_t,task_port_t,uint32_t,mach_port_t *);
typedef kern_return_t (*dre_iokit_close_fn)(mach_port_t);
typedef kern_return_t (*dre_iokit_call_fn)(mach_port_t,uint32_t,const void *,size_t,void *,size_t *);
struct dre_iokit_api {
    void *library; /* Process-lifetime reference, never dlclosed under clients. */
    dre_iokit_matching_fn matching;
    dre_iokit_service_fn service;
    dre_iokit_release_fn release;
    dre_iokit_conforms_fn conforms;
    dre_iokit_id_fn registry_id;
    dre_iokit_open_fn open;
    dre_iokit_close_fn close;
    dre_iokit_call_fn call;
    int available;
};
/* Each linked consumer owns its own immutable once-initialized API table. No
 * pointer is exported as a provider or GPU capability. Missing ANY actual
 * entrypoint refuses use; there is no fallback image/search path or stub. */
static struct dre_iokit_api dre_iokit_table;
static pthread_once_t dre_iokit_once=PTHREAD_ONCE_INIT;
static void dre_iokit_load(void)
{
    struct dre_iokit_api *a=&dre_iokit_table;
    a->library=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    if(!a->library)return;
#define DRE_IOKIT_LOAD(field,type,name) a->field=(type)dlsym(a->library,name);if(!a->field)return
    DRE_IOKIT_LOAD(matching,dre_iokit_matching_fn,"IOServiceMatching");
    DRE_IOKIT_LOAD(service,dre_iokit_service_fn,"IOServiceGetMatchingService");
    DRE_IOKIT_LOAD(release,dre_iokit_release_fn,"IOObjectRelease");
    DRE_IOKIT_LOAD(conforms,dre_iokit_conforms_fn,"IOObjectConformsTo");
    DRE_IOKIT_LOAD(registry_id,dre_iokit_id_fn,"IORegistryEntryGetRegistryEntryID");
    DRE_IOKIT_LOAD(open,dre_iokit_open_fn,"IOServiceOpen");
    DRE_IOKIT_LOAD(close,dre_iokit_close_fn,"IOServiceClose");
    DRE_IOKIT_LOAD(call,dre_iokit_call_fn,"IOConnectCallStructMethod");
#undef DRE_IOKIT_LOAD
    a->available=1;
}
#define DRE_IOKIT_INLINE static inline __attribute__((unused))
DRE_IOKIT_INLINE const struct dre_iokit_api *dre_iokit_functions(void)
{
    if(pthread_once(&dre_iokit_once,dre_iokit_load)||!dre_iokit_table.available)return NULL;
    return &dre_iokit_table;
}
DRE_IOKIT_INLINE CFMutableDictionaryRef dre_iokit_matching(const char *name)
{
    const struct dre_iokit_api *a=dre_iokit_functions();
    return a&&name?a->matching(name):NULL;
}
/* Like the authoritative API, consumes the supplied matching dictionary even
 * on failure. MACH_PORT_NULL is the real default main/master port, not a
 * fabricated service/connection right. Returned object requires release. */
DRE_IOKIT_INLINE mach_port_t dre_iokit_matching_service(CFDictionaryRef matching)
{
    if(!matching)return MACH_PORT_NULL;
    const struct dre_iokit_api *a=dre_iokit_functions();
    if(!a) {CFRelease(matching);return MACH_PORT_NULL;}
    return a->service(MACH_PORT_NULL,matching);
}
DRE_IOKIT_INLINE kern_return_t dre_iokit_release(mach_port_t object)
{
    const struct dre_iokit_api *a=dre_iokit_functions();
    return a?a->release(object):KERN_NOT_SUPPORTED;
}
DRE_IOKIT_INLINE boolean_t dre_iokit_conforms(mach_port_t object,const char *name)
{
    const struct dre_iokit_api *a=dre_iokit_functions();
    return a&&name?a->conforms(object,name):0;
}
DRE_IOKIT_INLINE kern_return_t dre_iokit_registry_id(mach_port_t entry,uint64_t *id)
{
    const struct dre_iokit_api *a=dre_iokit_functions();
    if(!id)return KERN_INVALID_ARGUMENT;
    *id=0;return a?a->registry_id(entry,id):KERN_NOT_SUPPORTED;
}
DRE_IOKIT_INLINE kern_return_t dre_iokit_open(mach_port_t service,task_port_t task,
    uint32_t type,mach_port_t *connection)
{
    const struct dre_iokit_api *a=dre_iokit_functions();
    if(!connection)return KERN_INVALID_ARGUMENT;
    *connection=MACH_PORT_NULL;return a?a->open(service,task,type,connection):KERN_NOT_SUPPORTED;
}
DRE_IOKIT_INLINE kern_return_t dre_iokit_close(mach_port_t connection)
{
    const struct dre_iokit_api *a=dre_iokit_functions();
    return a?a->close(connection):KERN_NOT_SUPPORTED;
}
DRE_IOKIT_INLINE kern_return_t dre_iokit_call(mach_port_t connection,uint32_t selector,
    const void *in,size_t in_bytes,void *out,size_t *out_bytes)
{
    const struct dre_iokit_api *a=dre_iokit_functions();
    if(!out_bytes)return KERN_INVALID_ARGUMENT;
    return a?a->call(connection,selector,in,in_bytes,out,out_bytes):KERN_NOT_SUPPORTED;
}
#undef DRE_IOKIT_INLINE
#endif
#endif
