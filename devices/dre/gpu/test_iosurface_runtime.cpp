// SPDX-License-Identifier: MIT
// Instrumented IOKit boundary; method bodies are extracted from the actual
// incremental kernel patch by test_iosurface.py, not reimplemented here.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
using UInt32=uint32_t; using SInt32=int32_t; using task_t=uintptr_t;
using mach_port_name_t=uint32_t; using user_addr_t=uintptr_t; using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnNoResources=1,kIOReturnBadArgument=2,
    kIOReturnNotFound=3,kIOReturnNoMemory=4,kIOReturnVMError=5,kIOReturnNoSpace=6,
    kIOReturnUnsupported=7,kIOReturnNotOpen=8;
constexpr int kIOMapAnywhere=1,kIOMapReadOnly=2,kIOSurfaceLockReadOnly=1;
constexpr int kSweetSurfaceCreate=0,kSweetSurfaceLookup=4,kSweetSurfaceLimits=13,
    kSweetSurfaceLookupFromPort=34,kSweetSurfaceCreatePort=35;
#define APPLE_KEXT_OVERRIDE override
#define OSDeclareDefaultStructors(C)
#define OSTypeAlloc(C) new C()
#define OSSafeReleaseNULL(p) do { if(p) { (p)->release(); (p)=nullptr; } } while(0)
#define OSDynamicCast(C,p) dynamic_cast<C *>(p)
#define __unused __attribute__((unused))
static std::atomic<unsigned> liveObjects{0},liveMaps{0};
struct OSObject {
    std::atomic<unsigned> refs{1};
    OSObject() { liveObjects++; }
    virtual ~OSObject() { liveObjects--; }
    virtual bool init() { return true; }
    virtual void free() { delete this; }
    void retain() { assert(refs.fetch_add(1)>0); }
    void release() { unsigned n=refs.fetch_sub(1); assert(n>0); if(n==1) free(); }
};
using IOLock=std::mutex;
static IOLock *IOLockAlloc() { return new IOLock; }
static void IOLockFree(IOLock *p) { delete p; }
static void IOLockLock(IOLock *p) { p->lock(); }
static void IOLockUnlock(IOLock *p) { p->unlock(); }
struct IOService:OSObject {};
struct IOExternalTrap {};
struct IOExternalMethodDispatch {};
struct Memory;
struct IOExternalMethodArguments {
    uint32_t scalarInputCount=0,scalarOutputCount=0;
    uint64_t *scalarInput=nullptr,*scalarOutput=nullptr;
    uint32_t structureInputSize=0,structureOutputSize=0,structureOutputDescriptorSize=0;
    void *structureInput=nullptr,*structureOutput=nullptr;
    Memory *structureInputDescriptor=nullptr,*structureOutputDescriptor=nullptr;
};
struct IOUserClient:IOService {
    virtual bool initWithTask(task_t,void *,UInt32) { return true; }
    virtual IOReturn clientClose() { return 0; }
    virtual IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *) { return 0; }
    virtual IOExternalTrap *getTargetAndTrapForIndex(IOService **,UInt32) { return nullptr; }
    void terminate() {}
    static IOReturn copyObjectForPortNameInTask(task_t,mach_port_name_t,OSObject **o) { *o=nullptr; return kIOReturnNotFound; }
};
static std::mutex hookLock;
static std::function<void()> mapHook,allocHook;
static std::atomic<bool> failSet{false},failAlloc{false},failMap{false};
static void hook(std::function<void()> &slot)
{ std::function<void()> cb; { std::lock_guard<std::mutex> lock(hookLock); cb=slot; } if(cb) cb(); }
[[maybe_unused]] static void setHook(std::function<void()> &slot,std::function<void()> value)
{ std::lock_guard<std::mutex> lock(hookLock); slot=std::move(value); }
struct IOMemoryMap:OSObject {
    Memory *memory;
    explicit IOMemoryMap(Memory *m);
    ~IOMemoryMap() override;
    uintptr_t getAddress();
};
struct Memory:OSObject {
    std::vector<uint8_t> bytes;
    explicit Memory(size_t n=4096):bytes(n) {}
    void *getBytesNoCopy() { return bytes.data(); }
    size_t getLength() { return bytes.size(); }
    size_t writeBytes(size_t off,const void *p,size_t n) { assert(off+n<=bytes.size()); memcpy(bytes.data()+off,p,n); return n; }
    IOMemoryMap *createMappingInTask(task_t,uint64_t,int) {
        hook(mapHook); if(failMap.exchange(false)) return nullptr; return new IOMemoryMap(this);
    }
};
IOMemoryMap::IOMemoryMap(Memory *m):memory(m) { m->retain(); liveMaps++; }
IOMemoryMap::~IOMemoryMap() { memory->release(); liveMaps--; }
uintptr_t IOMemoryMap::getAddress() { assert(refs.load()>0); return (uintptr_t)memory->getBytesNoCopy(); }
struct OSArray:OSObject {
    std::vector<OSObject *> entries;
    static OSArray *withCapacity(unsigned) { return new OSArray; }
    unsigned getCount() { return (unsigned)entries.size(); }
    OSObject *getObject(unsigned i) { return entries.at(i); }
    bool setObject(OSObject *o) { if(failSet.exchange(false)) return false; o->retain(); entries.push_back(o); return true; }
    void removeObject(unsigned i) { auto *o=entries.at(i); entries.erase(entries.begin()+i); o->release(); }
    void flushCollection() { while(!entries.empty()) removeObject((unsigned)entries.size()-1); }
    ~OSArray() override { flushCollection(); }
};
struct sweet_surface_plane {
    uint32_t width=0,height=0,bytesPerRow=0,offset=0,size=0;
    uint16_t bytesPerElement=0; uint8_t elementWidth=0,elementHeight=0;
};
struct SweetSurface:OSObject {
    uint32_t id=1,width=64,height=64,bytesPerRow=256,offset=0,pixelFormat=0x42475241,allocSize=16384;
    uint16_t bytesPerElement=4; uint8_t elementWidth=1,elementHeight=1;
    uint32_t cacheMode=0,planeCount=0,parentID=0;
    sweet_surface_plane planes[20];
    Memory *memory=new Memory(16384),*state=new Memory;
    int mapCache=0;
    ~SweetSurface() override { memory->release(); state->release(); }
};
static IOLock *sweet_surface_lock=IOLockAlloc();
static SweetSurface *sweet_surface_table[4096];
static uint32_t sweet_surface_users[4096];
static mach_port_name_t sweet_surface_port_in_task(SweetSurface *s,task_t) { return s->id+100; }
static void sweet_surf_log_once(const char *,uint32_t) {}
static void *IOMallocZeroData(size_t n) { hook(allocHook); return failAlloc.exchange(false) ? nullptr : calloc(1,n); }
static void IOFreeData(void *p,size_t) { std::free(p); }
static int copyout(const void *p,user_addr_t out,size_t n) { if(out<4096) return 1; memcpy((void *)out,p,n); return 0; }
static int32_t OSIncrementAtomic(volatile int32_t *p) { return __atomic_fetch_add(p,1,__ATOMIC_SEQ_CST); }
#include "surface_selected.inc"

static void demand(bool condition,const char *message)
{ if(!condition) { fprintf(stderr,"FAIL %s\n",message); std::abort(); } }
static SweetSurface *surface()
{
    auto *s=new SweetSurface;
    demand(!sweet_surface_table[1],"fixture table empty");
    s->retain(); sweet_surface_table[1]=s; sweet_surface_users[1]=1;
    return s;
}
static IOSurfaceRootUserClient *client()
{ auto *c=new IOSurfaceRootUserClient(); demand(c->initWithTask(1,nullptr,0),"client init"); return c; }
static SweetSurfaceRef *add(IOSurfaceRootUserClient *c,SweetSurface *s,bool &existing,bool incoming=true)
{
    if(incoming) demand(sweet_surface_get(s),"incoming user count");
    SweetSurfaceRef *ref=nullptr;
    demand(c->addRef(s,&ref,&existing)==kIOReturnSuccess,"addRef success");
    return ref;
}
[[maybe_unused]] static void finish(IOSurfaceRootUserClient *c) { c->clientClose(); c->release(); }
[[maybe_unused]] static void releaseMapping(IOSurfaceRootUserClient *c,uint32_t id=1)
{ demand(c->trapRelease((void *)(uintptr_t)id,nullptr,nullptr,nullptr,nullptr,nullptr)==0,"release mapped ref"); }
static void clean(SweetSurface *s)
{ s->release(); demand(!sweet_surface_table[1] && !sweet_surface_users[1],"no global surface users leaked"); demand(liveMaps==0,"no task mappings leaked"); }
struct Gate {
    std::mutex mutex; std::condition_variable cv; unsigned arrivals=0; bool proceed=false;
    void block() { std::unique_lock<std::mutex> l(mutex); arrivals++; cv.notify_all(); cv.wait(l,[&]{return proceed;}); }
    void wait(unsigned n) { std::unique_lock<std::mutex> l(mutex); cv.wait(l,[&]{return arrivals>=n;}); }
    void open() { std::lock_guard<std::mutex> l(mutex); proceed=true; cv.notify_all(); }
};
[[maybe_unused]] static uint64_t stateUse(SweetSurface *s)
{ return __atomic_load_n((uint64_t *)((uint8_t *)s->state->getBytesNoCopy()+0x18),__ATOMIC_ACQUIRE); }
int main()
{
    unsigned cases=0;
    { auto *s=surface(); auto *c=client(); bool existing;
      auto *ref=add(c,s,existing,false);
      demand(!existing,"first mapping is new");
      demand(ref->refs==2,"result-local retain survives client close");
      c->clientClose(); demand(liveMaps==2,"local retained result still mapped after close");
      uint8_t out[DRE_SURFACE_RESULT_SIZE]; sweet_surface_fill_result(s,ref,out);
      demand(out[0x18]==1,"result remains readable"); ref->release(); c->release(); clean(s); cases++; }
#if SURFACE_FIXED
    { auto *s=surface(); auto *c=client(); bool existing;
      auto *first=add(c,s,existing,false); auto *second=add(c,s,existing);
      demand(existing && first==second && first->count==1,"cached lookup has one mapping/user count");
      demand(sweet_surface_users[1]==1,"cached lookup balances incoming surface user");
      first->release(); second->release(); finish(c); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); Gate gate;
      setHook(mapHook,[&]{gate.block();});
      SweetSurfaceRef *refs[2]={}; bool existing[2]={};
      demand(sweet_surface_get(s),"second racing incoming user");
      std::thread a([&]{demand(c->addRef(s,&refs[0],&existing[0])==0,"first racing mapping");});
      std::thread b([&]{demand(c->addRef(s,&refs[1],&existing[1])==0,"second racing mapping");});
      gate.wait(2); gate.open(); a.join(); b.join(); setHook(mapHook,{});
      demand(refs[0]==refs[1] && existing[0]!=existing[1],"raced lookup uses cached result");
      demand(refs[0]->count==1 && liveMaps==2 && sweet_surface_users[1]==1,"raced losing mappings released");
      refs[0]->release(); refs[1]->release(); finish(c); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); Gate gate; setHook(mapHook,[&]{gate.block();});
      int result=-1; std::thread t([&]{SweetSurfaceRef *ref=nullptr;bool existing;result=c->addRef(s,&ref,&existing);});
      gate.wait(1); c->clientClose(); gate.open(); t.join(); setHook(mapHook,{});
      demand(result==kIOReturnNotOpen && liveMaps==0,"close rejects in-flight new mapping");
      c->release(); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); Gate gate; setHook(allocHook,[&]{gate.block();});
      uint8_t output[DRE_SURFACE_RESULT_SIZE]; IOExternalMethodArguments args;
      args.structureOutput=output; args.structureOutputSize=sizeof(output);
      int result=-1; std::thread t([&]{result=c->returnSurface(s,&args);});
      gate.wait(1); c->clientClose(); demand(liveMaps==2,"result copy pins maps while close runs");
      gate.open(); t.join(); setHook(allocHook,{});
      demand(result==0 && liveMaps==0,"result copy completes then releases mapping retain");
      c->release(); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); SweetSurfaceRef *replacement=nullptr;
      setHook(allocHook,[&]{
        setHook(allocHook,{}); releaseMapping(c); bool existing;
        replacement=add(c,s,existing); demand(!existing,"replacement is new mapping"); failAlloc=true;
      });
      IOExternalMethodArguments args;
      demand(c->returnSurface(s,&args)==kIOReturnNoMemory,"failed result allocation");
      demand(c->fRefs->getCount()==1 && c->fRefs->getObject(0)==replacement,"rollback preserves replacement mapping");
      replacement->release(); finish(c); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); failSet=true; SweetSurfaceRef *ref=nullptr; bool existing;
      demand(c->addRef(s,&ref,&existing)==kIOReturnNoMemory,"array insertion failure returned");
      demand(!c->fRefs->getCount() && !liveMaps,"array failure cleans both task mappings");
      finish(c); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); failMap=true; SweetSurfaceRef *ref=nullptr; bool existing;
      demand(c->addRef(s,&ref,&existing)==kIOReturnVMError,"mapping failure returned");
      demand(!c->fRefs->getCount() && !liveMaps,"mapping failure cleans other mapping");
      finish(c); clean(s); cases++; }
    { auto *s=surface(); auto *a=client(); auto *b=client(); bool existing;
      auto *ra=add(a,s,existing,false); auto *rb=add(b,s,existing);
      ra->release(); rb->release();
      demand(a->changeUseCount(1,0,true)==0 && a->changeUseCount(1,0,true)==0,"category0 increments");
      demand(b->changeUseCount(1,0,true)==0 && b->changeUseCount(1,1,true)==0,"second client/categories");
      demand(stateUse(s)==(UINT64_C(1)<<32)+3,"packed shared categories visible");
      demand(a->changeUseCount(1,0,false)==0 && stateUse(s)==(UINT64_C(1)<<32)+2,"category decrement");
      demand(a->changeUseCount(1,1,false)==kIOReturnBadArgument,"client cannot decrement another category owner");
      demand(a->changeUseCount(1,2,true)==kIOReturnBadArgument,"category bounds");
      finish(a); demand(stateUse(s)==(UINT64_C(1)<<32)+1,"close removes only own outstanding uses");
      finish(b); demand(stateUse(s)==0,"last client close clears uses"); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); bool existing; auto *ref=add(c,s,existing,false); ref->release();
      auto *shared=(uint64_t *)((uint8_t *)s->state->getBytesNoCopy()+0x18);
      *shared=UINT32_MAX;
      demand(c->changeUseCount(1,0,true)==kIOReturnNoResources && *shared==UINT32_MAX,"global use count cannot wrap");
      *shared=0;
      demand(c->changeUseCount(1,0,false)==kIOReturnBadArgument && !*shared,"use count cannot underflow");
      finish(c); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); bool existing; auto *ref=add(c,s,existing,false); ref->release();
      uint64_t scalar[2]={99,88}; uint8_t data[16]; memset(data,0xff,sizeof(data));
      IOExternalMethodArguments args;args.scalarOutput=scalar;args.scalarOutputCount=2;
      args.structureOutput=data;args.structureOutputSize=sizeof(data);
      for(unsigned selector:{12u,36u,37u,38u,39u,40u,41u,42u,45u}) {
        demand(c->externalMethod(selector,&args,nullptr,nullptr,nullptr)==kIOReturnUnsupported,"unsupported GPU/event selector fails");
        demand(!scalar[0] && !scalar[1] && !data[0],"unsupported GPU outputs cleared");
      }
      demand(c->externalMethod(9,&args,nullptr,nullptr,nullptr)==0,"CPU metadata compatibility unchanged");
      finish(c); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); bool existing; auto *ref=add(c,s,existing,false); ref->release();
      uint32_t seed=123;
      demand(c->trapLock((void *)1,nullptr,&seed,nullptr,nullptr,nullptr)==0 && seed==0,"CPU lock returns current seed");
      demand(c->trapUnlock((void *)1,(void *)1,&seed,nullptr,nullptr,nullptr)==0 && seed==0,"read-only unlock preserves seed");
      demand(c->trapUnlock((void *)1,nullptr,&seed,nullptr,nullptr,nullptr)==0 && seed==1,"CPU write unlock advances seed");
      finish(c); clean(s); cases++; }
    { auto *s=surface(); auto *c=client(); sweet_surface_put(s); // each worker acquires its own incoming user count
      std::vector<std::thread> threads;
      for(unsigned worker=0;worker<8;worker++) threads.emplace_back([&]{
        for(unsigned i=0;i<1000;i++) {
          bool existing; auto *ref=add(c,s,existing);
          uint8_t out[DRE_SURFACE_RESULT_SIZE]; sweet_surface_fill_result(s,ref,out);
          demand(out[0x18]==1,"concurrent result readable");
          c->trapRelease((void *)1,nullptr,nullptr,nullptr,nullptr,nullptr);
          ref->release();
        }
      });
      for(auto &t:threads) { t.join(); }
      finish(c); clean(s); cases++; }
#endif
    demand(liveObjects==0,"all OSObject references released");
    IOLockFree(sweet_surface_lock);
    printf("PASS %u actual IOSurface method scenarios, 8000 concurrent lookup/release operations, ASan+UBSan; CPU lifetime only\n",cases);
}
