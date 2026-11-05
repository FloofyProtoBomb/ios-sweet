/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source. Not compiled, linked or executed.
 * Real selected iOS20H392 MTLCompiler306 Create/BuildRequest connection.
 * No private Apple header, copied implementation, source-to-GPU claim, latest
 * compiler search, request14 guess, cache bypass or dynamic-lookup link waiver.
 * Compile with ARC, blocks and C++17. The exact image/ABI still needs runtime
 * qualification before this connection can be part of a published device.
 */
#import "compiler_service.h"
#include "compiler_request.h"
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <pthread.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <string.h>

namespace {
constexpr size_t kReplyLimit=64u<<20,kMessageLimit=4096,kStackBytes=16u<<20;
const uint8_t kCompilerUUID[16]={0x0a,0xcf,0xc8,0x72,0x80,0x27,0x3d,0xfd,
    0x8f,0x30,0x39,0x7c,0x9c,0xb2,0x56,0x39};
const char kCompilerPath[]="/System/Library/PrivateFrameworks/MTLCompiler.framework/MTLCompiler";
typedef void (^CodegenReply)(int32_t,const void *,size_t,const char *);
typedef void *(*CreateService)(const char *);
/* Selected BuildRequest preserves arg1 as a32-bit word before dispatch;
 * this connection uses flags0 only. It does not infer another ABI/profile. */
typedef void (*BuildRequest)(void *,uint32_t,uint32_t,const void *,size_t,CodegenReply);
struct Service {
    void *library=nullptr,*object=nullptr;
    BuildRequest build=nullptr;
    NSString *__strong failure=nil;
};
Service service;
std::once_flag serviceOnce;
std::mutex serviceMutex;
std::atomic<bool> poisoned{false};

NSError *failure(NSInteger code,NSString *why)
{
    return [NSError errorWithDomain:@"org.iossweet.dre.metal306.compiler" code:code
        userInfo:@{NSLocalizedDescriptionKey:why?:@"local compiler request refused"}];
}
uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
bool zero(const uint8_t *p,size_t n)
{
    for(size_t i=0;i<n;i++)if(p[i])return false;
    return true;
}
bool request13(const void *data,size_t bytes)
{
    if(!data||bytes<16)return false;
    const uint8_t *p=(const uint8_t *)data;
    uint32_t source=word(p),metadata=word(p+4),arguments=word(p+8),flags=word(p+12);
    size_t required=0;
    if(source<2||arguments<2||metadata||flags||
       !dre_metal306_request13_size(source-1,arguments-1,&required)||required!=bytes)return false;
    size_t padded=((size_t)source+3)&~(size_t)3;
    return p[16+source-1]==0&&!memchr(p+16,0,source-1)&&
        zero(p+16+source,padded-source)&&p[16+padded+arguments-1]==0&&
        !memchr(p+16+padded,0,arguments-1);
}
bool selectedImage(const void *symbol,const void **base)
{
    Dl_info info={};
    if(!symbol||!dladdr(symbol,&info)||!info.dli_fbase)return false;
    const mach_header_64 *header=(const mach_header_64 *)info.dli_fbase;
    if(header->magic!=MH_MAGIC_64||header->cputype!=CPU_TYPE_ARM64||
       header->cpusubtype!=CPU_SUBTYPE_ARM64_ALL||header->filetype!=MH_DYLIB||
       !header->ncmds||header->ncmds>4096||header->sizeofcmds>(4u<<20))return false;
    const uint8_t *commands=(const uint8_t *)(header+1);size_t at=0;
    bool found=false;
    for(uint32_t i=0;i<header->ncmds;i++) {
        if(at>header->sizeofcmds||header->sizeofcmds-at<sizeof(load_command))return false;
        const load_command *command=(const load_command *)(commands+at);
        if(command->cmdsize<sizeof(load_command)||(command->cmdsize&3u)||
           command->cmdsize>header->sizeofcmds-at)return false;
        if(command->cmd==LC_UUID) {
            if(found||command->cmdsize!=sizeof(uuid_command)||
               memcmp(((const uuid_command *)command)->uuid,kCompilerUUID,16))return false;
            found=true;
        }
        at+=command->cmdsize;
    }
    if(!found||at!=header->sizeofcmds)return false;
    *base=info.dli_fbase;return true;
}
void openService()
{
    service.library=dlopen(kCompilerPath,RTLD_NOW|RTLD_LOCAL);
    if(!service.library) {service.failure=@"selected20H392 local compiler image is unavailable";return;}
    CreateService create=(CreateService)dlsym(service.library,"MTLCodeGenServiceCreate");
    service.build=(BuildRequest)dlsym(service.library,"MTLCodeGenServiceBuildRequest");
    const void *createBase=nullptr,*buildBase=nullptr;
    if(!selectedImage((const void *)create,&createBase)||
       !selectedImage((const void *)service.build,&buildBase)||createBase!=buildBase) {
        service.failure=@"compiler Create/BuildRequest does not match the selected20H392 compiler UUID/ARM64 image";
        service.build=nullptr;dlclose(service.library);service.library=nullptr;return;
    }
    @try {service.object=create("dre-metal306");}
    @catch(NSException *exception) {
        service.failure=[NSString stringWithFormat:@"compiler service creation exception: %@",exception.name];
        poisoned.store(true,std::memory_order_release);return;
    }
    if(!service.object)service.failure=@"selected compiler service creation failed";
    /* A created service has process lifetime. No destructor ABI is guessed,
     * and its loaded image is never unloaded while callbacks can retain it. */
}
struct Job {
    std::mutex lock;
    NSData *__strong input=nil;
    NSData *__strong library=nil;
    NSString *__strong warnings=nil;
    NSString *__strong error=nil;
    bool called=false,multiple=false;
};
struct Worker {
    std::shared_ptr<Job> job;
    explicit Worker(std::shared_ptr<Job> value):job(std::move(value)) {}
};
void *run(void *argument)
{
    std::unique_ptr<Worker> worker((Worker *)argument);
    std::shared_ptr<Job> job=worker->job;
    @autoreleasepool {
        @try {
            service.build(service.object,0,13,job->input.bytes,job->input.length,
                ^(int32_t status,const void *reply,size_t bytes,const char *why) {
                std::lock_guard<std::mutex> guard(job->lock);
                if(job->called) {
                    job->multiple=true;job->library=nil;
                    job->error=@"compiler returned multiple replies for one request";return;
                }
                job->called=true;
                struct dre_metal306_reply view={};
                if(bytes>kReplyLimit||!dre_metal306_reply(status,reply,bytes,&view)) {
                    NSString *detail=nil;
                    if(why) {
                        size_t n=strnlen(why,kMessageLimit);
                        if(n<kMessageLimit)detail=[[NSString alloc] initWithBytes:why length:n encoding:NSUTF8StringEncoding];
                    }
                    job->error=detail.length?detail:[NSString stringWithFormat:
                        @"compiler error or invalid Metal306 archive reply (status %d)",status];return;
                }
                if(view.warning_bytes) {
                    job->warnings=[[NSString alloc] initWithBytes:view.warnings length:view.warning_bytes encoding:NSUTF8StringEncoding];
                    if(!job->warnings) {job->error=@"compiler warning text is not valid UTF-8";return;}
                }
                /* Actual callback-owned bytes are copied before returning.
                 * The compiler can release its reply immediately afterwards. */
                job->library=[NSData dataWithBytes:view.metallib length:view.metallib_bytes];
                if(!job->library)job->error=@"unable to copy the compiler archive reply";
            });
        } @catch(NSException *exception) {
            std::lock_guard<std::mutex> guard(job->lock);
            job->library=nil;job->error=[NSString stringWithFormat:@"compiler exception: %@",exception.name];
            poisoned.store(true,std::memory_order_release);
        }
    }
    return nullptr;
}
}

NSData *dre_metal306_compile_request(uint32_t type,const void *data,size_t bytes,
    NSString **warnings,NSError **error)
{
    if(warnings)*warnings=nil;if(error)*error=nil;
    if(type!=13) {
        if(error)*error=failure(1,@"request type is not qualified for Metal306; stitched request14 is unsupported");return nil;
    }
    if(!request13(data,bytes)) {if(error)*error=failure(2,@"invalid bounded Metal306 request13 packet");return nil;}
    std::lock_guard<std::mutex> serial(serviceMutex);
    if(poisoned.load(std::memory_order_acquire)) {
        if(error)*error=failure(3,@"compiler service ownership is uncertain after an earlier failure");return nil;
    }
    std::call_once(serviceOnce,openService);
    if(!service.object||!service.build) {if(error)*error=failure(4,service.failure);return nil;}
    std::shared_ptr<Job> job;
    try {job=std::make_shared<Job>();}
    catch(const std::bad_alloc &) {if(error)*error=failure(5,@"compiler job allocation failed");return nil;}
    job->input=[NSData dataWithBytes:data length:bytes];
    if(!job->input) {if(error)*error=failure(5,@"compiler request allocation failed");return nil;}
    Worker *worker=new(std::nothrow)Worker(job);
    if(!worker) {if(error)*error=failure(5,@"compiler worker allocation failed");return nil;}
    pthread_attr_t attributes;pthread_t thread;
    int rc=pthread_attr_init(&attributes);
    if(rc) {delete worker;if(error)*error=failure(6,@"compiler worker attributes unavailable");return nil;}
    rc=pthread_attr_setstacksize(&attributes,kStackBytes);
    if(!rc)rc=pthread_create(&thread,&attributes,run,worker);
    pthread_attr_destroy(&attributes);
    if(rc) {
        delete worker;if(error)*error=failure(6,@"unable to create a 16 MiB compiler worker; caller-stack fallback refused");return nil;
    }
    /* Selected BuildRequest calls back before returning. There is no guessed
     * cancellation/kill ABI: an in-progress real compiler cannot be timed out
     * safely by freeing its request/service. Input and callback state are heap
     * owned by the worker/block if joining unexpectedly fails. */
    if(pthread_join(thread,nullptr)) {
        poisoned.store(true,std::memory_order_release);
        if(error)*error=failure(7,@"compiler worker join failed; live ownership retained and service poisoned");return nil;
    }
    std::lock_guard<std::mutex> guard(job->lock);
    if(!job->called||job->multiple||job->error||!job->library) {
        if(!job->called||job->multiple)poisoned.store(true,std::memory_order_release);
        if(error)*error=failure(8,job->error?:@"compiler did not return one valid Metal306 archive");return nil;
    }
    if(warnings)*warnings=job->warnings;
    return job->library;
}
