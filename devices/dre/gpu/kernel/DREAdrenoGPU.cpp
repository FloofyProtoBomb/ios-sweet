/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight draft: deliberately not compiled, staged or executed.
 * Actual XNU ownership/IOBuffer/IOMemoryMap glue, not an AGX or Metal device.
 */
#include "dre_gpu_provider.h"
#include "dre_gpu_vm_binding.h"
#include "dre_gpu_access_epoch.h"
#include "dre_surface_backing.h"
#include "allocation_catalog.h"
#include "../../native_board.h"
#if defined(DRE)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSArray.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <machine/memory_types.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include <vm/memory_types.h>
#include <mach/mach_time.h>
#if defined(DRE_GPU_NATIVE_PMAP_PROOF) && DRE_GPU_NATIVE_PMAP_PROOF
#include "dre_gpu_pmap_proof.h"
#endif
extern "C" {
#include "../../libfdt/libfdt.h"
#include "../../dma_memory.h"
vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned);
void ml_io_unmap(vm_offset_t,vm_size_t);
uint64_t kvtophys(vm_offset_t);
}

struct dre_gpu_native_allocation {
    dre_gpu_native_allocation *next;
    IOBufferMemoryDescriptor *descriptor;
    dre_gpu_native_memory view;
    size_t metadata_bytes;
    uint64_t *sortedPA;
    size_t sorted_bytes;
    bool prepared,published;
    uint64_t generation,taskGeneration,catalogPin;
    uint32_t purpose,access,catalogPins;
    IOUserClient *taskClient; /* real retained client, not a numeric task claim */
    const dre_a619_vm *mappedVM;
    const dre_a619_vm_ops *mappedOps;
    const dre_a619_vm_mapping *mappedHandle;
    dre_a619_vm_mapping mapped;
    struct {dre_gpu_native_allocation *owner;uint64_t generation;const void *address;size_t bytes;} mapStorage[8];
    uint32_t mapStorageCount;
};
struct DREMap { uint64_t pa,bytes; vm_offset_t cpu; };
struct DREBO {
    dre_gpu_native_allocation *allocation;
    dre_gpu_native_bo native;
    IOMemoryMap *cpuMap;
    uint64_t handle,mapLease;
    uint32_t pins,cpuAccess;
    dre_gpu_access_epoch accessEpoch;
    bool closed,unsafe,gpuRetired,persistentMap;
};
struct DREJob {
    uint64_t handle,ticket;
    dre_gpu_native_completion completion;
    DREBO *resources[DRE_GPU_MAX_BOS];
    DREJob *waits[DRE_GPU_MAX_WAITS];
    uint32_t resourceCount,waitCount,dependents;
    bool used,retired,uncertain;
};
struct DRESurface {
    void *pin;
    uint64_t token,chargedBytes;
    uint32_t access;
};
class DREAdrenoGPUClient;
class DREAdrenoGPU : public IOService {
    OSDeclareDefaultStructors(DREAdrenoGPU);
public:
    dre_gpu_native_provider native;
    dre_a619_startup startup;
    void *fdt;
    uint64_t fdtBytes,lease,nextSession;
    DREMap maps[6];
    IOLock *lock;
    thread_t lockHolder;
    OSArray *clients;
    IOWorkLoop *loop;
    IOTimerEventSource *timer;
    dre_gpu_native_allocation *allocations;
    dre_gpu_native_allocation *lastPAOwner,*exclusiveMetadata;
    dre_gpu_vm_binding *exclusiveVM;
    bool initialized,permanentHold,providerFault,ownershipAcquired;
    bool ownerLockBound;
    uint64_t nextAllocation,catalogGeneration,nextCatalogScope,activeCatalogScope;
    dre_gpu_allocation_catalog_snapshot *catalogSnapshot;
    DREAdrenoGPUClient *catalogTarget;
    bool initNative(const dre_gpu_native_provider *);
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **)
        APPLE_KEXT_OVERRIDE;
    bool owns();
    void ownerEnter();
    void ownerLeave();
    int ownerLockStatus();
    static void enterHook(void *);
    static void leaveHook(void *);
    static int lockStatusHook(void *);
    void info(dre_gpu_info_reply *);
    int allocate(uint64_t,uint32_t,dre_gpu_native_allocation **,
        dre_gpu_native_memory *,bool contiguous=false);
    void discard(dre_gpu_native_allocation *);
    int releaseRetired(dre_gpu_native_allocation *);
    int ownsCPU(const void *,size_t);
    int ownsRAM(uint64_t,size_t);
    int verifyMemory(const dre_gpu_native_memory *);
    bool kernelProofAvailable();
    bool taskProofAvailable();
    int kernelProof(const dre_gpu_native_memory *);
    int taskProof(task_t,uint64_t,const dre_gpu_native_memory *,uint32_t);
    static void poll(OSObject *,IOTimerEventSource *);
    static int read32(void *,uint64_t,uint32_t *);
    static int write32(void *,uint64_t,uint32_t);
    static int read64(void *,uint64_t,uint64_t *);
    static int write64(void *,uint64_t,uint64_t);
    static void barrier(void *);
    static uint64_t now(void *);
    static int allocHook(void *,uint64_t,uint32_t,dre_gpu_native_allocation **,
        dre_gpu_native_memory *);
    static int contiguousHook(void *,uint64_t,uint32_t,dre_gpu_native_allocation **,
        dre_gpu_native_memory *);
    static void discardHook(void *,dre_gpu_native_allocation *);
    static int ownsCPUHook(void *,const void *,size_t);
    static int ownsRAMHook(void *,uint64_t,size_t);
    static int verifyHook(void *,const dre_gpu_native_memory *);
    int classify(dre_gpu_native_allocation *,const dre_gpu_native_memory *,uint32_t);
    int classifyTask(dre_gpu_native_allocation *,DREAdrenoGPUClient *);
    int recordMapping(dre_gpu_native_allocation *,const dre_a619_vm *,const dre_a619_vm_ops *,const dre_a619_vm_mapping *);
    int mappingCurrent(dre_gpu_native_allocation *,dre_gpu_allocation_catalog_record *);
    int ownsPrivateCPU(const void *,size_t);
    dre_gpu_native_allocation *findAllocation(const dre_gpu_native_allocation *);
    dre_gpu_native_allocation *privateStorage(const void *,size_t);
    bool registeredClient(DREAdrenoGPUClient *,uint64_t);
    int withCatalog(const void *,uint64_t,dre_gpu_allocation_catalog_snapshot *,
        int (*)(void *,const dre_gpu_allocation_catalog_snapshot *),void *);
    int catalogCurrent(const dre_gpu_allocation_catalog_snapshot *);
    int catalogRecordCurrent(const dre_gpu_allocation_catalog_snapshot *,uint32_t);
    static int classifyHook(void *,dre_gpu_native_allocation *,const dre_gpu_native_memory *,dre_gpu_allocation_purpose);
    static int mappingHook(void *,dre_gpu_native_allocation *,const dre_a619_vm *,const dre_a619_vm_ops *,const dre_a619_vm_mapping *);
    static int catalogHook(void *,const void *,uint64_t,dre_gpu_allocation_catalog_snapshot *,
        int (*)(void *,const dre_gpu_allocation_catalog_snapshot *),void *);
    static int catalogCurrentHook(void *,const dre_gpu_allocation_catalog_snapshot *);
    static int catalogRecordHook(void *,const dre_gpu_allocation_catalog_snapshot *,uint32_t);
    static int privateCPUHook(void *,const void *,size_t);
};
class DREAdrenoGPUClient : public IOUserClient {
    OSDeclareDefaultStructors(DREAdrenoGPUClient);
public:
    DREAdrenoGPU *gpu;
    task_t owner;
    void *session;
    uint64_t generation,nextBO,nextMap,nextFence,wiredBytes,nextSurface,surfaceBytes;
    /* The bounded connection tables exceed XNU's 16KiB OSObject type limit.
     * They remain connection-owned until the real orphan/reap lifecycle ends. */
    DREBO *bos;
    DREJob *jobs;
    DRESurface surfaces[DRE_GPU_MAX_SURFACES];
    bool closing,sessionFault,exclusiveVM;
    bool initWithTask(task_t,void *,UInt32) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn clientDied() APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,
        IOExternalMethodDispatch *,OSObject *,void *) APPLE_KEXT_OVERRIDE;
    int dispatch(uint32_t,const void *,void *);
    DREBO *bo(uint64_t);
    DREJob *job(uint64_t);
    DRESurface *surface(uint64_t);
    int surfaceCall(uint32_t,const void *,void *);
    int closeSurface(DRESurface *);
    int unmap(DREBO *);
    int mapPersistent(const dre_gpu_cpu_map_request *,dre_gpu_cpu_map_reply *);
    int cpuAccess(uint32_t,const dre_gpu_cpu_access_request *,void *);
    int retireBO(DREBO *);
    int refresh(DREJob *);
    bool reap(); /* lock held; true iff closed session/resources all retired */
    int submit(const dre_gpu_submit_request *,dre_gpu_submit_reply *);
};
OSDefineMetaClassAndStructors(DREAdrenoGPU,IOService);
OSDefineMetaClassAndStructors(DREAdrenoGPUClient,IOUserClient);

static bool zeroBytes(const void *v,size_t bytes)
{
    const uint8_t *p=(const uint8_t *)v;
    for(size_t i=0;i<bytes;i++) if(p[i]) return false;
    return true;
}
static bool goodAccess(uint32_t a)
{
    return (a&DRE_GPU_ACCESS_READ) && !(a&~7u) &&
        !((a&DRE_GPU_ACCESS_WRITE)&&(a&DRE_GPU_ACCESS_EXECUTE));
}
static bool regPairs(const void *dt,int node,const uint64_t *pairs,unsigned n)
{
    int len=0,parent=fdt_parent_offset(dt,node);
    if(node<0||parent<0||fdt_address_cells(dt,parent)!=1||fdt_size_cells(dt,parent)!=1)
        return false;
    const fdt32_t *p=(const fdt32_t *)fdt_getprop(dt,node,"reg",&len);
    if(!p||len!=(int)(n*8)) return false;
    for(unsigned i=0;i<n*2;i++) if(fdt32_to_cpu(p[i])!=pairs[i]) return false;
    return true;
}
static int uniqueCompatible(const void *dt,const char *name)
{
    int n=fdt_node_offset_by_compatible(dt,-1,name);
    return n>=0 && fdt_node_offset_by_compatible(dt,n,name)==-FDT_ERR_NOTFOUND?n:-1;
}
static bool boardFDT(const void *dt,size_t bytes)
{
    if(bytes<40||bytes>(2u<<20)||fdt_check_full(dt,bytes)||fdt_totalsize(dt)!=bytes||
       fdt_node_check_compatible(dt,0,"qcom,holi")) return false;
    int len=0;
    const fdt32_t *ids=(const fdt32_t *)fdt_getprop(dt,0,"oplus,dtsi_no",&len);
    bool board=false;
    if(!ids||len<=0||len%4) return false;
    for(int i=0;i<len/4;i++) {
        uint32_t id=fdt32_to_cpu(ids[i]);
        if(id==133144||id==133180) board=true;
    }
    int gpu=uniqueCompatible(dt,"qcom,adreno-gpu-a619-holi");
    const fdt32_t *chip=(const fdt32_t *)fdt_getprop(dt,gpu,"qcom,chipid",&len);
    if(!board||!chip||len!=4||fdt32_to_cpu(*chip)!=DRE_A619_CHIP_ID) return false;
    const fdt32_t *irq=(const fdt32_t *)fdt_getprop(dt,gpu,"interrupts",&len);
    if(!irq||len!=12||fdt32_to_cpu(irq[0])!=0||fdt32_to_cpu(irq[1])!=177||
       fdt32_to_cpu(irq[2])!=4) return false; /* actual GIC INTID209 level-high */
    static const uint64_t gp[]={0x5900000,0x40000,0x5961000,0x800,
        0x596a000,0x30000,0x599e000,0x1000};
    static const uint64_t sm[]={0x5940000,0x10000};
    static const uint64_t cc[]={0x5990000,0x9000};
    static const uint64_t gc[]={0x1400000,0x1f0000};
    return regPairs(dt,gpu,gp,4)&&regPairs(dt,uniqueCompatible(dt,"qcom,kgsl-smmu-v2"),sm,1)&&
        regPairs(dt,uniqueCompatible(dt,"qcom,holi-gpucc"),cc,1)&&
        regPairs(dt,uniqueCompatible(dt,"qcom,holi-gcc"),gc,1);
}
bool DREAdrenoGPU::initNative(const dre_gpu_native_provider *p)
{
    native={}; startup={}; fdt=NULL; fdtBytes=lease=nextSession=0;
    bzero(maps,sizeof(maps));lock=NULL;clients=NULL;loop=NULL;timer=NULL;
    allocations=lastPAOwner=exclusiveMetadata=NULL;exclusiveVM=NULL;
    initialized=permanentHold=providerFault=ownershipAcquired=ownerLockBound=false;
    lockHolder=THREAD_NULL;
    nextAllocation=nextCatalogScope=activeCatalogScope=0;catalogGeneration=1;catalogSnapshot=NULL;catalogTarget=NULL;
    if(!p||p->version!=DRE_GPU_PROVIDER_VERSION||p->struct_bytes!=sizeof(*p)||
       p->use_exclusive_vm_adapter>1||p->vm_reserved_count>16||
       (p->vm_reserved_count&&!p->vm_reserved)||
       !p->fdt||!boardFDT(p->fdt,p->fdt_bytes)||!IOService::init()) return false;
    native=*p; fdtBytes=p->fdt_bytes;
    fdt=IOMalloc(fdtBytes);
    lock=IOLockAlloc(); clients=OSArray::withCapacity(4);
    if(!fdt||!lock||!clients) return false;
    bcopy(p->fdt,fdt,fdtBytes); native.fdt=fdt;
    if(!boardFDT(fdt,fdtBytes)) return false;
    maps[0]={0x5900000,0x40000,0}; maps[1]={0x5940000,0x10000,0};
    maps[2]={0x5961000,0x800,0}; maps[3]={0x596a000,0x30000,0};
    maps[4]={0x599e000,0x1000,0}; maps[5]={0x1400000,0x1f0000,0};
    /* gpucc is already inside the wrapper's exact FDT aperture. Avoid a
     * second overlapping Device mapping. No user mapping exposes registers. */
    for(unsigned i=0;i<6;i++) {
        maps[i].cpu=ml_io_map_unmappable(maps[i].pa,maps[i].bytes,VM_WIMG_IO);
        if(!maps[i].cpu) return false;
    }
    return true;
}
bool DREAdrenoGPU::owns()
{
    bool ok=ownerLockBound&&ownerLockStatus()==DRE_GPU_OK&&ownershipAcquired&&lease&&
        !providerFault&&native.owns&&native.owns(native.cookie,lease)==1;
    if(!ok && initialized) {
        providerFault=true;
        dre_a619_startup_invalidate(&startup,"NATIVE_LEASE_LOST");
    }
    return ok;
}
void DREAdrenoGPU::ownerEnter()
{
    /* This is the actual existing driver mutex, with a task-owner record
     * updated only inside its real lock/unlock boundary. No ready flag or
     * second mutex substitutes for exclusion. Normal APIs are task-only. */
    IOLockLock(lock);
    lck_mtx_assert(IOLockGetMachLock(lock),LCK_MTX_ASSERT_OWNED);
    __atomic_store_n(&lockHolder,current_thread(),__ATOMIC_RELEASE);
}
void DREAdrenoGPU::ownerLeave()
{
    lck_mtx_assert(IOLockGetMachLock(lock),LCK_MTX_ASSERT_OWNED);
    __atomic_store_n(&lockHolder,THREAD_NULL,__ATOMIC_RELEASE);
    IOLockUnlock(lock);
}
int DREAdrenoGPU::ownerLockStatus()
{
    if(!lock||ml_at_interrupt_context()||
       __atomic_load_n(&lockHolder,__ATOMIC_ACQUIRE)!=current_thread())return DRE_GPU_NOT_READY;
    /* Direct actual XNU8796 assertion also applies in non-MACH_ASSERT builds;
     * IOLockAssert's macro alone can compile away there. */
    lck_mtx_assert(IOLockGetMachLock(lock),LCK_MTX_ASSERT_OWNED);
    return DRE_GPU_OK;
}
void DREAdrenoGPU::enterHook(void *cookie) { ((DREAdrenoGPU *)cookie)->ownerEnter(); }
void DREAdrenoGPU::leaveHook(void *cookie) { ((DREAdrenoGPU *)cookie)->ownerLeave(); }
int DREAdrenoGPU::lockStatusHook(void *cookie) { return ((DREAdrenoGPU *)cookie)->ownerLockStatus(); }
static volatile uint8_t *registerAddress(DREAdrenoGPU *g,uint64_t pa,size_t n)
{
    if(!g||!g->owns()||!n||(pa&(n-1))) return NULL;
    for(unsigned i=0;i<6;i++) if(pa>=g->maps[i].pa&&
        pa-g->maps[i].pa<=g->maps[i].bytes-n)
        return (volatile uint8_t *)(g->maps[i].cpu+pa-g->maps[i].pa);
    return NULL;
}
int DREAdrenoGPU::read32(void *c,uint64_t p,uint32_t *v)
{
    volatile uint8_t *a=registerAddress((DREAdrenoGPU *)c,p,4);
    if(!a||!v) return 0;
    __asm__ volatile("dsb sy":::"memory"); *v=*(volatile uint32_t *)a;
    __asm__ volatile("dsb sy":::"memory"); return 1;
}
int DREAdrenoGPU::write32(void *c,uint64_t p,uint32_t v)
{
    volatile uint8_t *a=registerAddress((DREAdrenoGPU *)c,p,4);
    if(!a) return 0;
    *(volatile uint32_t *)a=v; __asm__ volatile("dsb sy":::"memory"); return 1;
}
int DREAdrenoGPU::read64(void *c,uint64_t p,uint64_t *v)
{
    volatile uint8_t *a=registerAddress((DREAdrenoGPU *)c,p,8);
    if(!a||!v) return 0;
    __asm__ volatile("dsb sy":::"memory"); *v=*(volatile uint64_t *)a;
    __asm__ volatile("dsb sy":::"memory"); return 1;
}
int DREAdrenoGPU::write64(void *c,uint64_t p,uint64_t v)
{
    volatile uint8_t *a=registerAddress((DREAdrenoGPU *)c,p,8);
    if(!a) return 0;
    *(volatile uint64_t *)a=v; __asm__ volatile("dsb sy":::"memory"); return 1;
}
void DREAdrenoGPU::barrier(void *) { __asm__ volatile("dsb sy":::"memory"); }
uint64_t DREAdrenoGPU::now(void *)
{
    uint64_t t; __asm__ volatile("isb; mrs %0,cntpct_el0":"=r"(t)::"memory"); return t;
}
int DREAdrenoGPU::allocHook(void *c,uint64_t n,uint32_t a,
    dre_gpu_native_allocation **p,dre_gpu_native_memory *v)
{ return ((DREAdrenoGPU *)c)->allocate(n,a,p,v); }
int DREAdrenoGPU::contiguousHook(void *c,uint64_t n,uint32_t a,
    dre_gpu_native_allocation **p,dre_gpu_native_memory *v)
{ return ((DREAdrenoGPU *)c)->allocate(n,a,p,v,true); }
void DREAdrenoGPU::discardHook(void *c,dre_gpu_native_allocation *p)
{ ((DREAdrenoGPU *)c)->discard(p); }
int DREAdrenoGPU::ownsCPUHook(void *c,const void *p,size_t n)
{ return ((DREAdrenoGPU *)c)->ownsCPU(p,n); }
int DREAdrenoGPU::ownsRAMHook(void *c,uint64_t p,size_t n)
{ return ((DREAdrenoGPU *)c)->ownsRAM(p,n); }
int DREAdrenoGPU::verifyHook(void *c,const dre_gpu_native_memory *v)
{ return ((DREAdrenoGPU *)c)->verifyMemory(v); }

static void sortPA(uint64_t *a,uint32_t count)
{
    /* In-place heapsort: O(n log n), no allocation or quadratic per-page
     * ownership scans. This storage is never published to GPU/userspace. */
    for(uint32_t pass=count/2+count;pass>0;pass--) {
        uint32_t root,end;
        if(pass>count) {root=pass-count-1;end=count;}
        else {
            end=pass-1;if(!end)break;
            uint64_t swap=a[0];a[0]=a[end];a[end]=swap;root=0;
        }
        for(;;) {
            uint32_t child=root*2+1;if(child>=end)break;
            if(child+1<end&&a[child]<a[child+1])child++;
            if(a[root]>=a[child])break;
            uint64_t swap=a[root];a[root]=a[child];a[child]=swap;root=child;
        }
    }
}
static bool containsPA(const dre_gpu_native_allocation *a,uint64_t page)
{
    uint32_t lo=0,hi=a->view.page_count;
    while(lo<hi) {uint32_t mid=lo+(hi-lo)/2;
        if(a->sortedPA[mid]<page)lo=mid+1;else hi=mid;}
    return lo<a->view.page_count&&a->sortedPA[lo]==page;
}
static bool withinCPU(const void *base,size_t bytes,const void *ptr,size_t size)
{
    uintptr_t a=(uintptr_t)base,p=(uintptr_t)ptr;
    return base&&ptr&&size&&p>=a&&p-a<=bytes&&size<=bytes-(p-a);
}
int DREAdrenoGPU::ownsCPU(const void *p,size_t n)
{
    if(!owns()||!n)return 0;
    for(dre_gpu_native_allocation *a=allocations;a;a=a->next)
        if(withinCPU(a->view.cpu,a->view.bytes,p,n)||withinCPU(a,sizeof(*a),p,n)||
           withinCPU(a->view.pages,a->metadata_bytes,p,n)||withinCPU(a->sortedPA,a->sorted_bytes,p,n))
            return 1;
    return 0;
}
int DREAdrenoGPU::ownsRAM(uint64_t pa,size_t n)
{
    if(!owns()||!n||((pa|n)&4095)||n>DRE_GPU_BO_MAX_BYTES||pa>UINT64_MAX-n)return 0;
    dre_gpu_native_allocation *found=NULL;
    if(lastPAOwner&&containsPA(lastPAOwner,pa))found=lastPAOwner;
    if(!found)for(dre_gpu_native_allocation *a=allocations;a;a=a->next)
        if(containsPA(a,pa)){found=a;break;}
    if(!found)return 0;
    for(uint64_t offset=0;offset<n;offset+=4096)if(!containsPA(found,pa+offset))return 0;
    lastPAOwner=found;return 1;
}
int DREAdrenoGPU::verifyMemory(const dre_gpu_native_memory *m)
{
    if(!m||!owns()||!kernelProofAvailable())return 0;
    for(dre_gpu_native_allocation *a=allocations;a;a=a->next)if(a->view.cpu==m->cpu&&
        a->view.pages==m->pages&&a->view.bytes==m->bytes&&a->view.page_count==m->page_count&&
        m->cache_contract==DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE) {
        for(uint32_t i=0;i<m->page_count;i++) {
            if(m->pages[i].permissions!=a->access)return 0;
            IOByteCount segment=0;uint64_t off=(uint64_t)i*4096;
            if(a->descriptor->getPhysicalSegment(off,&segment,kIOMemoryMapperNone)!=m->pages[i].pa||
               segment<4096||kvtophys((vm_offset_t)m->cpu+off)!=m->pages[i].pa)return 0;
        }
        return kernelProof(m);
    }
    return 0;
}
bool DREAdrenoGPU::kernelProofAvailable()
{
#if defined(DRE_GPU_NATIVE_PMAP_PROOF) && DRE_GPU_NATIVE_PMAP_PROOF
    return true; /* actual implementation must link; it can still refuse */
#else
    return native.normal_nc_alias!=NULL;
#endif
}
bool DREAdrenoGPU::taskProofAvailable()
{
#if defined(DRE_GPU_NATIVE_PMAP_PROOF) && DRE_GPU_NATIVE_PMAP_PROOF
    return true;
#else
    return native.task_nc_alias!=NULL;
#endif
}
int DREAdrenoGPU::kernelProof(const dre_gpu_native_memory *m)
{
    if(!m||!owns())return 0;
    if(native.normal_nc_alias)return native.normal_nc_alias(native.cookie,m->cpu,
        m->bytes,m->pages,m->page_count)==1;
#if defined(DRE_GPU_NATIVE_PMAP_PROOF) && DRE_GPU_NATIVE_PMAP_PROOF
    return dre_gpu_pmap_verify_kernel_nc((uint64_t)(uintptr_t)m->cpu,m->pages,
        m->page_count,sizeof(*m->pages));
#else
    return 0;
#endif
}
int DREAdrenoGPU::taskProof(task_t task,uint64_t address,
    const dre_gpu_native_memory *m,uint32_t access)
{
    if(!m||!owns())return 0;
    if(native.task_nc_alias)return native.task_nc_alias(native.cookie,
        (uint64_t)(uintptr_t)task,address,m)==1;
#if defined(DRE_GPU_NATIVE_PMAP_PROOF) && DRE_GPU_NATIVE_PMAP_PROOF
    return dre_gpu_pmap_verify_task_nc(task,address,m->pages,m->page_count,
        sizeof(*m->pages),access);
#else
    (void)access;return 0;
#endif
}

int DREAdrenoGPU::allocate(uint64_t bytes,uint32_t access,
    dre_gpu_native_allocation **out,dre_gpu_native_memory *view,bool contiguous)
{
    if(!out||!view||!goodAccess(access)||!bytes||bytes>DRE_GPU_BO_MAX_BYTES||
       bytes%DRE_GPU_CPU_PAGE_BYTES||!owns()||!kernelProofAvailable()||ownerLockStatus()!=DRE_GPU_OK||
       activeCatalogScope||nextAllocation==UINT64_MAX||catalogGeneration==UINT64_MAX)
        return DRE_GPU_UNSUPPORTED;
    *out=NULL; bzero(view,sizeof(*view));
    dre_gpu_native_allocation *a=(dre_gpu_native_allocation *)IOMallocZero(sizeof(*a));
    if(!a) return DRE_GPU_NO_MEMORY;
    a->view.bytes=bytes; a->view.page_count=(uint32_t)(bytes/DRE_GPU_PAGE_BYTES);
    a->metadata_bytes=a->view.page_count*sizeof(dre_a619_smmu_page);
    a->view.pages=(dre_a619_smmu_page *)IOMallocZero(a->metadata_bytes);
    a->sorted_bytes=a->view.page_count*sizeof(uint64_t);
    a->sortedPA=(uint64_t *)IOMallocZero(a->sorted_bytes);
    IOOptionBits options=kIODirectionInOut|kIOMemoryKernelUserShared|kIOMapWriteCombineCache;
    if(contiguous)options|=kIOMemoryPhysicallyContiguous;
    a->descriptor=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,options,
        bytes,DRE_GPU_CPU_PAGE_BYTES);
    if(a->view.pages&&a->sortedPA&&a->descriptor&&
       a->descriptor->prepare(kIODirectionInOut)==kIOReturnSuccess) {
        a->prepared=true;
        a->view.cpu=a->descriptor->getBytesNoCopy();
    }
    bool ok=a->view.cpu && !((uintptr_t)a->view.cpu&(DRE_GPU_CPU_PAGE_BYTES-1));
    for(uint32_t i=0;ok&&i<a->view.page_count;i++) {
        IOByteCount segment=0; uint64_t off=(uint64_t)i*DRE_GPU_PAGE_BYTES;
        uint64_t pa=a->descriptor->getPhysicalSegment(off,&segment,kIOMemoryMapperNone);
        if(!pa||(pa&(DRE_GPU_PAGE_BYTES-1))||pa>=(1ull<<40)||segment<DRE_GPU_PAGE_BYTES||
           kvtophys((vm_offset_t)a->view.cpu+off)!=pa||
           !dre_dma_ram_allowed(fdt,pa,DRE_GPU_PAGE_BYTES)) { ok=false;break; }
        dre_a619_smmu_page *page=(dre_a619_smmu_page *)&a->view.pages[i];
        if(contiguous&&((i==0&&segment<bytes)||
           (i&&pa!=a->view.pages[0].pa+off))) {ok=false;break;}
        page->pa=pa; page->permissions=access;
        a->sortedPA[i]=pa;
    }
    if(ok) {
        sortPA(a->sortedPA,a->view.page_count);
        for(uint32_t i=1;i<a->view.page_count;i++)if(a->sortedPA[i]==a->sortedPA[i-1])ok=false;
    }
    if(ok)ok=kernelProof(&a->view)!=0;
    if(!ok) {
        if(a->prepared) a->descriptor->complete(kIODirectionInOut);
        OSSafeReleaseNULL(a->descriptor);
        if(a->view.pages) IOFree((void *)a->view.pages,a->metadata_bytes);
        if(a->sortedPA) IOFree(a->sortedPA,a->sorted_bytes);
        IOFree(a,sizeof(*a)); return DRE_GPU_NOT_READY;
    }
    a->view.cache_contract=DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE;
    a->generation=++nextAllocation;a->access=access;a->purpose=DRE_GPU_ALLOC_UNKNOWN;
    bzero(a->view.cpu,bytes); barrier(this);
    a->next=allocations; allocations=a;
    ++catalogGeneration;
    *out=a; *view=a->view; return DRE_GPU_OK;
}
int DREAdrenoGPU::releaseRetired(dre_gpu_native_allocation *a)
{
    if(ownerLockStatus()!=DRE_GPU_OK||activeCatalogScope||catalogGeneration==UINT64_MAX)return DRE_GPU_BUSY;
    dre_gpu_native_allocation **p=&allocations;
    while(*p&&*p!=a) p=&(*p)->next;
    if(!*p) return DRE_GPU_STALE;
    if(a->catalogPins)return DRE_GPU_BUSY;
    /* Called only after the map/firmware/cache/TLB owner proved retirement. */
    if(a->prepared&&a->descriptor->complete(kIODirectionInOut)!=kIOReturnSuccess)
        return DRE_GPU_FAULT; /* retain prepared descriptor on uncertainty */
    a->prepared=false;
    *p=a->next;
    if(lastPAOwner==a)lastPAOwner=NULL;
    OSSafeReleaseNULL(a->descriptor);
    IOFree((void *)a->view.pages,a->metadata_bytes);
    IOFree(a->sortedPA,a->sorted_bytes);
    OSSafeReleaseNULL(a->taskClient);++catalogGeneration;
    IOFree(a,sizeof(*a));return DRE_GPU_OK;
}
void DREAdrenoGPU::discard(dre_gpu_native_allocation *a)
{
    for(dre_gpu_native_allocation *p=allocations;p;p=p->next)
        if(p==a&&!p->published) { releaseRetired(p);return; }
}
/* UNVALIDATED actual allocator registry. No numeric/user role can create a
 * TASK_BO record; all callbacks run under this service's real IOLock. */
dre_gpu_native_allocation *DREAdrenoGPU::findAllocation(const dre_gpu_native_allocation *wanted)
{ for(auto *a=allocations;a;a=a->next)if(a==wanted)return a;return NULL; }
bool DREAdrenoGPU::registeredClient(DREAdrenoGPUClient *c,uint64_t generation)
{
    if(!c||!generation||c->gpu!=this||c->generation!=generation||!c->owner)return false;
    for(unsigned i=0;i<clients->getCount();i++)if(clients->getObject(i)==c)return true;
    return false;
}
int DREAdrenoGPU::classify(dre_gpu_native_allocation *wanted,const dre_gpu_native_memory *view,uint32_t purpose)
{
    if(ownerLockStatus()!=DRE_GPU_OK||activeCatalogScope||catalogGeneration==UINT64_MAX)return DRE_GPU_BUSY;
    auto *a=findAllocation(wanted);
    if(!a||a->published||a->purpose!=DRE_GPU_ALLOC_UNKNOWN||a->taskClient||
       !dre_gpu_catalog_kernel_purpose(purpose,a->access)||
       !dre_gpu_catalog_memory_same(&a->view,view)||verifyMemory(view)!=1)return DRE_GPU_INVALID;
    a->purpose=purpose;++catalogGeneration;return DRE_GPU_OK;
}
int DREAdrenoGPU::classifyTask(dre_gpu_native_allocation *wanted,DREAdrenoGPUClient *c)
{
    if(ownerLockStatus()!=DRE_GPU_OK||activeCatalogScope||catalogGeneration==UINT64_MAX)return DRE_GPU_BUSY;
    auto *a=findAllocation(wanted);
    if(!a||a->published||a->purpose!=DRE_GPU_ALLOC_UNKNOWN||a->taskClient||
       !registeredClient(c,c?c->generation:0)||c->closing||c->sessionFault||verifyMemory(&a->view)!=1)
        return DRE_GPU_INVALID;
    unsigned linked=0;
    for(unsigned i=0;i<DRE_GPU_MAX_BOS;i++)if(c->bos[i].allocation==a&&
       dre_gpu_catalog_memory_same(&c->bos[i].native.memory,&a->view))linked++;
    if(linked!=1)return DRE_GPU_INVALID;
    a->taskClient=c;c->retain();a->taskGeneration=c->generation;
    a->purpose=DRE_GPU_ALLOC_TASK_BO;++catalogGeneration;return DRE_GPU_OK;
}
dre_gpu_native_allocation *DREAdrenoGPU::privateStorage(const void *p,size_t bytes)
{
    for(auto *a=allocations;a;a=a->next)if(!a->taskClient&&
       (a->purpose==DRE_GPU_ALLOC_CPU_METADATA||a->purpose==DRE_GPU_ALLOC_PRIVATE_CONTROL||
        a->purpose==DRE_GPU_ALLOC_TRUSTED_STATE||a->purpose==DRE_GPU_ALLOC_CATALOG_SCRATCH||
        a->purpose==DRE_GPU_ALLOC_BOOT_TABLES)&&
       withinCPU(a->view.cpu,a->view.bytes,p,bytes))return a;
    return NULL;
}
int DREAdrenoGPU::ownsPrivateCPU(const void *p,size_t bytes)
{
    if(ownerLockStatus()!=DRE_GPU_OK||!owns()||!bytes)return 0;
    auto *a=privateStorage(p,bytes);
    /* Live page tables are not writable planner output storage. */
    if(a&&a->purpose!=DRE_GPU_ALLOC_BOOT_TABLES&&verifyMemory(&a->view)==1)return 1;
    /* Borrowed page-list metadata is actual private allocator bookkeeping,
     * including TASK_BO physical descriptors, never its user-mapped data. */
    for(a=allocations;a;a=a->next)if(withinCPU(a->view.pages,a->metadata_bytes,p,bytes)&&
       verifyMemory(&a->view)==1)return 1;
    return 0;
}
int DREAdrenoGPU::recordMapping(dre_gpu_native_allocation *wanted,const dre_a619_vm *vm,
    const dre_a619_vm_ops *ops,const dre_a619_vm_mapping *mapping)
{
    if(ownerLockStatus()!=DRE_GPU_OK||activeCatalogScope||catalogGeneration==UINT64_MAX)return DRE_GPU_BUSY;
    auto *a=findAllocation(wanted);
    if(!a||a->mappedVM||a->purpose==DRE_GPU_ALLOC_UNKNOWN||a->purpose==DRE_GPU_ALLOC_CATALOG_SCRATCH||
       !vm||!ops||!mapping||
       !privateStorage(vm,sizeof(*vm))||!privateStorage(ops,sizeof(*ops))||
       !privateStorage(mapping,sizeof(*mapping))||verifyMemory(&a->view)!=1)
        return DRE_GPU_NOT_READY;
    if(!vm->initialized||vm->unsafe||!vm->domain||!vm->pages||!vm->leases||!vm->reverse_pa||
       !privateStorage(vm->domain,sizeof(*vm->domain)))return DRE_GPU_NOT_READY;
    if(!vm->domain->page_count||vm->domain->page_count>DRE_A619_SMMU_MAX_PAGES||
       vm->domain->page_count!=vm->owned_pages_count||
       vm->domain->table_capacity_bytes!=(size_t)(2u+(vm->domain->page_count+511u)/512u)*4096u||
       vm->reverse_entries!=vm->domain->page_count*2u||
       vm->owned_domain!=vm->domain||vm->owned_pages!=vm->pages||vm->owned_leases!=vm->leases||
       vm->owned_reverse!=vm->reverse_pa||vm->owned_tables!=vm->domain->tables||
       !vm->generation||mapping->generation!=vm->generation||!mapping->lease||mapping->retired||
       !mapping->page_count||mapping->page_count>a->view.page_count||
       mapping->first_page>=vm->domain->page_count||
       mapping->page_count>vm->domain->page_count-mapping->first_page||
       !ops->mmu.guard||!ops->table_mapping||ops->mmu.guard(ops->mmu.cookie)!=1)
        return DRE_GPU_NOT_READY;
    const void *addresses[]={vm,ops,mapping,vm->domain,vm->pages,vm->leases,vm->domain->tables,vm->reverse_pa};
    size_t lengths[]={sizeof(*vm),sizeof(*ops),sizeof(*mapping),sizeof(*vm->domain),
        (size_t)vm->domain->page_count*sizeof(*vm->pages),(size_t)vm->domain->page_count*8,
        vm->domain->table_capacity_bytes,(size_t)vm->reverse_entries*8};
    for(unsigned i=0;i<8;i++) {
        auto *storage=privateStorage(addresses[i],lengths[i]);
        if(!storage||storage->purpose==DRE_GPU_ALLOC_CATALOG_SCRATCH||
           verifyMemory(&storage->view)!=1)return DRE_GPU_NOT_READY;
        a->mapStorage[i]={storage,storage->generation,addresses[i],lengths[i]};
    }
    if(ops->table_mapping(ops->mmu.cookie,vm->domain->table_pa,vm->domain->tables,
       vm->domain->table_capacity_bytes)!=1)return DRE_GPU_NOT_READY;
    for(uint32_t i=0;i<mapping->page_count;i++) {
        uint32_t page=mapping->first_page+i;
        if(vm->leases[page]!=mapping->lease||vm->pages[page].pa!=a->view.pages[i].pa||
           vm->pages[page].permissions!=a->access)return DRE_GPU_NOT_READY;
        uint64_t expected=a->view.pages[i].pa|UINT64_C(0xe63);
        if(!(a->access&DRE_GPU_ACCESS_WRITE))expected|=0x80;
        if(!(a->access&DRE_GPU_ACCESS_EXECUTE))expected|=UINT64_C(3)<<53;
        if(vm->domain->tables[1024+page]!=(vm->domain->active?expected:0))return DRE_GPU_NOT_READY;
    }
    a->mapStorageCount=8;a->mappedVM=vm;a->mappedOps=ops;a->mappedHandle=mapping;a->mapped=*mapping;
    ++catalogGeneration;return DRE_GPU_OK; /* staged is not translation-ready */
}
int DREAdrenoGPU::mappingCurrent(dre_gpu_native_allocation *a,dre_gpu_allocation_catalog_record *out)
{
    if(!a->mappedVM)return DRE_GPU_OK; /* No scalar/native-BO VA inference. */
    if(a->mapStorageCount!=8)return DRE_GPU_NOT_READY;
    for(unsigned i=0;i<8;i++) {
        auto *storage=findAllocation(a->mapStorage[i].owner);
        if(!storage||storage->generation!=a->mapStorage[i].generation||
           privateStorage(a->mapStorage[i].address,a->mapStorage[i].bytes)!=storage)return DRE_GPU_STALE;
    }
    const auto *v=a->mappedVM;const auto *m=a->mappedHandle;const auto *o=a->mappedOps;
    if(!v->initialized||v->unsafe||v->domain!=a->mapStorage[3].address||
       v->pages!=a->mapStorage[4].address||v->leases!=a->mapStorage[5].address||
       v->reverse_pa!=a->mapStorage[7].address)return DRE_GPU_STALE;
    if(v->domain->unsafe||v->domain->tables!=a->mapStorage[6].address||
       v->domain->page_count!=v->owned_pages_count||!v->domain->page_count||
       v->domain->page_count>DRE_A619_SMMU_MAX_PAGES||
       v->domain->table_capacity_bytes!=a->mapStorage[6].bytes||
       m->first_page>=v->domain->page_count||m->page_count>v->domain->page_count-m->first_page||
       m->page_count>a->view.page_count||
       v->generation!=a->mapped.generation||m->generation!=a->mapped.generation||
       m->lease!=a->mapped.lease||m->first_page!=a->mapped.first_page||
       m->page_count!=a->mapped.page_count||v->owned_domain!=v->domain||
       v->owned_tables!=v->domain->tables||v->owned_pages!=v->pages||v->owned_leases!=v->leases||
       !o->mmu.guard||o->mmu.guard(o->mmu.cookie)!=1)return DRE_GPU_STALE;
    if(v->domain->active&&strcmp(dre_a619_smmu_check_live(v->domain,&o->mmu),"SMMU_LIVE_STATE_VERIFIED"))
        return DRE_GPU_NOT_READY;
    for(uint32_t i=0;i<m->page_count;i++) {
        uint32_t page=m->first_page+i;
        if(m->retired) {
            if(v->leases[page]||v->pages[page].pa||v->pages[page].permissions||
               v->domain->tables[1024+page])return DRE_GPU_STALE;
            continue;
        }
        if(v->leases[page]!=m->lease||v->pages[page].pa!=a->view.pages[i].pa||
           v->pages[page].permissions!=a->access)return DRE_GPU_STALE;
        uint64_t pte=a->view.pages[i].pa|UINT64_C(0xe63);
        if(!(a->access&2))pte|=0x80;
        if(!(a->access&4))pte|=UINT64_C(3)<<53;
        if(v->domain->tables[1024+page]!=(v->domain->active?pte:0))return DRE_GPU_STALE;
    }
    out->mapping_state=m->retired?DRE_GPU_CATALOG_MAPPING_RETIRED:
        v->domain->active?DRE_GPU_CATALOG_MAPPING_ACTIVE:DRE_GPU_CATALOG_MAPPING_STAGED;
    out->gpu_va=m->retired?0:v->domain->va_base+(uint64_t)m->first_page*4096;
    out->mapping_lease=m->lease;out->mapping_generation=m->generation;
    out->mapped_prefix_pages=m->retired?0:m->page_count;return DRE_GPU_OK;
}
int DREAdrenoGPU::catalogRecordCurrent(const dre_gpu_allocation_catalog_snapshot *s,uint32_t index)
{
    if(ownerLockStatus()!=DRE_GPU_OK||!s||s!=catalogSnapshot||!activeCatalogScope||
       s->scope_token!=activeCatalogScope||s->catalog_generation!=catalogGeneration||
       s->record_count>DRE_GPU_CATALOG_MAX_ALLOCATIONS||index>=DRE_GPU_CATALOG_MAX_ALLOCATIONS||index>=s->record_count)return 0;
    const auto *r=&s->records[index];auto *a=findAllocation(r->allocation);
    if(!a||a->generation!=r->allocation_generation||a->catalogPin!=s->scope_token||!a->catalogPins||
       !dre_gpu_catalog_memory_same(&a->view,&r->memory)||a->purpose!=r->purpose||a->access!=r->granted_access||
       (uint32_t)a->published!=r->published||r->pin_lease!=activeCatalogScope||r->reserved||r->reserved2||
       verifyMemory(&a->view)!=1)return 0;
    auto *c=OSDynamicCast(DREAdrenoGPUClient,a->taskClient);
    uint64_t task=0,generation=0;uint32_t cpuMapped=0;
    if(a->taskClient) {
        if(!c||!registeredClient(c,a->taskGeneration)||a->purpose!=DRE_GPU_ALLOC_TASK_BO)return 0;
        task=(uint64_t)(uintptr_t)c->owner;generation=c->generation;
        unsigned linked=0;
        for(unsigned j=0;j<DRE_GPU_MAX_BOS;j++)if(c->bos[j].allocation==a&&
           dre_gpu_catalog_memory_same(&c->bos[j].native.memory,&a->view)) {
            linked++;cpuMapped=c->bos[j].cpuMap?1:0;
        }
        if(linked!=1)return 0;
    }
    if(task!=r->task_identity||generation!=r->connection_generation||cpuMapped!=r->cpu_mapped)return 0;
    dre_gpu_allocation_catalog_record actual={};
    if(mappingCurrent(a,&actual)!=DRE_GPU_OK||actual.gpu_va!=r->gpu_va||
       actual.mapping_lease!=r->mapping_lease||actual.mapping_generation!=r->mapping_generation||
       actual.mapped_prefix_pages!=r->mapped_prefix_pages||actual.mapping_state!=r->mapping_state)return 0;
    return 1;
}
int DREAdrenoGPU::catalogCurrent(const dre_gpu_allocation_catalog_snapshot *s)
{
    if(ownerLockStatus()!=DRE_GPU_OK||!owns()||!s||s!=catalogSnapshot||!activeCatalogScope||
       s->scope_token!=activeCatalogScope||s->catalog_generation!=catalogGeneration||
       s->common_lease!=lease||s->version!=DRE_GPU_CATALOG_VERSION||s->struct_bytes!=sizeof(*s)||
       s->coverage_kind!=DRE_GPU_CATALOG_ALLOCATOR_DATA_ONLY||s->reserved||
       !registeredClient(catalogTarget,s->connection_generation)||
       s->task_identity!=(uint64_t)(uintptr_t)catalogTarget->owner||catalogTarget->closing||catalogTarget->sessionFault)return 0;
    dre_gpu_info_reply info={};
    if(!native.describe||native.describe(native.cookie,lease,&info)!=DRE_GPU_OK||
       info.reset_generation!=s->reset_epoch)return 0;
    if(s->window_kind==DRE_GPU_CATALOG_WINDOW_ABSENT) {
        if(s->va_base||s->window_pages)return 0;
    } else if(s->window_kind!=DRE_GPU_CATALOG_BOOTSTRAP_SHARED_WINDOW||
       !catalogTarget->exclusiveVM||!exclusiveVM||dre_gpu_vm_binding_live(exclusiveVM)!=DRE_GPU_OK||
       exclusiveVM->task_identity!=s->task_identity||exclusiveVM->task_generation!=s->connection_generation||
       exclusiveVM->base!=s->va_base||exclusiveVM->page_count!=s->window_pages)return 0;
    unsigned count=0;uint64_t pages=0;
    for(auto *a=allocations;a;a=a->next){count++;pages+=a->view.page_count;}
    if(count!=s->record_count||pages!=s->total_pages)return 0;
    for(unsigned i=0;i<count;i++)if(catalogRecordCurrent(s,i)!=1)return 0;
    return 1;
}
int DREAdrenoGPU::withCatalog(const void *client,uint64_t generation,
    dre_gpu_allocation_catalog_snapshot *s,int (*visitor)(void *,const dre_gpu_allocation_catalog_snapshot *),void *ctx)
{
    if(ownerLockStatus()!=DRE_GPU_OK||!owns()||activeCatalogScope||!s||!visitor||
       nextCatalogScope==UINT64_MAX||ownsPrivateCPU(s,sizeof(*s))!=1)return DRE_GPU_NOT_READY;
    auto *scratch=privateStorage(s,sizeof(*s));
    if(!scratch||scratch->purpose!=DRE_GPU_ALLOC_CATALOG_SCRATCH||scratch->mappedVM)
        return DRE_GPU_NOT_READY;
    DREAdrenoGPUClient *target=NULL;
    for(unsigned i=0;i<clients->getCount();i++)if(clients->getObject(i)==client)
        target=OSDynamicCast(DREAdrenoGPUClient,clients->getObject(i));
    if(!registeredClient(target,generation)||target->closing||target->sessionFault)return DRE_GPU_STALE;
    unsigned count=0;uint64_t pages=0;
    for(auto *a=allocations;a;a=a->next) {
        if(++count>DRE_GPU_CATALOG_MAX_ALLOCATIONS||
           (pages+=a->view.page_count)>DRE_GPU_CATALOG_MAX_PAGES||a->catalogPins||a->catalogPin||
           verifyMemory(&a->view)!=1)
            return DRE_GPU_LIMIT;
    }
    dre_gpu_info_reply info={};
    if(!native.describe||native.describe(native.cookie,lease,&info)!=DRE_GPU_OK||!info.reset_generation)
        return DRE_GPU_NOT_READY;
    bzero(s,sizeof(*s));s->version=DRE_GPU_CATALOG_VERSION;s->struct_bytes=sizeof(*s);
    s->scope_token=++nextCatalogScope;s->catalog_generation=catalogGeneration;
    s->common_lease=lease;s->reset_epoch=info.reset_generation;
    s->task_identity=(uint64_t)(uintptr_t)target->owner;s->connection_generation=generation;
    s->coverage_kind=DRE_GPU_CATALOG_ALLOCATOR_DATA_ONLY;
    if(target->exclusiveVM&&exclusiveVM&&dre_gpu_vm_binding_live(exclusiveVM)==DRE_GPU_OK&&
       exclusiveVM->task_identity==s->task_identity&&exclusiveVM->task_generation==generation) {
        s->window_kind=DRE_GPU_CATALOG_BOOTSTRAP_SHARED_WINDOW;s->va_base=exclusiveVM->base;
        s->window_pages=exclusiveVM->page_count;
    } /* Generic provider scalar VAs do NOT prove a task reservation. */
    retain();target->retain();catalogSnapshot=s;catalogTarget=target;activeCatalogScope=s->scope_token;
    int status=DRE_GPU_OK;
    for(auto *a=allocations;a;a=a->next) {
        auto *r=&s->records[s->record_count++];
        a->descriptor->retain();a->catalogPins++;a->catalogPin=s->scope_token;
        r->allocation=a;r->memory=a->view;r->allocation_generation=a->generation;
        r->pin_lease=s->scope_token;r->purpose=a->purpose;r->granted_access=a->access;r->published=a->published;
        auto *c=OSDynamicCast(DREAdrenoGPUClient,a->taskClient);
        if(c) {
            if(!registeredClient(c,a->taskGeneration)){status=DRE_GPU_STALE;break;}
            r->task_identity=(uint64_t)(uintptr_t)c->owner;r->connection_generation=c->generation;
            unsigned linked=0;
            for(unsigned j=0;j<DRE_GPU_MAX_BOS;j++)if(c->bos[j].allocation==a&&
               dre_gpu_catalog_memory_same(&c->bos[j].native.memory,&a->view)) {
                linked++;r->cpu_mapped=c->bos[j].cpuMap?1:0;
            }
            if(linked!=1){status=DRE_GPU_STALE;break;}
        }
        s->total_pages+=a->view.page_count;
        status=mappingCurrent(a,r);if(status!=DRE_GPU_OK)break;
    }
    if(status==DRE_GPU_OK&&catalogCurrent(s)!=1)status=DRE_GPU_STALE;
    if(status==DRE_GPU_OK)status=visitor(ctx,s);
    /* An illegal visitor dropping the real mutex cannot authorize cleanup.
     * Retain the whole scope/descriptor/client/service pins on that fault. */
    if(ownerLockStatus()!=DRE_GPU_OK)return DRE_GPU_FAULT;
    if(status==DRE_GPU_OK&&catalogCurrent(s)!=1)status=DRE_GPU_STALE;
    /* Borrowed view cannot outlive its genuine pin scope. Mutators are
     * blocked while active, and all real descriptor/task refs remain held. */
    for(auto *a=allocations;a;a=a->next) {
        if(a&&a->catalogPins&&a->catalogPin==activeCatalogScope) {
            a->catalogPins--;a->catalogPin=0;a->descriptor->release();
        }
    }
    bzero(s,sizeof(*s));activeCatalogScope=0;catalogSnapshot=NULL;catalogTarget=NULL;
    target->release();release();return status;
}
int DREAdrenoGPU::classifyHook(void *s,dre_gpu_native_allocation *a,const dre_gpu_native_memory *m,dre_gpu_allocation_purpose p)
{return ((DREAdrenoGPU *)s)->classify(a,m,p);}
int DREAdrenoGPU::mappingHook(void *s,dre_gpu_native_allocation *a,const dre_a619_vm *v,const dre_a619_vm_ops *o,const dre_a619_vm_mapping *m)
{return ((DREAdrenoGPU *)s)->recordMapping(a,v,o,m);}
int DREAdrenoGPU::catalogHook(void *s,const void *c,uint64_t g,dre_gpu_allocation_catalog_snapshot *v,int (*f)(void *,const dre_gpu_allocation_catalog_snapshot *),void *x)
{return ((DREAdrenoGPU *)s)->withCatalog(c,g,v,f,x);}
int DREAdrenoGPU::catalogCurrentHook(void *s,const dre_gpu_allocation_catalog_snapshot *v)
{return ((DREAdrenoGPU *)s)->catalogCurrent(v);}
int DREAdrenoGPU::catalogRecordHook(void *s,const dre_gpu_allocation_catalog_snapshot *v,uint32_t i)
{return ((DREAdrenoGPU *)s)->catalogRecordCurrent(v,i);}
int DREAdrenoGPU::privateCPUHook(void *s,const void *p,size_t n)
{return ((DREAdrenoGPU *)s)->ownsPrivateCPU(p,n);}
bool DREAdrenoGPU::start(IOService *provider)
{
    if(page_size!=DRE_GPU_CPU_PAGE_BYTES||!IOService::start(provider)) return false;
    setName(DRE_GPU_SERVICE);
    setProperty("UNVALIDATED",true);
    uint64_t hz=0; __asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
    ownerEnter();
    int r=DRE_GPU_UNSUPPORTED;
    if(hz==19200000&&native.bind_owner_lock) {
        dre_gpu_owner_lock serialization={DRE_GPU_OWNER_LOCK_VERSION,sizeof(dre_gpu_owner_lock),
            this,enterHook,leaveHook,lockStatusHook};
        uint32_t retained=0;
        r=native.bind_owner_lock(native.cookie,&serialization,&retained);
        if(retained)permanentHold=true;
        if(retained>1||(r==DRE_GPU_OK&&!retained)) {
            /* Unexpected lifetime result cannot authorize destroying a cookie
             * that an actual native worker may already have copied. */
            permanentHold=true;r=DRE_GPU_FAULT;
        }
        ownerLockBound=r==DRE_GPU_OK&&retained==1;
        if(ownerLockBound&&native.acquire)r=native.acquire(native.cookie,&lease);
        else if(r==DRE_GPU_OK)r=DRE_GPU_UNSUPPORTED;
    }
    ownershipAcquired=r==DRE_GPU_OK&&lease;
    if(r==DRE_GPU_OK&&lease&&hz==19200000&&owns()&&native.configure) {
        dre_gpu_native_io io={this,read32,write32,read64,write64,barrier,now};
        dre_gpu_native_allocator allocator={this,allocHook,discardHook,ownsCPUHook,
            ownsRAMHook,verifyHook,contiguousHook,DRE_GPU_ALLOCATOR_VERSION,sizeof(dre_gpu_native_allocator),
            classifyHook,mappingHook,catalogHook,catalogCurrentHook,catalogRecordHook,privateCPUHook};
        dre_a619_start_plan plan={};
        r=native.configure(native.cookie,lease,&io,&allocator,&startup,&plan);
        if(r==DRE_GPU_OK) {
            /* All configure allocations are now owned by startup. Before the
             * first possible MMIO/PAS publication there is no free path. */
            for(dre_gpu_native_allocation *a=allocations;a;a=a->next) a->published=true;
            permanentHold=true;
            dre_a619_startup_begin(&startup,&plan);
            initialized=startup.phase==DRE_A619_START_MARKER_CONFIRMED;
            if(initialized&&native.use_exclusive_vm_adapter) {
                dre_gpu_native_memory memory={};
                uint64_t bytes=(sizeof(dre_gpu_vm_binding)+DRE_GPU_CPU_PAGE_BYTES-1)&
                    ~(uint64_t)(DRE_GPU_CPU_PAGE_BYTES-1);
                if(allocate(bytes,DRE_GPU_ACCESS_READ|DRE_GPU_ACCESS_WRITE,
                   &exclusiveMetadata,&memory)==DRE_GPU_OK) {
                    if(classify(exclusiveMetadata,&memory,DRE_GPU_ALLOC_PRIVATE_CONTROL)!=DRE_GPU_OK) {
                        providerFault=true;permanentHold=true;ownerLeave();return false;
                    }
                    exclusiveMetadata->published=true;
                    dre_gpu_vm_binding *binding=(dre_gpu_vm_binding *)memory.cpu;
                    if(dre_gpu_vm_binding_init(binding,&startup,native.vm_reserved,
                       native.vm_reserved_count)==DRE_GPU_OK)exclusiveVM=binding;
                }
            }
        }
    }
    /* Failed configure may already have acquired irreversible RPM holds.
     * Never invent a cleanup/rail-off path; retain this diagnostic owner. */
    if(lease) permanentHold=true;
    ownerLeave();
    loop=IOWorkLoop::workLoop();
    timer=IOTimerEventSource::timerEventSource(this,poll);
    if(!loop||!timer||loop->addEventSource(timer)!=kIOReturnSuccess) return false;
    timer->setTimeoutMS(100);
    registerService(); return true;
}
void DREAdrenoGPU::info(dre_gpu_info_reply *out)
{
    out->bo_max_bytes=DRE_GPU_BO_MAX_BYTES; out->task_max_bytes=DRE_GPU_TASK_MAX_BYTES;
    out->cpu_page_bytes=DRE_GPU_CPU_PAGE_BYTES; out->gpu_page_bytes=DRE_GPU_PAGE_BYTES;
    out->max_bos=DRE_GPU_MAX_BOS; out->max_ibs=DRE_GPU_MAX_IBS;
    out->max_waits=DRE_GPU_MAX_WAITS; out->max_fences=DRE_GPU_MAX_FENCES;
    out->unavailable=(1ull<<10)-1;
    if(native.describe&&owns()&&native.describe(native.cookie,lease,out)!=DRE_GPU_OK)
        out->capabilities=0;
    if(!initialized||!owns()||strcmp(dre_a619_startup_check_live(&startup),"NATIVE_START_HELD")||
       startup.phase!=DRE_A619_START_MARKER_CONFIRMED) {
        out->capabilities=0;out->unavailable|=DRE_GPU_MISSING_MARKER;
    }
    if(native.use_exclusive_vm_adapter) {
        /* This is BO ownership in a bootstrap-sharing VM, NOT protected raw
         * task commands. Override even an overly optimistic description. */
        out->capabilities=0;
        out->unavailable|=DRE_GPU_MISSING_TASK_VM|DRE_GPU_MISSING_COMMAND_VALIDATOR|
            DRE_GPU_MISSING_HARDWARE_FENCE|DRE_GPU_MISSING_PERSISTENT_MAP;
        if(exclusiveVM&&dre_gpu_vm_binding_live(exclusiveVM)==DRE_GPU_OK) {
            out->capabilities=DRE_GPU_CAP_OWNED_TASK_BO;
            out->chip_id=startup.plan.smmu->chip_id;
            out->gpu_va_base=exclusiveVM->base;
            out->gpu_va_bytes=(uint64_t)exclusiveVM->page_count*4096;
            if(native.cpu_begin&&native.cpu_end&&taskProofAvailable())
                out->capabilities|=DRE_GPU_CAP_CPU_MAP_LEASE;
            if(native.cache)out->capabilities|=DRE_GPU_CAP_EXPLICIT_CACHE;
        }
    }
    /* Persistent pointer/access-epoch source exists, but its native cache,
     * immutable-command/protected-VM and raw-job ownership integration is not
     * qualified. Do not turn a description bit or an IOMemoryMap into support. */
    out->capabilities&=~(uint64_t)DRE_GPU_CAP_PERSISTENT_CPU_MAP;
    out->unavailable|=DRE_GPU_MISSING_PERSISTENT_MAP;
    if(!kernelProofAvailable()||(!native.use_exclusive_vm_adapter&&
       (!native.session_open||!native.session_close||!native.bo_map||!native.bo_retire)))
        out->capabilities=0;
    if(!native.cpu_begin||!native.cpu_end||!taskProofAvailable())
        out->capabilities&=~(uint64_t)DRE_GPU_CAP_CPU_MAP_LEASE;
    if(!native.cache) out->capabilities&=~(uint64_t)DRE_GPU_CAP_EXPLICIT_CACHE;
    if(!native.query||!native.ticket_close)
        out->capabilities&=~(uint64_t)(DRE_GPU_CAP_HARDWARE_FENCE|DRE_GPU_CAP_RAW_IB_SUBMIT);
    if(!native.submit) out->capabilities&=~(uint64_t)DRE_GPU_CAP_RAW_IB_SUBMIT;
    out->capabilities&=((1ull<<7)-1);
    if(out->chip_id!=DRE_A619_CHIP_ID) out->capabilities=0;
    /* Public limits describe THIS binding's arrays/allocator, not arbitrary
     * capacities an optional provider happened to return. */
    out->bo_max_bytes=DRE_GPU_BO_MAX_BYTES;out->task_max_bytes=DRE_GPU_TASK_MAX_BYTES;
    out->cpu_page_bytes=DRE_GPU_CPU_PAGE_BYTES;out->gpu_page_bytes=DRE_GPU_PAGE_BYTES;
    out->max_bos=DRE_GPU_MAX_BOS;out->max_ibs=DRE_GPU_MAX_IBS;
    out->max_waits=DRE_GPU_MAX_WAITS;out->max_fences=DRE_GPU_MAX_FENCES;
    out->cache_contract=DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE;
}
void DREAdrenoGPU::free()
{
    /* Hardware-visible objects cannot be freed by ordinary service teardown.
     * publish() holds a permanent reference once a native lease is acquired. */
    if(!permanentHold) {
        if(timer) timer->cancelTimeout();
        if(loop&&timer) loop->removeEventSource(timer);
        OSSafeReleaseNULL(timer); OSSafeReleaseNULL(loop);
        OSSafeReleaseNULL(clients);
        while(allocations) if(releaseRetired(allocations)!=DRE_GPU_OK)break;
        for(unsigned i=0;i<6;i++) if(maps[i].cpu) ml_io_unmap(
            trunc_page(maps[i].cpu),round_page(maps[i].bytes+(maps[i].cpu&PAGE_MASK)));
        if(fdt) IOFree(fdt,fdtBytes);
        if(lock) IOLockFree(lock);
    }
    IOService::free();
}

bool DREAdrenoGPUClient::initWithTask(task_t task,void *security,UInt32 type)
{
    gpu=NULL;owner=NULL;session=NULL;generation=nextBO=nextMap=nextFence=wiredBytes=0;
    nextSurface=surfaceBytes=0;bzero(surfaces,sizeof(surfaces));
    bos=NULL;jobs=NULL;closing=sessionFault=exclusiveVM=false;
    if(type||!task||!IOUserClient::initWithTask(task,security,type)) return false;
    bos=IONewZero(DREBO,DRE_GPU_MAX_BOS);
    jobs=IONewZero(DREJob,DRE_GPU_MAX_FENCES);
    if(!bos||!jobs)return false;
    owner=task; task_reference(owner);
    return true;
}
bool DREAdrenoGPUClient::start(IOService *provider)
{
    gpu=OSDynamicCast(DREAdrenoGPU,provider);
    return gpu&&IOUserClient::start(provider);
}
void DREAdrenoGPUClient::free()
{
    /* The service's clients array owns every open or orphaned session until
     * reap() retires all references. There is no free-on-task-death shortcut. */
    if(owner) { task_deallocate(owner); owner=NULL; }
    if(bos){IODelete(bos,DREBO,DRE_GPU_MAX_BOS);bos=NULL;}
    if(jobs){IODelete(jobs,DREJob,DRE_GPU_MAX_FENCES);jobs=NULL;}
    IOUserClient::free();
}
IOReturn DREAdrenoGPU::newUserClient(task_t owner,void *security,UInt32 type,
    OSDictionary *,IOUserClient **out)
{
    if(!out||type) return kIOReturnBadArgument;
    *out=NULL;
    DREAdrenoGPUClient *c=new DREAdrenoGPUClient;
    if(!c) return kIOReturnNoMemory;
    if(!c->initWithTask(owner,security,type)||!c->attach(this)||!c->start(this)) {
        c->detach(this);c->release();return kIOReturnNotReady;
    }
    ownerEnter();
    if(clients->getCount()>=4||nextSession==UINT64_MAX) {
        ownerLeave();c->detach(this);c->release();return kIOReturnNoResources;
    }
    c->generation=++nextSession;
    if(!clients->setObject(c)) {
        ownerLeave();c->detach(this);c->release();return kIOReturnNoMemory;
    }
    /* The strong orphan owner exists before an actual session can acquire
     * VM/context resources, including a nonzero session on partial failure. */
    dre_gpu_info_reply description={}; info(&description);
    if(description.capabilities&&native.use_exclusive_vm_adapter&&exclusiveVM) {
        int r=dre_gpu_vm_binding_open(exclusiveVM,(uint64_t)(uintptr_t)owner,c->generation);
        if(r==DRE_GPU_OK) {c->session=exclusiveVM;c->exclusiveVM=true;}
        else c->sessionFault=true;
    } else if(description.capabilities&&!native.use_exclusive_vm_adapter&&native.session_open) {
        int r=native.session_open(native.cookie,lease,(uint64_t)(uintptr_t)owner,&c->session);
        if(r!=DRE_GPU_OK) c->sessionFault=true;
    }
    ownerLeave();*out=c;return kIOReturnSuccess;
}
DREBO *DREAdrenoGPUClient::bo(uint64_t h)
{
    uint32_t slot=(uint32_t)(h&255u);
    if(!slot||slot>DRE_GPU_MAX_BOS) return NULL;
    DREBO *b=&bos[slot-1];
    return b->allocation&&b->handle==h&&!b->closed?b:NULL;
}
DREJob *DREAdrenoGPUClient::job(uint64_t h)
{
    uint32_t slot=(uint32_t)(h&255u);
    if(!slot||slot>DRE_GPU_MAX_FENCES) return NULL;
    DREJob *j=&jobs[slot-1];return j->used&&j->handle==h?j:NULL;
}
DRESurface *DREAdrenoGPUClient::surface(uint64_t token)
{
    uint32_t slot=(uint32_t)(token&255u);
    if(!slot||slot>DRE_GPU_MAX_SURFACES)return NULL;
    DRESurface *s=&surfaces[slot-1];return s->pin&&s->token==token?s:NULL;
}
int DREAdrenoGPUClient::closeSurface(DRESurface *s)
{
    if(!s||!s->pin)return DRE_GPU_STALE;
    int status=dre_surface_close(s->pin);
    if(status==DRE_GPU_OK) {surfaceBytes-=s->chargedBytes;bzero(s,sizeof(*s));}
    /* Failed descriptor complete retains the actual pin, strong task owner
     * and connection slot. The orphan timer cannot recycle that memory. */
    return status;
}
int DREAdrenoGPUClient::surfaceCall(uint32_t selector,const void *input,void *output)
{
    if(selector==DRE_GPU_SURFACE_RETAIN) {
        const dre_gpu_surface_retain_request *r=(const dre_gpu_surface_retain_request *)input;
        dre_gpu_surface_reply *out=(dre_gpu_surface_reply *)output;
        if(!r->mach_port_name||!(r->access&DRE_GPU_ACCESS_READ)||(r->access&~3u))return DRE_GPU_INVALID;
        /* Reserve the largest accepted backing before resolving/preparing.
         * A partial-failure pin remains charged conservatively until close. */
        if(nextSurface>=(UINT64_MAX>>8)||surfaceBytes>DRE_GPU_TASK_MAX_BYTES-wiredBytes||
           DRE_GPU_BO_MAX_BYTES>DRE_GPU_TASK_MAX_BYTES-wiredBytes-surfaceBytes)return DRE_GPU_LIMIT;
        unsigned slot=0;while(slot<DRE_GPU_MAX_SURFACES&&surfaces[slot].pin)slot++;
        if(slot==DRE_GPU_MAX_SURFACES)return DRE_GPU_LIMIT;
        DRESurface *s=&surfaces[slot];bzero(s,sizeof(*s));
        s->token=(++nextSurface<<8)|(slot+1);s->access=r->access;
        dre_gpu_surface_description description={};
        int status=dre_surface_retain_from_port(owner,r->mach_port_name,r->access,&s->pin,&description);
        if(!s->pin) {bzero(s,sizeof(*s));return status==DRE_GPU_OK?DRE_GPU_FAULT:status;}
        s->chargedBytes=DRE_GPU_BO_MAX_BYTES;surfaceBytes+=s->chargedBytes;
        out->surface_token=s->token; /* returned even on later failure for cleanup */
        out->description=description;
        if(status==DRE_GPU_OK) {
            uint64_t bytes=description.allocation_bytes;
            if(!bytes||bytes>DRE_GPU_BO_MAX_BYTES||bytes%DRE_GPU_CPU_PAGE_BYTES)
                return DRE_GPU_FAULT; /* preserve pin/charge until explicit close */
            surfaceBytes-=s->chargedBytes-bytes;s->chargedBytes=bytes;
        }
        return status;
    }
    uint64_t token=selector==DRE_GPU_SURFACE_IMPORT_BO?
        ((const dre_gpu_surface_import_request *)input)->surface_token:
        ((const dre_gpu_handle_request *)input)->handle;
    DRESurface *s=surface(token);if(!s)return DRE_GPU_STALE;
    if(selector==DRE_GPU_SURFACE_CLOSE)return closeSurface(s);
    if(selector==DRE_GPU_SURFACE_DESCRIBE) {
        dre_gpu_surface_reply *out=(dre_gpu_surface_reply *)output;out->surface_token=token;
        return dre_surface_describe(s->pin,&out->description);
    }
    if(selector==DRE_GPU_SURFACE_IMPORT_BO) {
        const dre_gpu_surface_import_request *r=(const dre_gpu_surface_import_request *)input;
        if(!(r->access&DRE_GPU_ACCESS_READ)||(r->access&~3u)||(r->access&~s->access))return DRE_GPU_INVALID;
        dre_gpu_surface_description description={};
        int status=dre_surface_describe(s->pin,&description);if(status!=DRE_GPU_OK)return status;
        if(r->plane>=description.plane_count)return DRE_GPU_INVALID;
        /* No GPU BO/VA is created. Existing live WB aliases, task-VM import,
         * descriptor/cache ownership and real cross-process shared events
         * must all be implemented before an importer can be added. A retained
         * descriptor or physical-page inventory alone grants none of them. */
        return DRE_GPU_UNSUPPORTED;
    }
    return DRE_GPU_UNSUPPORTED;
}
struct DREEpochContext { DREAdrenoGPUClient *client;DREBO *bo; };
static int epochMemory(void *cookie,const dre_gpu_access_epoch *s)
{
    DREEpochContext *c=(DREEpochContext *)cookie;DREBO *b=c->bo;
    dre_gpu_info_reply current={};c->client->gpu->info(&current);
    return c->client->gpu->owns()&&b->allocation&&!b->unsafe&&
        s->bo==b->handle&&s->session_generation==c->client->generation&&
        s->bytes==b->native.memory.bytes&&s->gpu_va==b->native.gpu_va&&
        s->gpu_mapping_lease==b->native.mapping_lease&&s->granted_access==b->native.access&&
        s->reset_generation==current.reset_generation&&
        c->client->gpu->verifyMemory(&b->native.memory)?1:0;
}
static int epochTaskMap(void *cookie,const dre_gpu_access_epoch *s)
{
    DREEpochContext *c=(DREEpochContext *)cookie;DREBO *b=c->bo;
    return b->cpuMap&&b->persistentMap&&b->mapLease==s->map_lease&&
        b->cpuMap->getVirtualAddress()==s->task_address&&b->cpuMap->getLength()==s->bytes&&
        c->client->gpu->taskProof(c->client->owner,s->task_address,&b->native.memory,b->cpuAccess)?1:0;
}
static int epochCache(void *cookie,uint32_t operation)
{
    DREEpochContext *c=(DREEpochContext *)cookie;
    return c->client->gpu->native.cache?c->client->gpu->native.cache(
        c->client->gpu->native.cookie,c->client->session,&c->bo->native,
        0,c->bo->native.memory.bytes,operation):DRE_GPU_UNSUPPORTED;
}
static int epochBegin(void *cookie,uint32_t access)
{
    DREEpochContext *c=(DREEpochContext *)cookie;
    return c->client->gpu->native.cpu_begin?c->client->gpu->native.cpu_begin(
        c->client->gpu->native.cookie,c->client->session,&c->bo->native,access):DRE_GPU_UNSUPPORTED;
}
static int epochEnd(void *cookie,uint32_t access)
{
    DREEpochContext *c=(DREEpochContext *)cookie;
    return c->client->gpu->native.cpu_end?c->client->gpu->native.cpu_end(
        c->client->gpu->native.cookie,c->client->session,&c->bo->native,access):DRE_GPU_UNSUPPORTED;
}
static int epochBarrier(void *cookie)
{
    DREEpochContext *c=(DREEpochContext *)cookie;
    DREAdrenoGPU::barrier(c->client->gpu);
    return c->client->gpu->owns()&&c->client->gpu->verifyMemory(&c->bo->native.memory)?DRE_GPU_OK:DRE_GPU_FAULT;
}
static int epochUnmap(void *cookie)
{
    DREEpochContext *c=(DREEpochContext *)cookie;DREBO *b=c->bo;
    if(!b->cpuMap||b->cpuMap->unmap()!=kIOReturnSuccess)return DRE_GPU_FAULT;
    OSSafeReleaseNULL(b->cpuMap);b->mapLease=0;b->cpuAccess=0;
    DREAdrenoGPU::barrier(c->client->gpu);return DRE_GPU_OK;
}
static int epochQuery(void *cookie,uint64_t ticket,dre_gpu_native_completion *out)
{
    DREEpochContext *c=(DREEpochContext *)cookie;
    return c->client->gpu->native.query?c->client->gpu->native.query(
        c->client->gpu->native.cookie,c->client->session,ticket,out):DRE_GPU_UNSUPPORTED;
}
static dre_gpu_access_ops epochOps(DREEpochContext *c)
{
    dre_gpu_access_ops result={c,epochMemory,epochTaskMap,epochCache,epochBegin,
        epochEnd,epochBarrier,epochUnmap,epochQuery};return result;
}
int DREAdrenoGPUClient::mapPersistent(const dre_gpu_cpu_map_request *r,dre_gpu_cpu_map_reply *out)
{
    dre_gpu_info_reply info={};gpu->info(&info);
    /* No current provider qualifies this capability. The complete implementation
     * below cannot be reached by merely having a long-lived IOMemoryMap. */
    if(!(info.capabilities&DRE_GPU_CAP_PERSISTENT_CPU_MAP)||
       info.cache_contract!=DRE_GPU_CACHE_OWNED_NORMAL_NC_PERSISTENT||exclusiveVM)
        return DRE_GPU_UNSUPPORTED;
    DREBO *b=bo(r->bo);if(!b)return DRE_GPU_STALE;
    if(r->reserved||!r->access||(r->access&~3u))return DRE_GPU_INVALID;
    if(b->cpuMap||b->pins||b->unsafe||b->accessEpoch.active_epoch||b->accessEpoch.ticket||
       (b->accessEpoch.initialized&&b->accessEpoch.phase!=DRE_GPU_ACCESS_IDLE))return DRE_GPU_BUSY;
    if(!gpu->native.cache||!gpu->native.cpu_begin||!gpu->native.cpu_end||!gpu->native.query||
       !gpu->taskProofAvailable()||!gpu->verifyMemory(&b->native.memory))return DRE_GPU_UNSUPPORTED;
    if(nextMap==UINT64_MAX)return DRE_GPU_LIMIT;
    IOOptionBits options=kIOMapAnywhere|kIOMapUnique|kIOMapWriteCombineCache|kIOMapPrefault;
    if(!(r->access&DRE_GPU_CPU_WRITE))options|=kIOMapReadOnly;
    b->cpuMap=b->allocation->descriptor->createMappingInTask(owner,0,options);
    if(!b->cpuMap)return DRE_GPU_NO_MEMORY;
    b->persistentMap=true;b->cpuAccess=r->access;b->mapLease=++nextMap;
    uint64_t address=b->cpuMap->getVirtualAddress();
    DREEpochContext c={this,b};dre_gpu_access_ops ops=epochOps(&c);
    int status=dre_gpu_access_attach(&b->accessEpoch,&ops,b->handle,generation,
        b->native.gpu_va,b->native.mapping_lease,b->native.memory.bytes,b->native.access,
        info.reset_generation,b->mapLease,address);
    if(status!=DRE_GPU_OK) {
        /* An unproven cache alias cannot be handed to userspace or freed.
         * Attempt actual revocation, then retain unsafe physical ownership. */
        (void)epochUnmap(&c);b->unsafe=true;sessionFault=true;return status;
    }
    out->lease=b->mapLease;out->task_address=address;out->bytes=b->native.memory.bytes;
    out->cache_contract=DRE_GPU_CACHE_OWNED_NORMAL_NC_PERSISTENT;
    return DRE_GPU_OK; /* Pointer lifetime only; no CPU access epoch acquired. */
}
int DREAdrenoGPUClient::cpuAccess(uint32_t selector,const dre_gpu_cpu_access_request *r,void *output)
{
    dre_gpu_info_reply info={};gpu->info(&info);
    if(!(info.capabilities&DRE_GPU_CAP_PERSISTENT_CPU_MAP)||exclusiveVM)return DRE_GPU_UNSUPPORTED;
    DREBO *b=bo(r->bo);if(!b)return DRE_GPU_STALE;
    if(r->reserved||!b->persistentMap||!b->cpuMap||!r->map_lease||r->map_lease!=b->mapLease)
        return DRE_GPU_STALE;
    if(b->pins||b->unsafe)return DRE_GPU_BUSY;
    DREEpochContext c={this,b};dre_gpu_access_ops ops=epochOps(&c);int status;
    if(selector==DRE_GPU_CPU_ACCESS_BEGIN) {
        if(r->epoch||!r->access||(r->access&~b->cpuAccess))return DRE_GPU_INVALID;
        status=dre_gpu_access_cpu_begin(&b->accessEpoch,&ops,r->access,
            &((dre_gpu_cpu_access_reply *)output)->epoch);
    } else {
        if(!r->epoch||r->access)return DRE_GPU_INVALID;
        status=dre_gpu_access_cpu_end(&b->accessEpoch,&ops,r->epoch);
    }
    if(b->accessEpoch.unsafe){b->unsafe=true;sessionFault=true;}
    return status;
}
int DREAdrenoGPUClient::unmap(DREBO *b)
{
    if(!b->cpuMap) return DRE_GPU_OK;
    if(b->persistentMap) {
        DREEpochContext c={this,b};dre_gpu_access_ops ops=epochOps(&c);
        int status=dre_gpu_access_detach(&b->accessEpoch,&ops);
        if(status!=DRE_GPU_OK||b->accessEpoch.unsafe){b->unsafe=true;sessionFault=true;}
        return status;
    }
    if(b->cpuMap->unmap()!=kIOReturnSuccess) {
        b->unsafe=true;sessionFault=true;return DRE_GPU_FAULT;
    }
    OSSafeReleaseNULL(b->cpuMap);b->mapLease=0;
    DREAdrenoGPU::barrier(gpu);
    int r=gpu->native.cpu_end?gpu->native.cpu_end(gpu->native.cookie,session,
        &b->native,b->cpuAccess):DRE_GPU_UNSUPPORTED;
    b->cpuAccess=0;
    if(r!=DRE_GPU_OK) { b->unsafe=true;sessionFault=true; }
    return r;
}
int DREAdrenoGPUClient::retireBO(DREBO *b)
{
    if(!b->allocation) return DRE_GPU_OK;
    if(b->pins) return DRE_GPU_BUSY;
    if(b->accessEpoch.initialized&&(b->accessEpoch.unsafe||b->accessEpoch.active_epoch||b->accessEpoch.ticket))
        return DRE_GPU_BUSY;
    int r=unmap(b); if(r!=DRE_GPU_OK) return r;
    if(!exclusiveVM&&!gpu->native.bo_retire) return DRE_GPU_UNSUPPORTED;
    /* Includes real GPU cache/firmware-reference retirement and actual
     * leaf removal + SMMU TLB synchronization. Even an errored/closed handle
     * remains owned on failure, including CPU-page padding and page metadata. */
    if(!b->gpuRetired) {
        r=exclusiveVM?dre_gpu_vm_binding_retire(gpu->exclusiveVM,(uint64_t)(uintptr_t)owner,
            generation,&b->native):gpu->native.bo_retire(gpu->native.cookie,session,&b->native);
        if(r!=DRE_GPU_OK) return r;
        b->gpuRetired=true;
    }
    uint64_t bytes=b->native.memory.bytes;
    if(bytes>wiredBytes) { b->unsafe=true;sessionFault=true;return DRE_GPU_FAULT; }
    r=gpu->releaseRetired(b->allocation);if(r!=DRE_GPU_OK)return r;
    bzero(b,sizeof(*b));
    wiredBytes-=bytes;return DRE_GPU_OK;
}
int DREAdrenoGPUClient::refresh(DREJob *j)
{
    if(j->retired) return DRE_GPU_OK;
    if(j->uncertain||!j->ticket||!gpu->native.query) return DRE_GPU_FAULT;
    dre_gpu_native_completion c={};
    int r=gpu->native.query(gpu->native.cookie,session,j->ticket,&c);
    if(r!=DRE_GPU_OK) return r;
    if(c.reserved||c.resources_retired>1||c.state>DRE_GPU_FENCE_RESET||
       (c.state==DRE_GPU_FENCE_PENDING&&c.resources_retired)||
       (c.state==DRE_GPU_FENCE_COMPLETE&&c.hardware_error)) {
        j->uncertain=true;sessionFault=true;return DRE_GPU_FAULT;
    }
    /* Terminal records cannot later regress or change reset generations. */
    if(j->completion.state!=DRE_GPU_FENCE_PENDING &&
       (c.state!=j->completion.state||c.reset_generation!=j->completion.reset_generation||
        c.hardware_error!=j->completion.hardware_error)) {
        j->uncertain=true;sessionFault=true;return DRE_GPU_FAULT;
    }
    j->completion=c;
    if(c.state!=DRE_GPU_FENCE_PENDING&&c.resources_retired) {
        for(uint32_t i=0;i<j->resourceCount;i++) {
            if(!j->resources[i]||!j->resources[i]->pins) {
                j->uncertain=true;sessionFault=true;return DRE_GPU_FAULT;
            }
        }
        for(uint32_t i=0;i<j->waitCount;i++) if(!j->waits[i]||!j->waits[i]->dependents) {
            j->uncertain=true;sessionFault=true;return DRE_GPU_FAULT;
        }
        for(uint32_t i=0;i<j->resourceCount;i++) j->resources[i]->pins--;
        for(uint32_t i=0;i<j->waitCount;i++) j->waits[i]->dependents--;
        j->retired=true;
    }
    return DRE_GPU_OK;
}
bool DREAdrenoGPUClient::reap()
{
    if(closing)for(unsigned i=0;i<DRE_GPU_MAX_SURFACES;i++)
        if(surfaces[i].pin)(void)closeSurface(&surfaces[i]);
    for(unsigned i=0;i<DRE_GPU_MAX_FENCES;i++) if(jobs[i].used) {
        DREJob *j=&jobs[i];(void)refresh(j);
        if(closing&&j->retired&&!j->dependents&&gpu->native.ticket_close&&
           gpu->native.ticket_close(gpu->native.cookie,session,j->ticket)==DRE_GPU_OK)
            bzero(j,sizeof(*j));
    }
    for(unsigned i=0;i<DRE_GPU_MAX_BOS;i++) if(bos[i].allocation) {
        if(closing) bos[i].closed=true;
        if(bos[i].closed) (void)retireBO(&bos[i]);
    }
    if(!closing) return false;
    for(unsigned i=0;i<DRE_GPU_MAX_FENCES;i++) if(jobs[i].used) return false;
    for(unsigned i=0;i<DRE_GPU_MAX_BOS;i++) if(bos[i].allocation) return false;
    for(unsigned i=0;i<DRE_GPU_MAX_SURFACES;i++) if(surfaces[i].pin) return false;
    if(session) {
        int result=exclusiveVM?dre_gpu_vm_binding_close(gpu->exclusiveVM,
            (uint64_t)(uintptr_t)owner,generation):
            (gpu->native.session_close?gpu->native.session_close(gpu->native.cookie,session):DRE_GPU_UNSUPPORTED);
        if(result!=DRE_GPU_OK)
            return false;
        session=NULL;
    }
    return true;
}
void DREAdrenoGPU::poll(OSObject *owner,IOTimerEventSource *source)
{
    DREAdrenoGPU *g=OSDynamicCast(DREAdrenoGPU,owner);
    if(!g||!source) return;
    g->ownerEnter();
    for(unsigned i=0;i<g->clients->getCount();) {
        DREAdrenoGPUClient *c=OSDynamicCast(DREAdrenoGPUClient,g->clients->getObject(i));
        if(c&&c->reap()) g->clients->removeObject(i);
        else i++;
    }
    g->ownerLeave();source->setTimeoutMS(100);
}
IOReturn DREAdrenoGPUClient::clientClose()
{
    if(gpu&&gpu->lock) {
        gpu->ownerEnter();closing=true;
        for(unsigned i=0;i<DRE_GPU_MAX_BOS;i++) if(bos[i].allocation) {
            bos[i].closed=true;(void)unmap(&bos[i]);
        }
        gpu->ownerLeave();
    }
    /* The timer retains/reaps uncertain GPU jobs after the task disappears. */
    terminate();return kIOReturnSuccess;
}
IOReturn DREAdrenoGPUClient::clientDied() { return clientClose(); }

int DREAdrenoGPUClient::submit(const dre_gpu_submit_request *r,dre_gpu_submit_reply *out)
{
    dre_gpu_info_reply info={};gpu->info(&info);
    const uint64_t need=DRE_GPU_CAP_RAW_IB_SUBMIT|DRE_GPU_CAP_HARDWARE_FENCE|
        DRE_GPU_CAP_SINGLE_FIFO|DRE_GPU_CAP_OWNED_TASK_BO|DRE_GPU_CAP_EXPLICIT_CACHE;
    if((info.capabilities&need)!=need||!gpu->native.submit||!gpu->native.query)
        return DRE_GPU_UNSUPPORTED;
    if(!r->ib_count||r->ib_count>DRE_GPU_MAX_IBS||!r->resource_count||
       r->resource_count>DRE_GPU_MAX_BOS||r->wait_count>DRE_GPU_MAX_WAITS||r->reserved||
       !zeroBytes(r->ibs+r->ib_count,sizeof(r->ibs[0])*(DRE_GPU_MAX_IBS-r->ib_count))||
       !zeroBytes(r->resources+r->resource_count,sizeof(r->resources[0])*(DRE_GPU_MAX_BOS-r->resource_count))||
       !zeroBytes(r->wait_fences+r->wait_count,sizeof(uint64_t)*(DRE_GPU_MAX_WAITS-r->wait_count)))
        return DRE_GPU_INVALID;
    dre_gpu_native_submit native={};
    native.ib_count=r->ib_count;native.resource_count=r->resource_count;native.wait_count=r->wait_count;
    DREBO *resources[DRE_GPU_MAX_BOS]={};
    DREJob *waits[DRE_GPU_MAX_WAITS]={};
    for(uint32_t i=0;i<r->resource_count;i++) {
        const dre_gpu_submit_resource *res=&r->resources[i];
        DREBO *b=bo(res->bo);
        if(!b||!res->access||(res->access&~3u)||res->reserved||
           (res->access&~b->native.access)) return DRE_GPU_INVALID;
        if(b->cpuMap||b->unsafe) return DRE_GPU_BUSY;
        for(uint32_t k=0;k<i;k++) if(resources[k]==b) return DRE_GPU_INVALID;
        resources[i]=b;native.resources[i]={&b->native,res->access,0};
    }
    for(uint32_t i=0;i<r->ib_count;i++) {
        const dre_gpu_submit_ib *ib=&r->ibs[i];DREBO *b=bo(ib->bo);
        if(!b||!ib->bytes||((ib->offset|ib->bytes)&3)||
           !(b->native.access&DRE_GPU_ACCESS_EXECUTE)||ib->offset>b->native.memory.bytes||
           ib->bytes>b->native.memory.bytes-ib->offset) return DRE_GPU_INVALID;
        bool listed=false;
        for(uint32_t k=0;k<r->resource_count;k++) if(resources[k]==b&&
            (r->resources[k].access&DRE_GPU_ACCESS_READ)) listed=true;
        if(!listed) return DRE_GPU_INVALID;
        native.ibs[i]={&b->native,ib->offset,ib->bytes};
    }
    for(uint32_t i=0;i<r->wait_count;i++) {
        DREJob *wait=job(r->wait_fences[i]);
        if(!wait||wait->uncertain||!wait->ticket) return DRE_GPU_STALE;
        for(uint32_t k=0;k<i;k++) if(waits[k]==wait) return DRE_GPU_INVALID;
        waits[i]=wait;
        native.wait_tickets[i]=wait->ticket;
    }
    unsigned slot=0;
    while(slot<DRE_GPU_MAX_FENCES&&jobs[slot].used) slot++;
    if(slot==DRE_GPU_MAX_FENCES||nextFence>=(UINT64_MAX>>8)) return DRE_GPU_LIMIT;
    DREJob *j=&jobs[slot];bzero(j,sizeof(*j));
    j->handle=(++nextFence<<8)|(slot+1);j->used=true;
    j->resourceCount=r->resource_count;
    j->waitCount=r->wait_count;
    for(uint32_t i=0;i<j->resourceCount;i++) { j->resources[i]=resources[i];resources[i]->pins++; }
    for(uint32_t i=0;i<j->waitCount;i++) { j->waits[i]=waits[i];waits[i]->dependents++; }
    uint32_t published=0;
    int status=gpu->native.submit(gpu->native.cookie,session,&native,&j->ticket,&published);
    if(!published&&!j->ticket&&status!=DRE_GPU_OK) {
        for(uint32_t i=0;i<j->resourceCount;i++) resources[i]->pins--;
        for(uint32_t i=0;i<j->waitCount;i++) waits[i]->dependents--;
        bzero(j,sizeof(*j));return status;
    }
    out->fence=j->handle;out->reset_generation=info.reset_generation;
    if(published!=1||!j->ticket) {
        j->uncertain=true;sessionFault=true;return DRE_GPU_FAULT;
    }
    /* Published errors retain the returned fence and every resource. The
     * caller must query/close even if the reply status reports a GPU fault. */
    if(status!=DRE_GPU_OK) sessionFault=true;
    return status;
}

int DREAdrenoGPUClient::dispatch(uint32_t selector,const void *input,void *output)
{
    if(selector==DRE_GPU_GET_INFO) {
        dre_gpu_info_reply *out=(dre_gpu_info_reply *)output;gpu->info(out);
        if(!session||sessionFault||closing) out->capabilities=0;
        return DRE_GPU_OK;
    }
    if(selector>=DRE_GPU_SURFACE_RETAIN&&selector<=DRE_GPU_SURFACE_IMPORT_BO)
        return surfaceCall(selector,input,output);
    if(selector==DRE_GPU_CPU_UNMAP) {
        uint64_t h=((const dre_gpu_handle_request *)input)->handle;
        for(unsigned i=0;i<DRE_GPU_MAX_BOS;i++)
            if(bos[i].allocation&&bos[i].cpuMap&&bos[i].mapLease==h) return unmap(&bos[i]);
        return DRE_GPU_STALE;
    }
    if(selector==DRE_GPU_BO_CLOSE) {
        DREBO *b=bo(((const dre_gpu_handle_request *)input)->handle);
        if(!b) return DRE_GPU_STALE;
        b->closed=true;
        int r=unmap(b);if(r!=DRE_GPU_OK) return r;
        /* The handle is closed now. In-flight jobs retain its allocation,
         * mapping lease and slot until asynchronous retirement succeeds. */
        (void)retireBO(b);return DRE_GPU_OK;
    }
    if(selector==DRE_GPU_FENCE_CLOSE) {
        DREJob *j=job(((const dre_gpu_handle_request *)input)->handle);
        if(!j) return DRE_GPU_STALE;
        int r=refresh(j);if(r!=DRE_GPU_OK) return r;
        if(!j->retired||j->dependents) return DRE_GPU_BUSY;
        if(!gpu->native.ticket_close) return DRE_GPU_UNSUPPORTED;
        r=gpu->native.ticket_close(gpu->native.cookie,session,j->ticket);
        if(r==DRE_GPU_OK) bzero(j,sizeof(*j));
        return r;
    }
    if(selector==DRE_GPU_QUERY_FENCE||selector==DRE_GPU_WAIT_FENCE) {
        const dre_gpu_fence_request *r=(const dre_gpu_fence_request *)input;
        dre_gpu_fence_reply *out=(dre_gpu_fence_reply *)output;
        if(r->timeout_ns>DRE_GPU_WAIT_MAX_NS||
           (selector==DRE_GPU_QUERY_FENCE&&r->timeout_ns)) return DRE_GPU_INVALID;
        uint64_t deadline=0;
        clock_interval_to_deadline((uint32_t)((r->timeout_ns+999999)/1000000),
            kMillisecondScale,&deadline);
        for(;;) {
            DREJob *j=job(r->fence);if(!j) return DRE_GPU_STALE;
            int status=refresh(j);if(status!=DRE_GPU_OK) return status;
            out->fence=r->fence;out->reset_generation=j->completion.reset_generation;
            out->hardware_error=j->completion.hardware_error;
            out->state=j->completion.state;
            /* COMPLETE promises usable data, not merely CP consumed a ring.
             * Hold it pending until the real cache/ref retirement completes. */
            if(out->state==DRE_GPU_FENCE_COMPLETE&&!j->retired)
                out->state=DRE_GPU_FENCE_PENDING;
            if(out->state!=DRE_GPU_FENCE_PENDING) return DRE_GPU_OK;
            if(selector==DRE_GPU_QUERY_FENCE) return DRE_GPU_OK;
            if(mach_absolute_time()>=deadline) return DRE_GPU_TIMEOUT;
            /* Never hold the device lock while sleeping. Re-resolve opaque
             * handles after waking: another call may close a completed one. */
            gpu->ownerLeave();IOSleep(1);gpu->ownerEnter();
            if(closing) return DRE_GPU_CLOSED;
        }
    }
    if(!session||sessionFault||!gpu->initialized||!gpu->owns()) return DRE_GPU_NOT_READY;
    dre_gpu_info_reply current={};gpu->info(&current);
    if(!(current.capabilities&DRE_GPU_CAP_OWNED_TASK_BO)) return DRE_GPU_NOT_READY;
    if(selector==DRE_GPU_BO_CREATE) {
        const dre_gpu_bo_create_request *r=(const dre_gpu_bo_create_request *)input;
        dre_gpu_bo_create_reply *out=(dre_gpu_bo_create_reply *)output;
        if(r->reserved||!goodAccess(r->access)||!r->bytes||r->bytes>DRE_GPU_BO_MAX_BYTES)
            return DRE_GPU_INVALID;
        uint64_t size=(r->bytes+DRE_GPU_CPU_PAGE_BYTES-1)&~(uint64_t)(DRE_GPU_CPU_PAGE_BYTES-1);
        if(surfaceBytes>DRE_GPU_TASK_MAX_BYTES-wiredBytes||
           size>DRE_GPU_TASK_MAX_BYTES-wiredBytes-surfaceBytes||nextBO>=(UINT64_MAX>>8))return DRE_GPU_LIMIT;
        unsigned slot=0;while(slot<DRE_GPU_MAX_BOS&&bos[slot].allocation) slot++;
        if(slot==DRE_GPU_MAX_BOS) return DRE_GPU_LIMIT;
        DREBO *b=&bos[slot];bzero(b,sizeof(*b));
        int status=gpu->allocate(size,r->access,&b->allocation,&b->native.memory);
        if(status!=DRE_GPU_OK) return status;
        wiredBytes+=size;b->native.access=r->access;
        status=gpu->classifyTask(b->allocation,this);
        if(status!=DRE_GPU_OK) {
            b->closed=b->unsafe=true;sessionFault=true;return status;
        }
        b->handle=(++nextBO<<8)|(slot+1);
        b->allocation->published=true;
        if(!gpu->verifyMemory(&b->native.memory))status=DRE_GPU_NOT_READY;
        else if(exclusiveVM)status=dre_gpu_vm_binding_map(gpu->exclusiveVM,
            (uint64_t)(uintptr_t)owner,generation,&b->native.memory,r->access,
            &b->native.gpu_va,&b->native.mapping_lease);
        else status=gpu->native.bo_map(gpu->native.cookie,session,&b->native.memory,r->access,
            &b->native.gpu_va,&b->native.mapping_lease);
        if(status!=DRE_GPU_OK||!b->native.mapping_lease||
           !b->native.gpu_va||(b->native.gpu_va&(DRE_GPU_PAGE_BYTES-1))||
           b->native.gpu_va>UINT64_MAX-size) {
            /* A failed map may already have published PTEs; no speculative
             * free of these pages. Keep an orphan until real retirement. */
            b->closed=b->unsafe=true;sessionFault=true;
            return status==DRE_GPU_OK?DRE_GPU_FAULT:status;
        }
        if(exclusiveVM) {
            dre_gpu_vm_entry *actual=NULL;
            for(unsigned i=0;i<DRE_GPU_MAX_BOS;i++) {
                dre_gpu_vm_entry *e=&gpu->exclusiveVM->entries[i];
                if(e->held&&!e->uncertain&&e->mapping.lease==b->native.mapping_lease&&
                   e->gpu_va==b->native.gpu_va&&dre_gpu_catalog_memory_same(&e->memory,&b->native.memory)) {
                    if(actual){actual=NULL;break;}actual=e;
                }
            }
            if(!actual||gpu->recordMapping(b->allocation,gpu->exclusiveVM->vm,
               gpu->exclusiveVM->ops,&actual->mapping)!=DRE_GPU_OK) {
                b->closed=b->unsafe=true;sessionFault=true;return DRE_GPU_NOT_READY;
            }
        } /* Generic bo_map scalar VA is deliberately not a mapping proof. */
        out->bo=b->handle;out->gpu_va=b->native.gpu_va;out->bytes=size;
        out->access=r->access;out->cache_contract=b->native.memory.cache_contract;
        return DRE_GPU_OK;
    }
    if(selector==DRE_GPU_CPU_MAP) {
        if(!(current.capabilities&DRE_GPU_CAP_CPU_MAP_LEASE)) return DRE_GPU_UNSUPPORTED;
        const dre_gpu_cpu_map_request *r=(const dre_gpu_cpu_map_request *)input;
        dre_gpu_cpu_map_reply *out=(dre_gpu_cpu_map_reply *)output;
        DREBO *b=bo(r->bo);
        if(!b) return DRE_GPU_STALE;
        if(b->persistentMap||b->accessEpoch.initialized)return DRE_GPU_UNSUPPORTED;
        if(r->reserved||!r->access||(r->access&~3u)) return DRE_GPU_INVALID;
        if(b->cpuMap||b->pins||b->unsafe) return DRE_GPU_BUSY;
        if(!gpu->verifyMemory(&b->native.memory))return DRE_GPU_NOT_READY;
        if(!gpu->native.cpu_begin||!gpu->native.cpu_end||!gpu->taskProofAvailable())
            return DRE_GPU_UNSUPPORTED;
        if(nextMap==UINT64_MAX) return DRE_GPU_LIMIT;
        int status=gpu->native.cpu_begin(gpu->native.cookie,session,&b->native,r->access);
        if(status!=DRE_GPU_OK) return status;
        b->cpuAccess=r->access;
        IOOptionBits options=kIOMapAnywhere|kIOMapUnique|kIOMapWriteCombineCache|kIOMapPrefault;
        if(!(r->access&DRE_GPU_CPU_WRITE)) options|=kIOMapReadOnly;
        b->cpuMap=b->allocation->descriptor->createMappingInTask(owner,0,options);
        if(!b->cpuMap) {
            status=gpu->native.cpu_end(gpu->native.cookie,session,&b->native,r->access);
            b->cpuAccess=0;
            if(status!=DRE_GPU_OK) { b->unsafe=true;sessionFault=true;return status; }
            return DRE_GPU_NO_MEMORY;
        }
        uint64_t address=b->cpuMap->getVirtualAddress();
        if(!address||b->cpuMap->getLength()!=b->native.memory.bytes||
           !gpu->taskProof(owner,address,&b->native.memory,r->access)) {
            status=unmap(b);return status==DRE_GPU_OK?DRE_GPU_NOT_READY:status;
        }
        b->mapLease=++nextMap;
        out->lease=b->mapLease;out->task_address=address;out->bytes=b->native.memory.bytes;
        out->cache_contract=b->native.memory.cache_contract;return DRE_GPU_OK;
    }
    if(selector==DRE_GPU_CPU_MAP_PERSISTENT)return mapPersistent(
        (const dre_gpu_cpu_map_request *)input,(dre_gpu_cpu_map_reply *)output);
    if(selector==DRE_GPU_CPU_ACCESS_BEGIN||selector==DRE_GPU_CPU_ACCESS_END)
        return cpuAccess(selector,(const dre_gpu_cpu_access_request *)input,output);
    if(selector==DRE_GPU_CACHE_OP) {
        if(!(current.capabilities&DRE_GPU_CAP_EXPLICIT_CACHE)) return DRE_GPU_UNSUPPORTED;
        const dre_gpu_cache_request *r=(const dre_gpu_cache_request *)input;
        DREBO *b=bo(r->bo);if(!b) return DRE_GPU_STALE;
        if(r->reserved||!r->bytes||r->offset>b->native.memory.bytes||
           r->bytes>b->native.memory.bytes-r->offset||
           (r->operation!=DRE_GPU_CACHE_TO_GPU&&r->operation!=DRE_GPU_CACHE_FROM_GPU))
            return DRE_GPU_INVALID;
        if(b->cpuMap||b->pins||b->unsafe) return DRE_GPU_BUSY;
        return gpu->native.cache?gpu->native.cache(gpu->native.cookie,session,&b->native,
            r->offset,r->bytes,r->operation):DRE_GPU_UNSUPPORTED;
    }
    if(selector==DRE_GPU_SUBMIT) return submit((const dre_gpu_submit_request *)input,
        (dre_gpu_submit_reply *)output);
    return DRE_GPU_UNSUPPORTED;
}

IOReturn DREAdrenoGPUClient::externalMethod(uint32_t selector,IOExternalMethodArguments *args,
    IOExternalMethodDispatch *,OSObject *,void *)
{
    static const size_t ins[DRE_GPU_SELECTOR_COUNT]={
        sizeof(dre_gpu_info_request),sizeof(dre_gpu_bo_create_request),sizeof(dre_gpu_handle_request),
        sizeof(dre_gpu_submit_request),sizeof(dre_gpu_fence_request),sizeof(dre_gpu_fence_request),
        sizeof(dre_gpu_cache_request),sizeof(dre_gpu_cpu_map_request),sizeof(dre_gpu_handle_request),
        sizeof(dre_gpu_handle_request),sizeof(dre_gpu_cpu_map_request),
        sizeof(dre_gpu_cpu_access_request),sizeof(dre_gpu_cpu_access_request),
        sizeof(dre_gpu_surface_retain_request),sizeof(dre_gpu_handle_request),
        sizeof(dre_gpu_handle_request),sizeof(dre_gpu_surface_import_request)};
    static const size_t outs[DRE_GPU_SELECTOR_COUNT]={
        sizeof(dre_gpu_info_reply),sizeof(dre_gpu_bo_create_reply),sizeof(dre_gpu_status_reply),
        sizeof(dre_gpu_submit_reply),sizeof(dre_gpu_fence_reply),sizeof(dre_gpu_fence_reply),
        sizeof(dre_gpu_status_reply),sizeof(dre_gpu_cpu_map_reply),sizeof(dre_gpu_status_reply),
        sizeof(dre_gpu_status_reply),sizeof(dre_gpu_cpu_map_reply),
        sizeof(dre_gpu_cpu_access_reply),sizeof(dre_gpu_status_reply),
        sizeof(dre_gpu_surface_reply),sizeof(dre_gpu_surface_reply),
        sizeof(dre_gpu_status_reply),sizeof(dre_gpu_bo_create_reply)};
    if(!args||selector>=DRE_GPU_SELECTOR_COUNT||!gpu||!owner||current_task()!=owner||
       args->scalarInputCount||args->scalarOutputCount||args->asyncReferenceCount||args->asyncWakePort||
       args->structureInputDescriptor||args->structureOutputDescriptor||
       !args->structureInput||!args->structureOutput||args->structureInputSize!=ins[selector]||
       args->structureOutputSize!=outs[selector]) return kIOReturnBadArgument;
    /* Copy every command once before locking or examining it. Never parse
     * mutable structureInput pages or user-pointer command arrays in place. */
    union { uint64_t align; uint8_t bytes[sizeof(dre_gpu_submit_request)]; } request;
    bzero(&request,sizeof(request));bcopy(args->structureInput,request.bytes,ins[selector]);
    const dre_gpu_request_header *h=(const dre_gpu_request_header *)request.bytes;
    if(h->abi_version!=DRE_GPU_ABI_VERSION||h->struct_bytes!=ins[selector]||h->reserved||
       (selector==DRE_GPU_GET_INFO?(h->session_generation&&h->session_generation!=generation):
        h->session_generation!=generation)) return kIOReturnBadArgument;
    bzero(args->structureOutput,outs[selector]);
    gpu->ownerEnter();
    int status=closing?DRE_GPU_CLOSED:dispatch(selector,request.bytes,args->structureOutput);
    dre_gpu_reply_header *reply=(dre_gpu_reply_header *)args->structureOutput;
    reply->abi_version=DRE_GPU_ABI_VERSION;reply->struct_bytes=(uint32_t)outs[selector];
    reply->status=status;reply->flags=0;reply->session_generation=generation;
    gpu->ownerLeave();return kIOReturnSuccess;
}

static DREAdrenoGPU *residentGPU;
static volatile uint32_t publishOwner;
extern "C" int dre_gpu_native_publish(const dre_gpu_native_provider *provider)
{
    int enabled=0;PE_parse_boot_argn("dre_gpu_native",&enabled,sizeof(enabled));
    if(enabled!=1) return DRE_GPU_UNSUPPORTED;
    if(!provider||provider->version!=DRE_GPU_PROVIDER_VERSION||
       provider->struct_bytes!=sizeof(*provider))return DRE_GPU_INVALID;
    dre_gpu_native_provider configuration=*provider;
    if(!configuration.fdt) {
        if(configuration.fdt_bytes)return DRE_GPU_INVALID;
        size_t bytes=0;
        if(dre_native_board_snapshot(&configuration.fdt,&bytes)!=0)return DRE_GPU_NOT_READY;
        configuration.fdt_bytes=bytes;
    }
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&publishOwner,&expected,1,false,__ATOMIC_ACQ_REL,
        __ATOMIC_ACQUIRE)) return DRE_GPU_BUSY;
    DREAdrenoGPU *g=new DREAdrenoGPU;
    if(!g) { __atomic_store_n(&publishOwner,0,__ATOMIC_RELEASE);return DRE_GPU_NO_MEMORY; }
    if(!g->initNative(&configuration)||!g->attach(IOService::getResourceService())||
       !g->start(IOService::getResourceService())) {
        if(g->permanentHold) {
            residentGPU=g;return DRE_GPU_FAULT; /* actual holds survive failure */
        }
        g->detach(IOService::getResourceService());g->release();
        __atomic_store_n(&publishOwner,0,__ATOMIC_RELEASE);return DRE_GPU_NOT_READY;
    }
    /* Built-in native provider ownership has no unload path. Retain even
     * without a Metal frontend; clients/timer never outlive their MMIO owner. */
    residentGPU=g;return DRE_GPU_OK;
}
#endif /* DRE */
