/* SPDX-License-Identifier: MIT -- UNVALIDATED original native source.
 * Actual Device mappings and bounded, owned MMIO for the existing GLINK/RPM
 * protocol. No default policy, voltage request or IRQ/controller takeover.
 * Hardware/source facts are pinned in platform/sources.json. */
#include "rpm_native.h"
#include "rpm_layout.h"
#include "../kernel/dre_gpu_uapi.h"
#include "../../native_board.h"
#include "../../libfdt/libfdt.h"
#include <IOKit/IOLib.h>
#include <machine/machine_routines.h>
#include <machine/memory_types.h>
#include <vm/pmap.h>
#include <vm/memory_types.h>
#include <libkern/libkern.h>
extern "C" {
vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned);
void ml_io_unmap(vm_offset_t,vm_size_t);
uint64_t kvtophys(vm_offset_t);
}
struct dre_rpm_native_mapping { uint64_t pa,bytes;vm_offset_t cpu; };
struct dre_rpm_native {
    dre_rpm_native_broker broker;
    dre_rpm_native_resources resources;
    dre_rpm_native_mapping mappings[2];
    const void *fdt;
    size_t fdt_bytes;
    uint64_t lease,remote_epoch,writes;
    const char *error;
    uint32_t toc_problems;
    bool mapped,unsafe,closing;
};
static int one(const void *f,int node,const char *key,uint32_t want)
{ int n;const fdt32_t *p=(const fdt32_t *)fdt_getprop(f,node,key,&n);return p&&n==4&&fdt32_to_cpu(*p)==want; }
static bool word(const void *f,int node,const char *key,uint32_t *value)
{ int n;const fdt32_t *p=(const fdt32_t *)fdt_getprop(f,node,key,&n);if(!p||n!=4)return false;*value=fdt32_to_cpu(*p);return true; }
static bool string(const void *f,int node,const char *key,const char *want)
{ int n;const char *p=(const char *)fdt_getprop(f,node,key,&n);size_t bytes=strlen(want)+1;return p&&n==(int)bytes&&!memcmp(p,want,bytes); }
static bool absent(const void *f,int node,const char *key)
{ int n;return !fdt_getprop(f,node,key,&n)&&n==-FDT_ERR_NOTFOUND; }
static bool enabled(const void *f,int node)
{
    for(unsigned i=0;node>=0&&i<32;i++) {
        if(!absent(f,node,"status")&&!string(f,node,"status","ok")&&!string(f,node,"status","okay"))return false;
        if(!node)return true;node=fdt_parent_offset(f,node);
    }
    return false;
}
static int unique(const void *f,const char *compatible)
{ int n=fdt_node_offset_by_compatible(f,-1,compatible);return n>=0&&enabled(f,n)&&fdt_node_offset_by_compatible(f,n,compatible)==-FDT_ERR_NOTFOUND?n:-1; }
static int reference(const void *f,int node,const char *key)
{
    uint32_t h;if(!word(f,node,key,&h)||!h||h==UINT32_MAX)return -1;
    int result=-1,at=-1;unsigned count=0;
    while((at=fdt_next_node(f,at,NULL))>=0) {
        if(++count>8192)return -1;
        uint32_t a=0,b=0;bool hasA=!absent(f,at,"phandle"),hasB=!absent(f,at,"linux,phandle");
        if((hasA&&!word(f,at,"phandle",&a))||(hasB&&!word(f,at,"linux,phandle",&b))||
           (hasA&&hasB&&a!=b))return -1;
        if((hasA&&a==h)||(hasB&&b==h)){if(result>=0)return -1;result=at;}
    }
    return at==-FDT_ERR_NOTFOUND&&result>=0&&enabled(f,result)?result:-1;
}
static bool tuple(const void *f,int node,const char *key,const uint32_t *values,unsigned count)
{
    int n;const fdt32_t *p=(const fdt32_t *)fdt_getprop(f,node,key,&n);
    if(!p||n!=(int)count*4)return false;
    for(unsigned i=0;i<count;i++)if(fdt32_to_cpu(p[i])!=values[i])return false;
    return true;
}
static int parent_irq(const void *f,int node)
{
    for(unsigned i=0;node>=0&&i<32;i++) {
        if(!absent(f,node,"interrupt-parent"))return reference(f,node,"interrupt-parent");
        node=fdt_parent_offset(f,node);
    }
    return -1;
}
static bool profile(const void *f,size_t bytes,dre_rpm_native_resources *r)
{
    if(!f||bytes<40||bytes>2u*1024u*1024u||fdt_check_full(f,bytes)||fdt_totalsize(f)!=bytes||
       fdt_node_check_compatible(f,0,"qcom,holi"))return false;
    int soc=fdt_path_offset(f,"/soc"),n;
    const fdt32_t *ranges=(const fdt32_t *)fdt_getprop(f,soc,"ranges",&n);
    if(soc<0||fdt_address_cells(f,0)!=2||fdt_address_cells(f,soc)!=1||fdt_size_cells(f,soc)!=1||
       !ranges||n!=16||ranges[0]||ranges[1]||ranges[2]||fdt32_to_cpu(ranges[3])<0xf400010)return false;
    int ram=unique(f,"qcom,rpm-msg-ram"),glink=unique(f,"qcom,glink-rpm");
    int rpm=unique(f,"qcom,rpm-smd"),ipcc=unique(f,"qcom,ipcc"),gic=unique(f,"arm,gic-v3");
    int mpm=unique(f,"qcom,mpm-gic-holi");
    if(ram<0||glink<0||rpm<0||ipcc<0||gic<0||mpm<0)return false;
    const uint32_t ramReg[]={0x45f0000,0x7000},ipccReg[]={0x208000,0x1000};
    const uint32_t mpmReg[]={0x45f01b8,0x1000,0xf40000c,4};
    const uint32_t receive[]={0,334,4},endpoint[]={0,0,1};
    if(fdt_parent_offset(f,ram)!=soc||fdt_parent_offset(f,ipcc)!=soc||
       fdt_parent_offset(f,mpm)!=soc||!tuple(f,ram,"reg",ramReg,2)||
       !tuple(f,ipcc,"reg",ipccReg,2)||!tuple(f,mpm,"reg",mpmReg,4)||
       reference(f,glink,"qcom,rpm-msg-ram")!=ram||parent_irq(f,glink)!=ipcc||
       parent_irq(f,rpm)!=ipcc||parent_irq(f,ipcc)!=gic||
       !tuple(f,glink,"interrupts",endpoint,3)||!tuple(f,rpm,"interrupts",endpoint,3)||
       !tuple(f,ipcc,"interrupts",receive,3)||!one(f,ipcc,"#interrupt-cells",3)||
       !one(f,ipcc,"#mbox-cells",2)||!one(f,gic,"#interrupt-cells",3)||
       !string(f,glink,"mbox-names","rpm_smem")||!string(f,rpm,"rpm-channel-name","rpm_requests")||
       !one(f,rpm,"rpm-channel-type",15)||!absent(f,rpm,"reg")||
       !absent(f,glink,"interrupts-extended")||!absent(f,rpm,"interrupts-extended")||
       !absent(f,ipcc,"interrupts-extended"))return false;
    uint32_t ipcHandle=fdt_get_phandle(f,ipcc),gicHandle=fdt_get_phandle(f,gic);
    const uint32_t mailbox[]={ipcHandle,0,0};
    if(!ipcHandle||!gicHandle||!tuple(f,glink,"mboxes",mailbox,3))return false;
    bzero(r,sizeof(*r));r->map={DRE_RPM_MSG_RAM_BASE,DRE_RPM_IPCC_BASE,
        DRE_RPM_MSG_RAM_BYTES,DRE_RPM_IPCC_BYTES,0,0};
    r->protocol={DRE_RPM_MSG_RAM_BASE,DRE_RPM_MSG_RAM_BYTES,DRE_RPM_TIMER_HZ,15,0,0};
    r->message_mapping_bytes=0x8000;r->ipcc_mapping_bytes=0x4000;
    r->protected_mpm_pa=mpmReg[0];r->protected_mpm_bytes=mpmReg[1];
    r->receive_spi=334;r->receive_intid=366;r->receive_flags=4;r->gic_phandle=gicHandle;
    return true;
}
static bool callbacks(const dre_rpm_native_broker *b)
{ return b&&b->version==DRE_RPM_NATIVE_BROKER_VERSION&&b->struct_bytes==sizeof(*b)&&
    b->owner_lock_held&&b->acquire&&b->check&&b->device_aliases&&b->access&&
    b->receive_owned&&b->retired&&b->release; }
static bool held(dre_rpm_native *s)
{
    if(!s||!s->lease||!s->remote_epoch||s->closing||s->unsafe)return false;
    if(s->broker.owner_lock_held(s->broker.cookie)||
       s->broker.check(s->broker.cookie,s->lease,s->remote_epoch)||
       s->broker.receive_owned(s->broker.cookie,s->lease,s->remote_epoch)) {
        s->error="RPM_OWNERSHIP_OR_RECEIVE_ROUTE_LOST";s->unsafe=true;return false;
    }
    return true;
}
/* Actual AT/PAR PA and Device attributes, not just VM_WIMG_IO request flags.
 * Keep this PE's translation result private from intervening IRQ/preemption. */
static bool deviceTranslation(uintptr_t va,uint64_t pa)
{
    uint64_t read,write;boolean_t interrupts=ml_set_interrupts_enabled(false);
    __asm__ volatile("at s1e1r, %2; isb; mrs %0, par_el1; at s1e1w, %2; isb; mrs %1, par_el1"
        : "=&r"(read),"=&r"(write):"r"(va):"memory");
    ml_set_interrupts_enabled(interrupts);
    const uint64_t mask=UINT64_C(0x000000fffffff000);
    uint64_t a=read>>56,b=write>>56;
    return !(read&1)&&!(write&1)&&(a==0||a==4)&&(b==0||b==4)&&a==b&&
        (read&mask)==(pa&mask)&&(write&mask)==(pa&mask)&&kvtophys(va)==pa;
}
static void unmap(dre_rpm_native_mapping *m)
{ if(m->cpu){ml_io_unmap(trunc_page(m->cpu),round_page(m->bytes+(m->cpu&PAGE_MASK)));m->cpu=0;} }
static bool map(dre_rpm_native *s,unsigned i,uint64_t pa,uint64_t bytes)
{
    dre_rpm_native_mapping *m=&s->mappings[i];m->pa=pa;m->bytes=bytes;
    if(!held(s)||s->broker.device_aliases(s->broker.cookie,s->lease,s->remote_epoch,pa,bytes))return false;
    m->cpu=ml_io_map_unmappable(pa,bytes,VM_WIMG_IO);
    if(!m->cpu||((uintptr_t)m->cpu&16383))return false;
    for(uint64_t off=0;off<bytes;off+=4096)
        if(!deviceTranslation(m->cpu+off,pa+off))return false;
    return held(s);
}
static volatile uint32_t *access(dre_rpm_native *s,uint64_t pa,int write)
{
    if(!s||!s->mapped||!held(s)||(pa&3))return NULL;
    const dre_rpm_native_resources *r=&s->resources;
    bool ram=pa>=r->map.message_base&&pa-r->map.message_base<=r->map.message_bytes-4;
    bool send=write&&pa==r->map.ipcc_base+0x0c;
    if(!ram&&!send)return NULL;
    /* vMPM is a foreign consumer INSIDE the advertised RPM RAM resource.
     * Never read/write its registers, even if a malformed TOC points there. */
    if(pa<r->protected_mpm_pa+r->protected_mpm_bytes&&r->protected_mpm_pa<pa+4)return NULL;
    unsigned i=ram?0:1;dre_rpm_native_mapping *m=&s->mappings[i];
    if(!m->cpu||pa<m->pa||pa-m->pa>m->bytes-4||
       s->broker.access(s->broker.cookie,s->lease,s->remote_epoch,pa,4,write))return NULL;
    uintptr_t cpu=m->cpu+(pa-m->pa);
    if(!deviceTranslation(cpu,pa)){s->unsafe=true;return NULL;}
    return held(s)?(volatile uint32_t *)cpu:NULL;
}
static uint64_t lease(void *cookie)
{ dre_rpm_native *s=(dre_rpm_native *)cookie;return held(s)?s->lease:0; }
static int read32(void *cookie,uint64_t pa,uint32_t *value)
{
    dre_rpm_native *s=(dre_rpm_native *)cookie;if(!value)return 0;*value=0;
    volatile uint32_t *p=access(s,pa,0);if(!p)return 0;
    *value=*p;__asm__ volatile("dsb sy":::"memory");return held(s)?1:0;
}
static int write32(void *cookie,uint64_t pa,uint32_t value)
{
    dre_rpm_native *s=(dre_rpm_native *)cookie;
    if(!s||s->writes==UINT64_MAX||(pa==DRE_RPM_IPCC_BASE+0x0c&&value))return 0;
    volatile uint32_t *p=access(s,pa,1);if(!p)return 0;
    ++s->writes; /* Publication may become visible before any later failure. */
    __asm__ volatile("dsb sy":::"memory");*p=value;__asm__ volatile("dsb sy":::"memory");
    return held(s)?1:0;
}
static void barrier(void *) { __asm__ volatile("dsb sy":::"memory"); }
static uint64_t now(void *cookie)
{ uint64_t ticks=0;if(held((dre_rpm_native *)cookie))__asm__ volatile("mrs %0,cntpct_el0":"=r"(ticks));return ticks; }
static bool preflightKnownPipes(dre_rpm_native *s)
{
    dre_rpm_layout layout={};dre_rpm_layout_report report={};
    uint8_t snapshot[DRE_RPM_LAYOUT_TOC_BYTES]={};
    if(strcmp(dre_rpm_layout_from_fdt(s->fdt,s->fdt_bytes,&layout),
        "RPM_LAYOUT_FDT_DESCRIBED_UNOWNED")) {
        s->error="RPM_SHARED_RESOURCE_LAYOUT_UNQUALIFIED";return false;
    }
    /* Actual qualified read-only TOC snapshot. Ownership still comes from the
     * real per-word broker; this description cannot grant any access. */
    for(unsigned off=0;off<sizeof(snapshot);off+=4) {
        uint32_t value=0;
        if(read32(s,layout.message_pa+layout.toc.offset+off,&value)!=1) {
            s->error="RPM_QUALIFIED_TOC_READ_FAILED";
            bzero(snapshot,sizeof(snapshot));return false;
        }
        for(unsigned byte=0;byte<4;byte++)snapshot[off+byte]=(uint8_t)(value>>(byte*8));
    }
    const char *result=dre_rpm_layout_describe_toc(&layout,snapshot,sizeof(snapshot),&report);
    bzero(snapshot,sizeof(snapshot));
    s->toc_problems=report.problems;
    bool described=!strcmp(result,"RPM_LAYOUT_TOC_DESCRIBED_UNOWNED")||
        !strcmp(result,"RPM_LAYOUT_TOC_CONFLICTS_DESCRIBED_UNOWNED");
    const uint32_t missing=DRE_RPM_LAYOUT_BAD_MAGIC|DRE_RPM_LAYOUT_BAD_COUNT|
        DRE_RPM_LAYOUT_MISSING_TX|DRE_RPM_LAYOUT_MISSING_RX|
        DRE_RPM_LAYOUT_DUPLICATE_TX|DRE_RPM_LAYOUT_DUPLICATE_RX;
    bool good=described&&!(report.problems&missing)&&report.tx_index<report.count&&
        report.rx_index<report.count&&report.count<=DRE_RPM_LAYOUT_MAX_ENTRIES;
    if(good) {
        const uint32_t conflict=DRE_RPM_LAYOUT_ENTRY_RANGE|DRE_RPM_LAYOUT_TOC_OVERLAP|
            DRE_RPM_LAYOUT_ENTRY_OVERLAP|DRE_RPM_LAYOUT_MPM_LINUX_CONFLICT|
            DRE_RPM_LAYOUT_MPM_UNKNOWN_CONFLICT|DRE_RPM_LAYOUT_PIPE_TRANSPORT_LIMIT;
        good=!(report.entry[report.tx_index].problems&conflict)&&
            !(report.entry[report.rx_index].problems&conflict);
    }
    /* Unknown entries remain unowned. Do not reinterpret them as FIFO
     * headers or use a gap as authority. Existing GLINK admission and every
     * actual per-word broker check still apply independently afterwards. */
    if(!good)s->error="RPM_KNOWN_PIPE_LAYOUT_OR_SHARED_RESOURCE_CONFLICT";
    bzero(&report,sizeof(report));return good&&held(s);
}
extern "C" int dre_rpm_native_create(const dre_rpm_native_broker *b,dre_rpm_native **out)
{
    if(!out)return DRE_GPU_INVALID;*out=NULL;
    if(!callbacks(b)||b->owner_lock_held(b->cookie))return DRE_GPU_INVALID;
    uint64_t hz;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
    if(page_size!=16384||hz!=DRE_RPM_TIMER_HZ)return DRE_GPU_UNSUPPORTED;
    dre_rpm_native *s=(dre_rpm_native *)IOMallocZero(sizeof(*s));if(!s)return DRE_GPU_NO_MEMORY;
    s->broker=*b;s->error="RPM_EXTERNAL_POLICY_NOT_ACQUIRED";
    if(dre_native_board_snapshot(&s->fdt,&s->fdt_bytes)||!profile(s->fdt,s->fdt_bytes,&s->resources)) {
        IOFree(s,sizeof(*s));return DRE_GPU_UNSUPPORTED;
    }
    int status=b->acquire(b->cookie,s->fdt,s->fdt_bytes,&s->resources,&s->lease,&s->remote_epoch);
    if(s->lease)*out=s; /* Partial native ownership is never forgotten. */
    if(status||!s->lease||!s->remote_epoch) {
        s->error="RPM_NATIVE_PARTIAL_ACQUIRE_RETAINED";
        if(!s->lease)IOFree(s,sizeof(*s));return status?status:DRE_GPU_NOT_READY;
    }
    if(!map(s,0,s->resources.map.message_base,s->resources.message_mapping_bytes)||
       !map(s,1,s->resources.map.ipcc_base,s->resources.ipcc_mapping_bytes)) {
        s->error="RPM_DEVICE_ALIAS_OR_MAPPING_UNQUALIFIED";s->unsafe=true;return DRE_GPU_FAULT;
    }
    s->mapped=true;
    if(!preflightKnownPipes(s)){s->unsafe=true;return DRE_GPU_NOT_READY;}
    s->error=NULL;return DRE_GPU_OK;
}
extern "C" int dre_rpm_native_binding(dre_rpm_native *s,dre_rpm_mmio_ops *ops,
    dre_rpm_mmio_map *mapping,dre_rpm_config *protocol)
{
    if(!ops||!mapping||!protocol)return DRE_GPU_INVALID;
    *ops={};*mapping={};*protocol={};
    if(!s||!s->mapped||!held(s))return DRE_GPU_NOT_READY;
    *ops={lease,read32,write32,barrier,now,s};*mapping=s->resources.map;*protocol=s->resources.protocol;
    return DRE_GPU_OK;
}
extern "C" int dre_rpm_native_diagnostic(dre_rpm_native *s,const char **reason,
    uint32_t *problems,uint64_t *writes)
{
    if(!s||!reason||!problems||!writes)return DRE_GPU_INVALID;
    if(s->broker.owner_lock_held(s->broker.cookie))return DRE_GPU_BUSY;
    *reason=s->error;*problems=s->toc_problems;*writes=s->writes;
    return DRE_GPU_OK; /* Metadata copied, not an ownership/clock/RPM ACK. */
}
extern "C" int dre_rpm_native_close(dre_rpm_native **owner)
{
    if(!owner||!*owner)return DRE_GPU_INVALID;dre_rpm_native *s=*owner;
    if(s->broker.owner_lock_held(s->broker.cookie)||!s->lease||
       s->broker.retired(s->broker.cookie,s->lease,s->remote_epoch))return DRE_GPU_BUSY;
    s->closing=true;
    /* Actual retirement joins every CPU/firmware user before these mappings
     * disappear. A failed release retains the object and existing mappings. */
    if(s->broker.release(s->broker.cookie,s->lease,s->remote_epoch))return DRE_GPU_BUSY;
    unmap(&s->mappings[1]);unmap(&s->mappings[0]);bzero(s,sizeof(*s));
    IOFree(s,sizeof(*s));*owner=NULL;return DRE_GPU_OK;
}
