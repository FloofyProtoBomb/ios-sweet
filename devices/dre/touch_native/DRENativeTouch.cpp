/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight source: no builds/tests/phone execution performed.
 * Actual XNU memory/work-loop/IOService/IRQ glue for the bounded touch engine.
 * No default board-policy, IRQ-route or SMMU provider is installed.
 */
#include "xnu_provider.h"
#include "xnu_irq_route.h"
#include "xnu_tlmm.h"
#if defined(DRE)
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/task.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <vm/memory_types.h>
#include <vm/pmap.h>
extern "C" {
#include "input.h"
#include "../libfdt/libfdt.h"
#include "../native_board.h"
#include <libkern/crypto/sha2.h>
#include <kern/locks.h>
vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned);
void ml_io_unmap(vm_offset_t, vm_size_t);
uint64_t kvtophys(vm_offset_t);
int dre_dma_ram_allowed(const void *, uint64_t, uint64_t);
}

enum { CPU_PAGE = 16384, DMA_PAGE = 4096, PAGES_PER_BLOCK = 4, REGISTER_MAPS = 6 };
static LCK_GRP_DECLARE(dre_touch_qup_group,"dre_touch_qup");
static LCK_MTX_DECLARE(dre_touch_qup_lock,&dre_touch_qup_group);
static dre_tn_qup_registry dre_touch_qup_registry;
struct DRETouchMap { uint64_t pa, bytes; vm_offset_t cpu; };
struct DRETouchAllocation {
    DRETouchAllocation *next;
    IOBufferMemoryDescriptor *descriptor;
    dre_touch_xnu_page pages[PAGES_PER_BLOCK];
    dre_tn_memory view;
    dre_tn_smmu_token mapping;
    unsigned permissions, role;
    bool prepared;
};
class DRENativeTouch;
class DRETouchWork : public IOEventSource {
    OSDeclareDefaultStructors(DRETouchWork);
public:
    DRENativeTouch *touch;
    uint32_t pending;
    bool initTouch(DRENativeTouch *);
    void post();
    void discard();
    bool checkForWork() APPLE_KEXT_OVERRIDE;
};
class DRENativeTouch : public IOService {
    OSDeclareDefaultStructors(DRENativeTouch);
public:
    dre_touch_xnu_provider native;
    dre_tn_profile profile;
    dre_tn_spi spi;
    dre_tn_controller controller;
    dre_tn_input input;
    dre_tn_smmu_profile smmuProfile;
    dre_tn_smmu_reservation smmuReservation;
    dre_tn_smmu smmu;
    dre_tn_qup_claim qupClaim;
    dre_tn_qup_snapshot qupSnapshot;
    dre_tn_qup_clock qupClock;
    DRETouchIRQRoute gpioRoute;
    dre_tn_tlmm_pin *gpioPin;
    void *fdt, *nativeLease, *domainLease;
    size_t fdtBytes;
    IOService *hid, *gpioNub;
    unsigned gpioSource, gpii, allocationRole;
    DRETouchMap maps[REGISTER_MAPS];
    DRETouchMap smmuMaps[2];
    DRETouchAllocation *allocations, *tableAllocation;
    IOWorkLoop *loop;
    DRETouchWork *work;
    IOTimerEventSource *retry;
    uint32_t closing, irqFault, dispatchEnabled, peripheralMutated;
    bool irqRegistered, hold, attached, inputStarted, started;
    enum dre_tn_result result;

    bool initNative(const dre_touch_xnu_provider *, IOService *);
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    bool owns();
    int shutdown();
    void runWorker();
    void sourceFault(); /* IRQ-safe flag/post only; no hardware or HID work. */
    int allocate(size_t, size_t, dre_tn_memory *);
    int prepareBacking(DRETouchAllocation *);
    int verify(const dre_tn_memory *, unsigned);
    int retire(dre_tn_memory *);
    int reserve(const dre_tn_profile *, void **, unsigned *);
    int releaseLease(void *);
    int openDomain();
    int closeDomain();
    bool readSMMU(uint64_t, void *, unsigned);
    bool writeSMMU(uint64_t, uint64_t, unsigned);
    bool mapRegisters();
    void unmapRegisters();
    bool readRegister(uint64_t, uint32_t *);
    bool readObserved(uint64_t, uint32_t *);
    int observeClock();
    int claimQUP();
    int snapshotQUP();
    void publishClockEvidence();
    bool writeRegister(uint64_t, uint32_t);
    int maskPin(bool acknowledge);
    int rearmPin(unsigned *);
    static void interrupt(OSObject *, void *, IOService *, int);
    static void retryFired(OSObject *, IOTimerEventSource *);
    static IOReturn startGated(OSObject *, void *, void *, void *, void *);
    static IOReturn stopGated(OSObject *, void *, void *, void *, void *);
    static int reserveHook(void *, const dre_tn_profile *, void **, unsigned *);
    static int leaseHook(void *, void *);
    static int clockHook(void *, void *, dre_tn_clock *);
    static int allocateHook(void *, void *, size_t, size_t, dre_tn_memory *);
    static int verifyHook(void *, void *, const dre_tn_memory *, unsigned);
    static int freeHook(void *, void *, dre_tn_memory *);
    static int readHook(void *, void *, uint64_t, uint32_t *);
    static int writeHook(void *, void *, uint64_t, uint32_t);
    static void barrier(void *, enum dre_tn_barrier);
    static uint64_t ticks(void *);
    static uint64_t frequency(void *);
    static void pause(void *, unsigned);
    static int maskGPI(void *, void *);
    static int releaseHook(void *, void *);
    static int hash(void *, const uint8_t *, size_t, uint8_t [32]);
    static int maskGPIO(void *, void *);
    static int queueWork(void *, unsigned);
    static int sampleGPIO(void *, void *, unsigned *);
    static int rearmGPIO(void *, void *, unsigned *);
    static uint64_t absolute(void *);
    static enum dre_tn_result submit(void *, const dre_tn_hid_frame *);
    static int quiesce(void *, void *);
    static int domainGuard(void *, const dre_tn_smmu_reservation *, int);
    static int domainNC(void *, const void *, uint64_t, size_t);
    static int domainPage(void *, uint64_t);
    static int smmuRead32(void *, uint64_t, uint32_t *);
    static int smmuRead64(void *, uint64_t, uint64_t *);
    static int smmuWrite32(void *, uint64_t, uint32_t);
    static int smmuWrite64(void *, uint64_t, uint64_t);
    static int qupGuard(void *);
    static int qupRead(void *, uint64_t, uint32_t *);
};
OSDefineMetaClassAndStructors(DRENativeTouch, IOService);
OSDefineMetaClassAndStructors(DRETouchWork, IOEventSource);

bool DRETouchWork::initTouch(DRENativeTouch *t)
{
    touch = t; pending = 0;
    return t && IOEventSource::init(t);
}
void DRETouchWork::post()
{
    __atomic_store_n(&pending, 1, __ATOMIC_RELEASE);
    signalWorkAvailable(); /* Supported from primary interrupt context. */
}
void DRETouchWork::discard()
{
    disable();
    __atomic_store_n(&pending, 0, __ATOMIC_RELEASE);
}
bool DRETouchWork::checkForWork()
{
    if (isEnabled() && __atomic_exchange_n(&pending, 0, __ATOMIC_ACQ_REL)) touch->runWorker();
    /* Preserve a post which raced this check or was queued by the worker.
     * Budget exhaustion uses the timer, so this is not an unbounded SPI loop. */
    return isEnabled() && __atomic_load_n(&pending, __ATOMIC_ACQUIRE);
}
static bool callbacks(const dre_touch_xnu_provider *p)
{
    return p && p->version == DRE_TOUCH_XNU_PROVIDER_VERSION &&
        p->struct_bytes == sizeof(*p) && p->acquire && p->check_lease &&
        p->check_irq_lease_fast && p->clock_reference && p->check_observed_clock && p->mask_gpi_geni_sync &&
        p->gpio_irq_nub && p->physical_aliases_nc && p->reserve_domain &&
        p->check_domain && p->release_domain && p->release;
}
static bool board(const void *dt, size_t bytes)
{
    if (!dt || bytes < 40 || bytes > (2u << 20) || fdt_check_full(dt, bytes) ||
        fdt_totalsize(dt) != bytes || fdt_node_check_compatible(dt, 0, "qcom,holi")) return false;
    int n = 0;
    const fdt32_t *ids = (const fdt32_t *)fdt_getprop(dt, 0, "oplus,dtsi_no", &n);
    if (!ids || n <= 0 || n % 4) return false;
    for (int i = 0; i != n/4; ++i)
        if (fdt32_to_cpu(ids[i]) == 133144 || fdt32_to_cpu(ids[i]) == 133180) return true;
    return false;
}
bool DRENativeTouch::initNative(const dre_touch_xnu_provider *p, IOService *service)
{
    native = {}; profile = {}; spi = {}; controller = {}; input = {};
    smmuProfile = {}; smmuReservation = {}; smmu = {};
    qupClaim = {}; qupSnapshot = {}; qupClock = {};
    gpioRoute = {};
    gpioPin = NULL;
    fdt = nativeLease = domainLease = NULL; fdtBytes = 0; hid = gpioNub = NULL;
    gpioSource = gpii = allocationRole = 0; bzero(maps, sizeof(maps));
    bzero(smmuMaps,sizeof(smmuMaps)); allocations = tableAllocation = NULL;
    loop = NULL; work = NULL; retry = NULL;
    closing = irqFault = dispatchEnabled = peripheralMutated = 0;
    irqRegistered = hold = attached = inputStarted = started = false;
    result = DRE_TN_PROVIDER;
    if (!IOService::init()) return false;
    const void *bootFDT = NULL;
    size_t bootBytes = 0;
    if (!callbacks(p) || !service || !service->metaCast("SweetHIDTouch") ||
        service->isInactive() || dre_native_board_snapshot(&bootFDT, &bootBytes) ||
        !board(bootFDT, bootBytes) ||
        (p->fdt && (p->fdt_bytes != bootBytes || memcmp(p->fdt, bootFDT, bootBytes))) ||
        (!p->fdt && p->fdt_bytes)) return false;
    native = *p;
    fdtBytes = bootBytes;
    fdt = IOMalloc(fdtBytes);
    if (!fdt) return false;
    memcpy(fdt, bootFDT, fdtBytes);
    if (!board(fdt, fdtBytes) || dre_tn_profile_from_fdt(fdt, fdtBytes, &profile) ||
        dre_tn_smmu_profile_from_fdt(fdt,fdtBytes,&smmuProfile) ||
        dre_tn_qup_profile_from_fdt(fdt,fdtBytes)) return false;
    native.fdt = fdt; /* Do not retain a mutable caller's borrowed FDT pointer. */
    native.fdt_bytes = fdtBytes;
    hid = service; hid->retain();
    loop = IOWorkLoop::workLoop();
    work = new DRETouchWork;
    retry = IOTimerEventSource::timerEventSource(this, retryFired);
    if (!loop || !work || !work->initTouch(this) || !retry ||
        loop->addEventSource(work) != kIOReturnSuccess ||
        loop->addEventSource(retry) != kIOReturnSuccess) return false;
    work->enable();
    return true;
}
bool DRENativeTouch::owns()
{
    if (!nativeLease || native.check_lease(native.cookie,nativeLease)) return false;
    if (!qupClaim.generation) return true; /* Pre-mutation platform acquisition. */
    lck_mtx_lock(&dre_touch_qup_lock);
    bool owned=!dre_tn_qup_claim_live(&dre_touch_qup_registry,&qupClaim);
    lck_mtx_unlock(&dre_touch_qup_lock);
    return owned;
}
bool DRENativeTouch::mapRegisters()
{
    /* Exact small device apertures. Pins21/22 share one real16KiB CPU page,
     * so map that page once instead of making competing IO aliases. The GCC
     * and wrapper maps are read-only through readObserved's exact allowlist. */
    const uint64_t pa[REGISTER_MAPS] = {
        0x04a00000,0x04a80000,0x00514000,0x01400000,0x04ac0000,0x00500000 };
    const uint64_t bytes[REGISTER_MAPS] = {0x60000,0x4000,0x4000,0xc8000,0x2000,0x4000};
    if (!owns()) return false;
    for (unsigned i = 0; i != REGISTER_MAPS; ++i) {
        maps[i].pa = pa[i]; maps[i].bytes = bytes[i];
        maps[i].cpu = ml_io_map_unmappable(pa[i], bytes[i], VM_WIMG_IO);
        if (!maps[i].cpu) return false;
    }
    return true;
}
void DRENativeTouch::unmapRegisters()
{
    for (unsigned i = 0; i != REGISTER_MAPS; ++i) {
        if (maps[i].cpu) ml_io_unmap(trunc_page(maps[i].cpu),
            round_page(maps[i].bytes + (maps[i].cpu & PAGE_MASK)));
        maps[i] = {};
    }
}
bool DRENativeTouch::readRegister(uint64_t address, uint32_t *out)
{
    if (!out || (address & 3) || !owns()) return false;
    bool permitted = address >= profile.gpi_ee_base + 0x20000 + gpii * 0x4000 &&
        address < profile.gpi_ee_base + 0x24000 + gpii * 0x4000;
    const unsigned offsets[] = {0x40,0x48,0x64,0x68,0x7c,0x610};
    for (unsigned i = 0; i != sizeof(offsets)/sizeof(offsets[0]); ++i)
        if (address == profile.spi_base + offsets[i]) permitted = true;
    if (!permitted) return false;
    for (unsigned i = 0; i != 2; ++i) {
        if (!maps[i].cpu || address < maps[i].pa || address-maps[i].pa > maps[i].bytes-4) continue;
        *out = *(volatile uint32_t *)(maps[i].cpu + address-maps[i].pa);
        __asm__ volatile("dmb oshld" ::: "memory");
        return true;
    }
    return false;
}
bool DRENativeTouch::readObserved(uint64_t address,uint32_t *out)
{
    if (!out || !owns() || !dre_tn_qup_read_allowed(address)) return false;
    for (unsigned i=0;i<REGISTER_MAPS;i++) {
        if (!maps[i].cpu || address<maps[i].pa || address-maps[i].pa>maps[i].bytes-4) continue;
        *out=*(volatile uint32_t *)(maps[i].cpu+address-maps[i].pa);
        __asm__ volatile("dmb oshld" ::: "memory");
        return true;
    }
    return false;
}
int DRENativeTouch::qupGuard(void *c)
{ return ((DRENativeTouch *)c)->owns()?DRE_TN_OK:DRE_TN_OWNERSHIP; }
int DRENativeTouch::qupRead(void *c,uint64_t a,uint32_t *v)
{ return ((DRENativeTouch *)c)->readObserved(a,v)?DRE_TN_OK:DRE_TN_MMIO; }
void DRENativeTouch::publishClockEvidence()
{
    /* Fixed hardware-register observations only: no firmware, FDT or device
     * identity. Exposing refusal evidence does not advertise ready hardware. */
    setProperty("TouchClockReason",(uint64_t)qupClock.reason,32);
    setProperty("TouchQUPReadback",(void *)&qupSnapshot,(unsigned)sizeof(qupSnapshot));
    /* Keep structure padding out of IORegistry data. */
    setProperty("TouchQUPPLLHz",qupClock.pll_hz,64);
    setProperty("TouchQUPParentHz",qupClock.parent_hz,64);
    setProperty("TouchQUPSEHz",qupClock.serial.parent_hz,64);
    setProperty("TouchQUPSCKHz",qupClock.serial_hz,64);
}
int DRENativeTouch::snapshotQUP()
{
    dre_tn_qup_io io={this,qupGuard,qupRead,barrier};
    int r=dre_tn_qup_observe(&io,&qupSnapshot);
    if (r) {
        qupClock={};
        qupClock.reason=r==DRE_TN_OWNERSHIP?DRE_TN_QUP_CHANGED:DRE_TN_QUP_ACCESS;
        publishClockEvidence();
    }
    return r;
}
int DRENativeTouch::observeClock()
{
    int r=snapshotQUP();
    if (r) return r;
    uint64_t xo=0;
    if (native.clock_reference(native.cookie,nativeLease,&xo)) {
        qupClock={}; qupClock.reason=DRE_TN_QUP_PARENT;
        publishClockEvidence(); return DRE_TN_PROVIDER;
    }
    r=dre_tn_qup_clock_decode(&qupSnapshot,xo,&qupClock);
    if (!r && native.check_observed_clock(native.cookie,nativeLease,&qupSnapshot,&qupClock)) {
        qupClock.reason=DRE_TN_QUP_PARENT;
        r=DRE_TN_PROVIDER;
    }
    publishClockEvidence();
    return r;
}
int DRENativeTouch::claimQUP()
{
    if (qupClaim.generation || !owns() || gpii>4) return DRE_TN_OWNERSHIP;
    int r=observeClock();
    if (r) return r;
    /* Native acquire already owns a global exclusion lease for this exact
     * pair and SPI/pin/IRQ set. Independently refuse live hardware before even
     * masking an owned source. Never choose a different, unleased GPII. */
    if (!(dre_tn_qup_idle_mask(&qupSnapshot)&(1u<<gpii))) {
        qupClock.reason=DRE_TN_QUP_BUSY; publishClockEvidence(); return DRE_TN_OWNERSHIP;
    }
    lck_mtx_lock(&dre_touch_qup_lock);
    r=dre_tn_qup_claim_idle(&dre_touch_qup_registry,(uintptr_t)this,&qupSnapshot,gpii,&qupClaim);
    lck_mtx_unlock(&dre_touch_qup_lock);
    if (r) { qupClock.reason=DRE_TN_QUP_REGISTERED_OWNER; publishClockEvidence(); }
    return r;
}
static bool allowedWrite(unsigned off, uint32_t value)
{
    for (unsigned ch = 0; ch != 2; ++ch) {
        unsigned base = 0x20000 + ch * 0x80;
        const unsigned context[] = {0,4,8,12,0x5c,0x60,0x64,0x68,0x6c};
        for (unsigned i = 0; i != sizeof(context)/sizeof(context[0]); ++i)
            if (off == base + context[i]) return true;
        if (off == 0x22000 + 8*ch || off == 0x22004 + 8*ch) return true;
    }
    const unsigned event[] = {0,4,8,12,0x20,0x28,0x2c,0x30,0x34};
    for (unsigned i = 0; i != sizeof(event)/sizeof(event[0]); ++i)
        if (off == 0x21000 + event[i]) return true;
    if (off == 0x22100 || off == 0x22104) return true;
    if (off == 0x23008) return !(value & 0x00fffffeu) &&
        ((value >> 24) == 0 || (value >> 24) == 1 ||
         (value >> 24) == 2 || (value >> 24) == 10);
    if (off == 0x23010) return value == 0 || value == 0x0a000000;
    switch (off) {
    case 0x23088: return value == 0 || value == 0x4f;
    case 0x23098: case 0x230a0: return !(value & ~3u);
    case 0x2309c: case 0x230a4: case 0x230b8: case 0x230c0:
    case 0x23108: case 0x23180: return value <= 1;
    case 0x23120: return value == 0 || value == 15;
    case 0x23188: case 0x2318c: case 0x23400: case 0x23404: return value == 0;
    default: return false;
    }
}
bool DRENativeTouch::writeRegister(uint64_t address, uint32_t value)
{
    uint64_t base = profile.gpi_ee_base + gpii * 0x4000;
    if ((address & 3) || address < base || address-base > 0x23fff ||
        !allowedWrite((unsigned)(address-base), value) || !maps[0].cpu || !qupClaim.generation || !owns() ||
        address < maps[0].pa || address-maps[0].pa > maps[0].bytes-4) return false;
    __atomic_store_n(&peripheralMutated,1,__ATOMIC_RELEASE);
    *(volatile uint32_t *)(maps[0].cpu + address-maps[0].pa) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return true;
}
int DRENativeTouch::maskPin(bool acknowledge)
{
    if (!gpioPin || !nativeLease || !qupClaim.generation ||
        native.check_irq_lease_fast(native.cookie,nativeLease)) return DRE_TN_OWNERSHIP;
    __atomic_store_n(&peripheralMutated,1,__ATOMIC_RELEASE);
    /* The actual TLMM controller and adapter use this ONE pin engine and
     * IOSimpleLock. No independent controller may race a second lock. */
    return dre_tn_tlmm_mask(gpioPin,acknowledge);
}
int DRENativeTouch::rearmPin(unsigned *again)
{
    if (!again || __atomic_load_n(&closing,__ATOMIC_ACQUIRE)) return DRE_TN_STATE;
    if (!gpioPin || !nativeLease || native.check_irq_lease_fast(native.cookie,nativeLease))
        return DRE_TN_OWNERSHIP;
    __atomic_store_n(&peripheralMutated,1,__ATOMIC_RELEASE);
    return dre_tn_tlmm_arm(gpioPin,again);
}
void DRENativeTouch::sourceFault()
{
    __atomic_store_n(&irqFault, 1, __ATOMIC_RELEASE);
    /* Fault delivery must reach the serialized contact-cancellation path.
     * An invalid/foreign source authorizes no pin acknowledge or register
     * access. The retained owner's coalescing event is memory-only and is
     * explicitly supported from primary interrupt context. */
    if (__atomic_load_n(&dispatchEnabled, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&closing, __ATOMIC_ACQUIRE) && work) work->post();
}
void DRENativeTouch::interrupt(OSObject *target, void *, IOService *nub, int source)
{
    DRENativeTouch *self = (DRENativeTouch *)target;
    if (!self->gpioRoute.fastMatches(nub,source)) {
        /* A foreign/stale source is not permission to acknowledge GPIO22. */
        self->sourceFault();
        return;
    }
    /* Provider is a logical GPIO22 nub. The real interrupt controller handles
     * cascade/GIC EOI; this callback never writes GIC registers or another pin. */
    if (self->maskPin(true)) {
        self->sourceFault();
        return;
    }
    if (__atomic_load_n(&self->closing, __ATOMIC_ACQUIRE)) return;
    if (dre_tn_input_irq(&self->input)) self->sourceFault();
}
int DRENativeTouch::reserve(const dre_tn_profile *p, void **lease, unsigned *selected)
{
    if (!p || !lease || !selected || nativeLease) return DRE_TN_STATE;
    int r = native.acquire(native.cookie, p, &nativeLease, &gpii);
    *lease = nativeLease; *selected = gpii;
    if (nativeLease && !hold) { retain(); hold = true; } /* Unknown owners stay resident. */
    if (r || !owns() || gpii > 4) return r ? r : DRE_TN_OWNERSHIP;
    if (!mapRegisters()) return DRE_TN_MMIO;
    r=claimQUP();
    if (r) return r; /* No pin/GPI/SMMU writes precede this qualification. */
    void *nub = NULL;
    r = native.gpio_irq_nub(native.cookie, nativeLease, &nub, &gpioSource);
    if (r || !nub || gpioSource > 255) return DRE_TN_PROVIDER;
    gpioNub = OSDynamicCast(IOService, (OSObject *)nub);
    if (!gpioNub || gpioNub->isInactive()) { gpioNub = NULL; return DRE_TN_PROVIDER; }
    gpioNub->retain();
    r=gpioRoute.capture(gpioNub,gpioSource,getPlatform(),this,qupGuard);
    if (r) {
        setProperty("TouchIRQRoute","MISSING_OR_UNQUALIFIED_UNVALIDATED");
        return r; /* No adapter GPIO/GPI/SMMU write precedes this capture. */
    }
    r=dre_touch_tlmm_pin_for_nub(gpioNub,gpioSource,&gpioPin);
    if (r || !gpioPin) return DRE_TN_PROVIDER;
    if (maskPin(false)) return DRE_TN_OWNERSHIP;
    r=gpioRoute.registerOwned(this,interrupt,NULL);
    if (r) return r; /* An ambiguous registration retains every actual owner. */
    irqRegistered = true;
    if (gpioRoute.disableJoin()) return DRE_TN_OWNERSHIP;
    setProperty("TouchIRQRoute","CAPTURED_NOT_PHYSICALLY_VALIDATED");
    if (native.mask_gpi_geni_sync(native.cookie,nativeLease,gpii)) return DRE_TN_OWNERSHIP;
    return openDomain();
}

/* Inspect an actual EL1 translation instead of treating a cache option as
 * proof. Interrupts stay disabled across AT/PAR so an intervening handler or
 * context migration cannot overwrite this PE's result. Global alias exclusion
 * remains a separate mandatory native pmap/allocator ownership operation. */
static bool cpuNormalNC(uintptr_t va, uint64_t pa)
{
    uint64_t read, write;
    boolean_t interrupts = ml_set_interrupts_enabled(false);
    __asm__ volatile("at s1e1r, %2; isb; mrs %0, par_el1; at s1e1w, %2; isb; mrs %1, par_el1"
        : "=&r"(read), "=&r"(write) : "r"(va) : "memory");
    ml_set_interrupts_enabled(interrupts);
    const uint64_t mask = UINT64_C(0x000000fffffff000);
    return !(read & 1) && !(write & 1) && (read >> 56) == 0x44 &&
        (write >> 56) == 0x44 && (read & mask) == (pa & mask) &&
        (write & mask) == (pa & mask);
}
static bool sameReservation(const dre_tn_smmu_reservation *a,const dre_tn_smmu_reservation *b)
{
    return a && b && a->cb==b->cb && a->smr==b->smr && a->asid==b->asid &&
        a->fault_irq==b->fault_irq && a->id0==b->id0 && a->id1==b->id1 &&
        a->id2==b->id2 && a->cr0==b->cr0 && a->acr==b->acr &&
        a->tcu_version==b->tcu_version && a->epoch==b->epoch;
}
int DRENativeTouch::domainGuard(void *cookie,const dre_tn_smmu_reservation *r,int quiet)
{
    DRENativeTouch *s=(DRENativeTouch *)cookie;
    if (!s->owns() || !s->domainLease || !sameReservation(r,&s->smmuReservation) ||
        s->native.check_domain(s->native.cookie,s->nativeLease,s->domainLease,r,quiet))
        return DRE_TN_OWNERSHIP;
    if (quiet) {
        if (s->spi.state==DRE_TN_QUARANTINED || s->spi.state==DRE_TN_INFLIGHT ||
            s->spi.allocated_channels || s->spi.event_allocated) return DRE_TN_OWNERSHIP;
        uint32_t tx,rx,geni;
        uint64_t context=s->profile.gpi_ee_base+0x20000+s->gpii*0x4000;
        if (!s->readRegister(context,&tx) || !s->readRegister(context+0x80,&rx) ||
            !s->readRegister(s->profile.spi_base+0x40,&geni)) return DRE_TN_MMIO;
        tx=(tx>>20)&15; rx=(rx>>20)&15;
        /* DEALLOC ACK may leave the preceding STOP state in hardware. The
         * C engine's control ACK/retirement must also have cleared its owners.
         * The native policy above additionally retires every other SID0x16
         * producer and the shared TBU's outstanding references. */
        if ((tx && tx!=3) || (rx && rx!=3) || (geni&0x1001)) return DRE_TN_OWNERSHIP;
    }
    return DRE_TN_OK;
}
int DRENativeTouch::domainNC(void *cookie,const void *cpu,uint64_t pa,size_t bytes)
{
    DRENativeTouch *s=(DRENativeTouch *)cookie;
    DRETouchAllocation *a=s->tableAllocation;
    if (!a || !a->prepared || !a->view.cpu || bytes!=DMA_PAGE || !cpu) return DRE_TN_DMA;
    uintptr_t base=(uintptr_t)a->view.cpu,va=(uintptr_t)cpu;
    if (va<base || va-base>=3*DMA_PAGE || ((va-base)&(DMA_PAGE-1))) return DRE_TN_DMA;
    unsigned index=(unsigned)((va-base)/DMA_PAGE);
    IOByteCount n=0;
    if (pa!=a->pages[index].pa ||
        a->descriptor->getPhysicalSegment(index*DMA_PAGE,&n,kIOMemoryMapperNone)!=pa ||
        n<DMA_PAGE || kvtophys(va)!=pa || !cpuNormalNC(va,pa) ||
        !dre_dma_ram_allowed(s->fdt,pa,DMA_PAGE) ||
        s->native.physical_aliases_nc(s->native.cookie,s->nativeLease,
            (const void *)a->view.cpu,CPU_PAGE,a->pages,PAGES_PER_BLOCK)) return DRE_TN_DMA;
    return DRE_TN_OK;
}
int DRENativeTouch::domainPage(void *cookie,uint64_t pa)
{
    DRENativeTouch *s=(DRENativeTouch *)cookie;
    for (DRETouchAllocation *a=s->allocations;a;a=a->next) {
        if (!a->prepared || !a->view.cpu || !a->descriptor) continue;
        for (unsigned i=0;i<PAGES_PER_BLOCK;i++) if (a->pages[i].pa==pa) {
            uintptr_t va=(uintptr_t)a->view.cpu+i*DMA_PAGE;
            IOByteCount n=0;
            if (a->descriptor->getPhysicalSegment(i*DMA_PAGE,&n,kIOMemoryMapperNone)!=pa ||
                n<DMA_PAGE || kvtophys(va)!=pa || !cpuNormalNC(va,pa) ||
                !dre_dma_ram_allowed(s->fdt,pa,DMA_PAGE) ||
                s->native.physical_aliases_nc(s->native.cookie,s->nativeLease,
                    (const void *)a->view.cpu,CPU_PAGE,a->pages,PAGES_PER_BLOCK)) return DRE_TN_DMA;
            return DRE_TN_OK;
        }
    }
    return DRE_TN_DMA;
}
bool DRENativeTouch::readSMMU(uint64_t address,void *out,unsigned bytes)
{
    if (!out || !domainLease || (bytes!=4 && bytes!=8) || (address&(bytes-1))) return false;
    for (unsigned i=0;i<2;i++) {
        const DRETouchMap *m=smmuMaps+i;
        if (!m->cpu || address<m->pa || bytes>m->bytes || address-m->pa>m->bytes-bytes) continue;
        if (bytes==4) *(uint32_t *)out=*(volatile uint32_t *)(m->cpu+address-m->pa);
        else *(uint64_t *)out=*(volatile uint64_t *)(m->cpu+address-m->pa);
        __asm__ volatile("dmb oshld" ::: "memory");
        return true;
    }
    return false;
}
bool DRENativeTouch::writeSMMU(uint64_t address,uint64_t value,unsigned bytes)
{
    if (!domainLease || !smmu.cb_base || (bytes!=4 && bytes!=8) || (address&(bytes-1))) return false;
    uint64_t base=smmuProfile.base;
    bool allowed=false;
    if (bytes==4) {
        if (address==base+0x800+smmuReservation.smr*4 || address==base+0xc00+smmuReservation.smr*4 ||
            address==base+0x1000+smmuReservation.cb*4 || address==base+0x1800+smmuReservation.cb*4)
            allowed=true;
        const unsigned cb[]={0,4,0x10,0x30,0x34,0x38,0x3c,0x610,0x7f0};
        for (unsigned i=0;i<sizeof(cb)/sizeof(cb[0]);i++)
            if (address==smmu.cb_base+cb[i]) allowed=true;
    } else if (address==smmu.cb_base+0x20 || address==smmu.cb_base+0x28) allowed=true;
    /* This allowlist intentionally contains NO global CR0/ACR/reset/TLBI,
     * peer CB/stream, fault clear, RESUME, TBU/TCU debug or secure register. */
    const DRETouchMap *m=smmuMaps;
    if (!allowed || !m->cpu || address<m->pa || address-m->pa>m->bytes-bytes) return false;
    __atomic_store_n(&peripheralMutated,1,__ATOMIC_RELEASE);
    if (bytes==4) *(volatile uint32_t *)(m->cpu+address-m->pa)=(uint32_t)value;
    else *(volatile uint64_t *)(m->cpu+address-m->pa)=value;
    __asm__ volatile("dsb sy" ::: "memory");
    return true;
}
int DRENativeTouch::smmuRead32(void *c,uint64_t a,uint32_t *v)
{ return ((DRENativeTouch *)c)->readSMMU(a,v,4)?0:DRE_TN_MMIO; }
int DRENativeTouch::smmuRead64(void *c,uint64_t a,uint64_t *v)
{ return ((DRENativeTouch *)c)->readSMMU(a,v,8)?0:DRE_TN_MMIO; }
int DRENativeTouch::smmuWrite32(void *c,uint64_t a,uint32_t v)
{ return ((DRENativeTouch *)c)->writeSMMU(a,v,4)?0:DRE_TN_MMIO; }
int DRENativeTouch::smmuWrite64(void *c,uint64_t a,uint64_t v)
{ return ((DRENativeTouch *)c)->writeSMMU(a,v,8)?0:DRE_TN_MMIO; }
int DRENativeTouch::openDomain()
{
    int r=native.reserve_domain(native.cookie,nativeLease,&smmuProfile,&domainLease,&smmuReservation);
    if (r || !domainLease) return r?r:DRE_TN_OWNERSHIP;
    const uint64_t pa[2]={smmuProfile.base,smmuProfile.tcu};
    const uint64_t bytes[2]={smmuProfile.bytes,0x20};
    for (unsigned i=0;i<2;i++) {
        smmuMaps[i].pa=pa[i]; smmuMaps[i].bytes=bytes[i];
        smmuMaps[i].cpu=ml_io_map_unmappable(pa[i],bytes[i],VM_WIMG_IO);
        if (!smmuMaps[i].cpu) return DRE_TN_MMIO;
    }
    tableAllocation=(DRETouchAllocation *)IOMallocZero(sizeof(*tableAllocation));
    if (!tableAllocation) return DRE_TN_DMA;
    tableAllocation->role=DRE_TN_BLOCKS;
    tableAllocation->view.bytes=CPU_PAGE;
    tableAllocation->view.allocation=tableAllocation;
    r=prepareBacking(tableAllocation);
    if (r) return r;
    bzero((void *)tableAllocation->view.cpu,CPU_PAGE);
    dre_tn_smmu_tables tables={};
    for (unsigned i=0;i<DRE_TN_SMMU_TABLES;i++) {
        tables.cpu[i]=(volatile uint64_t *)(tableAllocation->view.cpu+i*DMA_PAGE);
        tables.pa[i]=tableAllocation->pages[i].pa;
    }
    dre_tn_smmu_ops ops={this,domainGuard,domainNC,domainPage,smmuRead32,smmuRead64,
        smmuWrite32,smmuWrite64,barrier,ticks,frequency,pause};
    return dre_tn_smmu_begin(&smmu,&smmuProfile,&smmuReservation,&tables,&ops);
}
int DRENativeTouch::closeDomain()
{
    if (allocations) return DRE_TN_STATE;
    int r=dre_tn_smmu_end(&smmu);
    if (r) return r;
    if (tableAllocation) {
        if (tableAllocation->prepared) {
            if (tableAllocation->descriptor->complete(kIODirectionInOut)!=kIOReturnSuccess) return DRE_TN_DMA;
            tableAllocation->prepared=false;
        }
        OSSafeReleaseNULL(tableAllocation->descriptor);
        IOFree(tableAllocation,sizeof(*tableAllocation)); tableAllocation=NULL;
    }
    if (domainLease) {
        if (native.release_domain(native.cookie,nativeLease,domainLease)) return DRE_TN_OWNERSHIP;
        domainLease=NULL;
    }
    for (unsigned i=0;i<2;i++) {
        if (smmuMaps[i].cpu) ml_io_unmap(trunc_page(smmuMaps[i].cpu),
            round_page(smmuMaps[i].bytes+(smmuMaps[i].cpu&PAGE_MASK)));
        smmuMaps[i]={};
    }
    return DRE_TN_OK;
}
int DRENativeTouch::prepareBacking(DRETouchAllocation *a)
{
    if (!a || !owns() || page_size != CPU_PAGE) return DRE_TN_ARGUMENT;
    /* One wired16KiB owner per logical block, never a shared suballocator.
     * The transport uses its original1024/4096-byte logical span, while the
     * SMMU and CPU proof cover every4KiB page of the full descriptor. */
    a->descriptor = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,
        CPU_PAGE, CPU_PAGE);
    if (!a->descriptor || a->descriptor->prepare(kIODirectionInOut) != kIOReturnSuccess)
        return DRE_TN_DMA;
    a->prepared = true;
    a->view.cpu = (volatile uint8_t *)a->descriptor->getBytesNoCopy();
    if (!a->view.cpu || ((uintptr_t)a->view.cpu & (CPU_PAGE-1))) return DRE_TN_DMA;
    for (unsigned i = 0; i != PAGES_PER_BLOCK; ++i) {
        IOByteCount length = 0;
        uintptr_t va = (uintptr_t)a->view.cpu + i*DMA_PAGE;
        uint64_t pa = a->descriptor->getPhysicalSegment(i*DMA_PAGE, &length, kIOMemoryMapperNone);
        if (!pa || (pa & (DMA_PAGE-1)) || pa >= (UINT64_C(1) << 40) || length < DMA_PAGE ||
            kvtophys(va) != pa || !dre_dma_ram_allowed(fdt, pa, DMA_PAGE) || !cpuNormalNC(va, pa))
            return DRE_TN_DMA;
        a->pages[i].pa = pa;
        for (unsigned j=0;j<i;j++) if (a->pages[j].pa==pa) return DRE_TN_DMA;
        for (DRETouchAllocation *b = allocations; b; b = b->next)
            for (unsigned j = 0; j != PAGES_PER_BLOCK; ++j)
                if ((b != a || j < i) && b->pages[j].pa == pa) return DRE_TN_DMA;
        if (tableAllocation && tableAllocation!=a)
            for (unsigned j=0;j<PAGES_PER_BLOCK;j++)
                if (tableAllocation->pages[j].pa==pa) return DRE_TN_DMA;
    }
    if (native.physical_aliases_nc(native.cookie, nativeLease, (const void *)a->view.cpu,
            CPU_PAGE, a->pages, PAGES_PER_BLOCK)) return DRE_TN_DMA;
    return DRE_TN_OK;
}
int DRENativeTouch::allocate(size_t bytes, size_t align, dre_tn_memory *out)
{
    if (!out || !owns() || !smmu.active || smmu.unsafe || !bytes || bytes>DMA_PAGE ||
        !align || (align&(align-1)) || align>DMA_PAGE || allocationRole>=DRE_TN_BLOCKS)
        return DRE_TN_ARGUMENT;
    *out={};
    DRETouchAllocation *a=(DRETouchAllocation *)IOMallocZero(sizeof(*a));
    if (!a) return DRE_TN_DMA;
    a->role=allocationRole++;
    a->permissions=a->role==DRE_TN_EVENT_RING || a->role==DRE_TN_RX_BYTES ?
        DRE_TOUCH_DEVICE_READ|DRE_TOUCH_DEVICE_WRITE : DRE_TOUCH_DEVICE_READ;
    a->view.bytes=bytes; a->view.allocation=a;
    a->next=allocations; allocations=a;
    int r=prepareBacking(a);
    *out=a->view;
    if (r) return r;
    r=dre_tn_smmu_map(&smmu,a->pages,PAGES_PER_BLOCK,a->permissions,&a->mapping);
    a->view.iova=a->mapping.iova;
    *out = a->view; /* Preserve the real component's partial publication token. */
    if (r || !a->mapping.generation || a->view.iova < profile.iova_base ||
        a->view.iova >= profile.iova_base + profile.iova_size ||
        CPU_PAGE > profile.iova_base + profile.iova_size-a->view.iova ||
        (a->view.iova & (CPU_PAGE-1))) return DRE_TN_DMA;
    for (DRETouchAllocation *b = allocations; b; b = b->next)
        if (b != a && b->mapping.generation && a->view.iova < b->view.iova + CPU_PAGE &&
            b->view.iova < a->view.iova + CPU_PAGE) return DRE_TN_DMA;
    if (verify(out, profile.stream_id)) return DRE_TN_DMA;
    bzero((void *)a->view.cpu, CPU_PAGE);
    barrier(this, DRE_TN_PUBLISH);
    return DRE_TN_OK;
}
int DRENativeTouch::verify(const dre_tn_memory *view, unsigned stream)
{
    if (!view || !owns() || stream != 0x16) return DRE_TN_DMA;
    DRETouchAllocation *a = allocations;
    while (a && a != view->allocation) a = a->next;
    if (!a || !a->prepared || !a->descriptor || !a->mapping.generation ||
        a->view.cpu != view->cpu || a->view.iova != view->iova || a->view.bytes != view->bytes)
        return DRE_TN_DMA;
    for (unsigned i = 0; i != PAGES_PER_BLOCK; ++i) {
        uintptr_t va = (uintptr_t)view->cpu + i*DMA_PAGE;
        IOByteCount length = 0;
        uint64_t pa = a->descriptor->getPhysicalSegment(i*DMA_PAGE, &length, kIOMemoryMapperNone);
        if (pa != a->pages[i].pa || length < DMA_PAGE || kvtophys(va) != pa || !cpuNormalNC(va, pa))
            return DRE_TN_DMA;
    }
    if (native.physical_aliases_nc(native.cookie, nativeLease, (const void *)a->view.cpu,
            CPU_PAGE, a->pages, PAGES_PER_BLOCK) ||
        dre_tn_smmu_walk(&smmu,&a->mapping,a->pages,PAGES_PER_BLOCK,a->permissions)) return DRE_TN_DMA;
    return DRE_TN_OK;
}
int DRENativeTouch::retire(dre_tn_memory *view)
{
    if (!view || !owns() || spi.state == DRE_TN_QUARANTINED || spi.state == DRE_TN_INFLIGHT ||
        spi.allocated_channels || spi.event_allocated) return DRE_TN_POISONED;
    DRETouchAllocation **where = &allocations;
    while (*where && *where != view->allocation) where = &(*where)->next;
    if (!*where) return DRE_TN_DMA;
    DRETouchAllocation *a = *where;
    if (a->mapping.generation) {
        if (dre_tn_smmu_unmap(&smmu,&a->mapping)) return DRE_TN_DMA;
        a->mapping = {};
    }
    if (a->prepared) {
        if (a->descriptor->complete(kIODirectionInOut) != kIOReturnSuccess) return DRE_TN_DMA;
        a->prepared = false;
    }
    OSSafeReleaseNULL(a->descriptor);
    *where = a->next; IOFree(a, sizeof(*a)); *view = {};
    return DRE_TN_OK;
}

int DRENativeTouch::reserveHook(void *c, const dre_tn_profile *p, void **l, unsigned *g)
{ return ((DRENativeTouch *)c)->reserve(p, l, g); }
int DRENativeTouch::leaseHook(void *c, void *l)
{ DRENativeTouch *s = (DRENativeTouch *)c; return l == s->nativeLease && s->owns() ? 0 : DRE_TN_OWNERSHIP; }
int DRENativeTouch::clockHook(void *c, void *l, dre_tn_clock *clock)
{
    DRENativeTouch *s=(DRENativeTouch *)c;
    if (!clock || leaseHook(c,l) || !s->qupClaim.generation) return DRE_TN_OWNERSHIP;
    if (s->gpioRoute.captured && s->gpioRoute.check()) return DRE_TN_OWNERSHIP;
    int r=s->observeClock();
    if (!r) *clock=s->qupClock.serial;
    return r;
}
int DRENativeTouch::allocateHook(void *c, void *l, size_t n, size_t a, dre_tn_memory *m)
{ return leaseHook(c,l) ? DRE_TN_OWNERSHIP : ((DRENativeTouch *)c)->allocate(n,a,m); }
int DRENativeTouch::verifyHook(void *c, void *l, const dre_tn_memory *m, unsigned stream)
{ return leaseHook(c,l) ? DRE_TN_OWNERSHIP : ((DRENativeTouch *)c)->verify(m,stream); }
int DRENativeTouch::freeHook(void *c, void *l, dre_tn_memory *m)
{ return leaseHook(c,l) ? DRE_TN_OWNERSHIP : ((DRENativeTouch *)c)->retire(m); }
int DRENativeTouch::readHook(void *c, void *l, uint64_t a, uint32_t *v)
{ return !leaseHook(c,l) && ((DRENativeTouch *)c)->readRegister(a,v) ? 0 : DRE_TN_MMIO; }
int DRENativeTouch::writeHook(void *c, void *l, uint64_t a, uint32_t v)
{ return !leaseHook(c,l) && ((DRENativeTouch *)c)->writeRegister(a,v) ? 0 : DRE_TN_MMIO; }
void DRENativeTouch::barrier(void *, enum dre_tn_barrier kind)
{
    if (kind == DRE_TN_PUBLISH) __asm__ volatile("dmb oshst" ::: "memory");
    else if (kind == DRE_TN_CONSUME) __asm__ volatile("dmb oshld" ::: "memory");
    else __asm__ volatile("dsb sy" ::: "memory");
}
uint64_t DRENativeTouch::ticks(void *)
{ uint64_t v; __asm__ volatile("isb; mrs %0,cntvct_el0" : "=r"(v) :: "memory"); return v; }
uint64_t DRENativeTouch::frequency(void *)
{ uint64_t v; __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(v)); return v; }
void DRENativeTouch::pause(void *, unsigned us)
{
    if (us >= 1000) IOSleep(us/1000);
    if (us%1000) IODelay(us%1000);
}
int DRENativeTouch::maskGPI(void *c, void *l)
{
    DRENativeTouch *s = (DRENativeTouch *)c;
    if (leaseHook(c,l) || s->native.mask_gpi_geni_sync(s->native.cookie,l,s->gpii))
        return DRE_TN_OWNERSHIP;
    if (s->maskPin(false)) return DRE_TN_OWNERSHIP;
    if (s->irqRegistered && s->gpioRoute.disableJoin())
        return DRE_TN_OWNERSHIP;
    return DRE_TN_OK;
}
int DRENativeTouch::releaseHook(void *c, void *l)
{ return ((DRENativeTouch *)c)->releaseLease(l); }
int DRENativeTouch::hash(void *, const uint8_t *p, size_t n, uint8_t digest[32])
{
    if (!p || !n || !digest) return DRE_TN_ARGUMENT;
    SHA256_CTX context;
    SHA256_Init(&context); SHA256_Update(&context, p, n); SHA256_Final(digest, &context);
    bzero(&context, sizeof(context));
    return DRE_TN_OK;
}
int DRENativeTouch::maskGPIO(void *c, void *l)
{
    DRENativeTouch *s = (DRENativeTouch *)c;
    if (l != s->nativeLease) return DRE_TN_OWNERSHIP;
    return s->maskPin(false);
}
int DRENativeTouch::queueWork(void *c, unsigned ms)
{
    DRENativeTouch *s = (DRENativeTouch *)c;
    if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE) || !s->work || !s->retry) return DRE_TN_STATE;
    if (ms) {
        if (!s->loop->inGate()) return DRE_TN_STATE;
        if (s->retry->setTimeoutMS(ms) != kIOReturnSuccess) return DRE_TN_PROVIDER;
    } else s->work->post();
    return DRE_TN_OK;
}
int DRENativeTouch::sampleGPIO(void *c, void *l, unsigned *level)
{
    DRENativeTouch *s = (DRENativeTouch *)c;
    if (!level || l != s->nativeLease || !s->gpioPin ||
        __atomic_load_n(&s->irqFault, __ATOMIC_ACQUIRE) ||
        s->native.check_irq_lease_fast(s->native.cookie,l)) return DRE_TN_OWNERSHIP;
    return dre_tn_tlmm_sample(s->gpioPin,level);
}
int DRENativeTouch::rearmGPIO(void *c, void *l, unsigned *again)
{
    DRENativeTouch *s = (DRENativeTouch *)c;
    if (l != s->nativeLease || !s->irqRegistered || !s->loop->inGate() ||
        __atomic_load_n(&s->irqFault, __ATOMIC_ACQUIRE)) return DRE_TN_OWNERSHIP;
    int r = s->rearmPin(again);
    if (!r && !*again && s->gpioRoute.enableOwned()) {
        (void)s->maskPin(false);
        return DRE_TN_OWNERSHIP;
    }
    if (!r && !*again) {
        /* Parent-controller enable may itself clear a stale parent pending
         * bit. Recheck the real pin after that operation as well, so an edge
         * between local unmask and parent enable cannot strand a report. */
        if (__atomic_load_n(&s->irqFault, __ATOMIC_ACQUIRE) ||
            !s->gpioPin || s->native.check_irq_lease_fast(s->native.cookie,l) ||
            dre_tn_tlmm_recheck(s->gpioPin,again)) return DRE_TN_OWNERSHIP;
    }
    return r;
}
uint64_t DRENativeTouch::absolute(void *) { return mach_absolute_time(); }
enum dre_tn_result DRENativeTouch::submit(void *c, const dre_tn_hid_frame *frame)
{
    DRENativeTouch *s = (DRENativeTouch *)c;
    if (!frame || !s->loop->inGate() || !s->hid || s->hid->isInactive()) return DRE_TN_INPUT;
    /* A transfer already in progress may finish under its genuine GPI lease.
     * Once IRQ loss is observed here, do not commit its normal report or a
     * held pre-loss report. Explicit cancellation remains deliverable. This
     * check does not synchronously abort DMA or claim hardware retirement. */
    if (frame->cancel_reason == DRE_TN_NOT_CANCELLED &&
        __atomic_load_n(&s->irqFault, __ATOMIC_ACQUIRE)) return DRE_TN_OWNERSHIP;
    return dre_sweet_hid_submit_frame(s->hid, frame) ? DRE_TN_INPUT : DRE_TN_OK;
}
int DRENativeTouch::quiesce(void *c, void *l)
{
    DRENativeTouch *s = (DRENativeTouch *)c;
    if (l != s->nativeLease || !s->loop->inGate()) return DRE_TN_OWNERSHIP;
    /* Native IRQ never takes the work-loop gate. Joining it while gated does
     * not wait on a callback blocked behind this gate. */
    if (s->maskPin(false) || (s->irqRegistered && s->gpioRoute.disableJoin())) return DRE_TN_OWNERSHIP;
    s->retry->cancelTimeout(); s->work->discard();
    return DRE_TN_OK;
}
int DRENativeTouch::releaseLease(void *l)
{
    if (!l || l != nativeLease || allocations || spi.allocated_channels || spi.event_allocated ||
        spi.state==DRE_TN_INFLIGHT || spi.state==DRE_TN_QUARANTINED ||
        (inputStarted && !__atomic_load_n(&input.stopping, __ATOMIC_ACQUIRE))) return DRE_TN_STATE;
    if (gpioRoute.uncertain || (gpioRoute.attempted && !gpioRoute.registered)) return DRE_TN_POISONED;
    /* The concrete C engine clears its allocated flags only after the actual
     * STOP/DEALLOC control acknowledgements. Join the leased GPI/GENI sources
     * again while the real native policy is still held. An unclaimed partial
     * acquisition does not authorize MMIO here; its broker must unwind it. */
    if (qupClaim.generation && !owns()) return DRE_TN_OWNERSHIP;
    if (qupClaim.generation && __atomic_load_n(&peripheralMutated,__ATOMIC_ACQUIRE) &&
        native.mask_gpi_geni_sync(native.cookie,l,gpii)) return DRE_TN_OWNERSHIP;
    /* IOInterruptController's generic disable is soft-disable/join only.
     * Mask the actual owned TLMM pin first, before retiring its DMA domain. */
    if (irqRegistered && (maskPin(false) || gpioRoute.disableJoin())) return DRE_TN_OWNERSHIP;
    int domainResult=closeDomain();
    if (domainResult) return domainResult;
    if (irqRegistered) {
        if (gpioRoute.unregisterOwned()) return DRE_TN_OWNERSHIP;
        irqRegistered = false;
    }
    if (qupClaim.generation) {
        int r=snapshotQUP();
        if (r) return r;
        lck_mtx_lock(&dre_touch_qup_lock);
        r=dre_tn_qup_retirement_check(&dre_touch_qup_registry,&qupClaim,&qupSnapshot);
        lck_mtx_unlock(&dre_touch_qup_lock);
        if (r) return r;
    }
    if (native.release(native.cookie, l)) return DRE_TN_OWNERSHIP;
    nativeLease = NULL;
    gpioPin = NULL; /* Borrowed controller binding has completed actual teardown. */
    /* No hardware access after the platform drops its rails/source/IRQ lease.
     * The local token is still exclusive and gated; release from the exact
     * preceding retirement evidence only after the real broker succeeded. */
    if (qupClaim.generation) {
        lck_mtx_lock(&dre_touch_qup_lock);
        int r=dre_tn_qup_release_retired(&dre_touch_qup_registry,&qupClaim,&qupSnapshot);
        if (!r) qupClaim={};
        lck_mtx_unlock(&dre_touch_qup_lock);
        if (r) return DRE_TN_POISONED; /* Keep diagnostic owner/ledger resident. */
    }
    if (gpioRoute.releaseCapture()) return DRE_TN_POISONED;
    OSSafeReleaseNULL(gpioNub);
    unmapRegisters();
    __atomic_store_n(&peripheralMutated,0,__ATOMIC_RELEASE);
    /* A publish/shutdown caller still owns its initial reference here. Drop
     * the separate hardware-lifetime retain only after actual policy release. */
    if (hold) { hold = false; release(); }
    return DRE_TN_OK;
}
void DRENativeTouch::retryFired(OSObject *owner, IOTimerEventSource *)
{
    DRENativeTouch *s = (DRENativeTouch *)owner;
    if (!__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE)) s->work->post();
}
void DRENativeTouch::runWorker()
{
    if (!loop->inGate() || !inputStarted ||
        !__atomic_load_n(&dispatchEnabled, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&closing, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&irqFault, __ATOMIC_ACQUIRE)) {
        result = dre_tn_input_source_lost(&input);
        setProperty("TouchPhase", "IRQ_OWNER_LOST_UNVALIDATED");
        return;
    }
    result = dre_tn_input_worker(&input);
    if (spi.state == DRE_TN_QUARANTINED || controller.state == DRE_TN_CONTROLLER_FAULT)
        setProperty("TouchPhase", "FAULT_HELD_UNVALIDATED");
}
IOReturn DRENativeTouch::startGated(OSObject *owner, void *, void *, void *, void *)
{
    DRENativeTouch *s = (DRENativeTouch *)owner;
    dre_tn_ops ops = { s, reserveHook, leaseHook, clockHook, allocateHook, verifyHook,
        freeHook, readHook, writeHook, barrier, ticks, frequency, pause, maskGPI, releaseHook };
    s->result = dre_tn_spi_open(&s->spi, &s->profile, &ops);
    if (s->result) return kIOReturnNotReady;
    int n = 0;
    int node = fdt_path_offset(s->fdt, "/soc/spi@4a80000/novatek@0");
    const uint8_t *fw = (const uint8_t *)fdt_getprop(s->fdt, node, "firmware-data-0", &n);
    if (!fw || n != (int)s->profile.firmware_size) {
        s->result = DRE_TN_FIRMWARE;
        return kIOReturnNotReady;
    }
    s->result = dre_tn_controller_boot(&s->controller, &s->spi, fw, (size_t)n, hash, s);
    if (s->result) return kIOReturnNotReady;
    dre_tn_input_ops inputOps = { s, maskGPIO, queueWork, sampleGPIO, rearmGPIO,
        absolute, submit, quiesce };
    s->result = dre_tn_input_start(&s->input, &s->controller, &inputOps);
    /* Even a failed partial input_start has a live controller pointer; retain
     * its quiesce/cancel path instead of treating that object as unstarted. */
    s->inputStarted = s->input.controller != NULL;
    if (!s->result) {
        __atomic_store_n(&s->dispatchEnabled, 1, __ATOMIC_RELEASE);
        /* Cover a real primary fault observed while input_start held the
         * work-loop gate but dispatch was not yet enabled. */
        if (__atomic_load_n(&s->irqFault, __ATOMIC_ACQUIRE)) s->work->post();
    }
    return s->result ? kIOReturnNotReady : kIOReturnSuccess;
}
bool DRENativeTouch::start(IOService *provider)
{
    if (page_size != CPU_PAGE || frequency(this) != 19200000 || !IOService::start(provider)) return false;
    setName("DRENativeTouch"); setProperty("UNVALIDATED", true);
    IOReturn r = loop->runAction(startGated, this);
    started = true;
    setProperty("TouchPhase", r == kIOReturnSuccess ? "INPUT_ATTACHED_UNVALIDATED" : "START_REFUSED_OR_HELD_UNVALIDATED");
    /* A retained diagnostic owner on failure is not advertised as an Apple
     * input device; SweetHIDTouch remains the only digitizer service. */
    if (nativeLease || qupClaim.generation) { registerService(); return true; }
    return r == kIOReturnSuccess;
}
IOReturn DRENativeTouch::stopGated(OSObject *owner, void *, void *, void *, void *)
{
    DRENativeTouch *s = (DRENativeTouch *)owner;
    if (s->inputStarted) {
        s->result = dre_tn_input_stop(&s->input);
        if (s->result) return kIOReturnNotReady;
    } else {
        if (s->retry) s->retry->cancelTimeout();
        if (s->work) s->work->discard();
        if (s->irqRegistered && (s->maskPin(false) || s->gpioRoute.disableJoin())) {
            s->result = DRE_TN_OWNERSHIP;
            return kIOReturnNotReady;
        }
    }
    s->result = dre_tn_spi_close(&s->spi);
    return s->result ? kIOReturnNotReady : kIOReturnSuccess;
}
int DRENativeTouch::shutdown()
{
    if (!loop || loop->onThread() || loop->inGate()) return DRE_TN_STATE;
    __atomic_store_n(&closing, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&dispatchEnabled, 0, __ATOMIC_RELEASE);
    if (loop->runAction(stopGated, this) != kIOReturnSuccess) return result;
    if (nativeLease || qupClaim.generation || allocations || irqRegistered || spi.lease || spi.blocks ||
        gpioRoute.attempted || gpioRoute.registered || gpioRoute.uncertain)
        return DRE_TN_POISONED;
    if (started) { IOService::stop(hid); started = false; }
    if (attached) { detach(hid); attached = false; }
    if (hold) { hold = false; release(); } /* Initial publish owner still retained. */
    return DRE_TN_OK;
}
void DRENativeTouch::stop(IOService *provider)
{
    /* Ordinary parent stop is not permission to destroy a live DMA owner.
     * External explicit shutdown performs the qualified retirement sequence. */
    __atomic_store_n(&closing, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&dispatchEnabled, 0, __ATOMIC_RELEASE);
    if (nativeLease) {
        (void)maskPin(false); /* Fast owned-pin mask; do not join this thread. */
        setProperty("TouchPhase", "STOP_HELD_UNVALIDATED");
    }
    IOService::stop(provider);
}
void DRENativeTouch::free()
{
    /* A native lease creates an extra retain, so this branch must not be
     * reachable while hardware or partial mappings can reference the owner. */
    if (nativeLease || qupClaim.generation || domainLease || allocations || tableAllocation ||
        smmu.changed || irqRegistered || gpioRoute.attempted || gpioRoute.registered || gpioRoute.uncertain) return;
    if (retry) retry->cancelTimeout();
    if (loop && work && work->getWorkLoop()) loop->removeEventSource(work);
    if (loop && retry && retry->getWorkLoop()) loop->removeEventSource(retry);
    OSSafeReleaseNULL(retry); OSSafeReleaseNULL(work); OSSafeReleaseNULL(loop);
    (void)gpioRoute.releaseCapture();
    OSSafeReleaseNULL(gpioNub); OSSafeReleaseNULL(hid);
    unmapRegisters();
    if (fdt) { bzero(fdt, fdtBytes); IOFree(fdt, fdtBytes); }
    IOService::free();
}
extern "C" int dre_touch_xnu_publish(const dre_touch_xnu_provider *provider,
                                      void *hidService, void **out)
{
    if (!out || *out || !callbacks(provider) || !hidService) return DRE_TN_PROVIDER;
    IOService *hid = OSDynamicCast(IOService, (OSObject *)hidService);
    if (!hid) return DRE_TN_INPUT;
    DRENativeTouch *s = new DRENativeTouch;
    if (!s) return DRE_TN_DMA;
    if (!s->initNative(provider, hid)) { s->release(); return DRE_TN_PROFILE; }
    if (!s->attach(hid)) { s->release(); return DRE_TN_INPUT; }
    s->attached = true;
    if (!s->start(hid) && !s->nativeLease && !s->qupClaim.generation) {
        enum dre_tn_result result = s->result;
        if (s->started) { s->stop(hid); s->started = false; }
        s->detach(hid); s->attached = false; s->release();
        return result ? result : DRE_TN_PROVIDER;
    }
    *out = s; /* Includes failed partial owners; do not drop this on error. */
    return s->result;
}
extern "C" int dre_touch_xnu_shutdown(void **owner)
{
    if (!owner || !*owner) return DRE_TN_ARGUMENT;
    DRENativeTouch *s = OSDynamicCast(DRENativeTouch, (OSObject *)*owner);
    if (!s) return DRE_TN_ARGUMENT;
    int result = s->shutdown();
    if (!result) { *owner = NULL; s->release(); }
    return result;
}
#endif /* DRE */
