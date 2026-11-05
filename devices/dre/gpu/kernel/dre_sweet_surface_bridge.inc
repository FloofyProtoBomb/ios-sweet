/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Include at END of actual selected SweetSurface.cpp after
 *0046/0068/0104 and DRE name substitution. No private class-offset copies.
 * The source shares actual sweet_surface_get/put/table lock and memory object.
 */
#if defined(DRE)
#include "dre_surface_backing.h"
extern "C" uint64_t kvtophys(vm_offset_t);
class DRESweetBackingPin : public OSObject {
    OSDeclareDefaultStructors(DRESweetBackingPin);
public:
    SweetSurface *surface;
    IOMemoryDescriptor *memory;
    uint64_t *pages;
    size_t pageBytes;
    bool tableUser,prepared,ready;
    dre_gpu_surface_description description;
    bool init() APPLE_KEXT_OVERRIDE {
        surface=NULL;memory=NULL;pages=NULL;pageBytes=0;
        tableUser=prepared=ready=false;bzero(&description,sizeof(description));
        return OSObject::init();
    }
    void free() APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(DRESweetBackingPin,OSObject);
void DRESweetBackingPin::free()
{
    /* Only close below drops the last reference after a successful prepare's
     * matching complete. Failed complete retains the owner and backing. */
    if(pages)IOFreeData(pages,pageBytes);
    OSSafeReleaseNULL(memory);
    if(surface) {if(tableUser)sweet_surface_put(surface);surface->release();surface=NULL;}
    OSObject::free();
}
static bool dre_surface_plane_bounds(const dre_gpu_surface_plane &p,uint64_t allocation)
{
    if(!p.width||!p.height||p.width>16384||p.height>16384||!p.bytes_per_element||p.bytes_per_element>64||
       !p.element_width||p.element_width>255||!p.element_height||p.element_height>255||
       !p.row_bytes||!p.bytes||p.offset>allocation||p.bytes>allocation-p.offset)return false;
    uint64_t columns=((uint64_t)p.width+p.element_width-1)/p.element_width;
    uint64_t rows=((uint64_t)p.height+p.element_height-1)/p.element_height;
    return columns*p.bytes_per_element<=p.row_bytes&&rows*p.row_bytes<=p.bytes;
}
static int dre_surface_layout(SweetSurface *s,dre_gpu_surface_description *d)
{
    if(!s->memory||!s->allocSize||s->allocSize>DRE_GPU_BO_MAX_BYTES||
       s->allocSize%16384||s->memory->getLength()!=s->allocSize||
       !s->width||!s->height||s->width>16384||s->height>16384||s->planeCount>DRE_GPU_MAX_SURFACE_PLANES)
        return DRE_GPU_INVALID;
    bzero(d,sizeof(*d));d->allocation_bytes=s->allocSize;d->surface_id=s->id;
    d->width=s->width;d->height=s->height;d->pixel_format=s->pixelFormat;
    d->plane_count=s->planeCount?s->planeCount:1;
    d->gpu_import_refusals=DRE_GPU_SURFACE_LIVE_CACHE_ALIASES|DRE_GPU_SURFACE_GPU_CACHE_OWNER_MISSING|
        DRE_GPU_SURFACE_TASK_VM_IMPORT_MISSING|DRE_GPU_SURFACE_SHARED_EVENT_MISSING|
        DRE_GPU_SURFACE_GPU_OWNERSHIP_UNPROVEN;
    for(uint32_t i=0;i<d->plane_count;i++) {
        dre_gpu_surface_plane *p=&d->planes[i];
        if(s->planeCount) {
            const sweet_surface_plane *old=&s->planes[i];
            *p={old->width,old->height,old->bytesPerRow,old->offset,old->size,
                old->bytesPerElement,old->elementWidth,old->elementHeight};
        } else {
            if(s->offset>s->allocSize)return DRE_GPU_INVALID;
            *p={s->width,s->height,s->bytesPerRow,s->offset,s->allocSize-s->offset,
                s->bytesPerElement,s->elementWidth,s->elementHeight};
        }
        if(!dre_surface_plane_bounds(*p,d->allocation_bytes))return DRE_GPU_INVALID;
        if(p->row_bytes%64)d->gpu_import_refusals|=DRE_GPU_SURFACE_PLANE_LAYOUT_UNSUPPORTED;
        for(uint32_t j=0;j<i;j++) {
            const dre_gpu_surface_plane *q=&d->planes[j];
            if((uint64_t)p->offset<q->offset+(uint64_t)q->bytes&&
               (uint64_t)q->offset<p->offset+(uint64_t)p->bytes)
                d->gpu_import_refusals|=DRE_GPU_SURFACE_PLANE_LAYOUT_UNSUPPORTED;
        }
    }
    bool packed=(d->pixel_format==0x42475241u||d->pixel_format==0x52474241u)&&d->plane_count==1&&
        d->planes[0].bytes_per_element==4&&d->planes[0].element_width==1&&d->planes[0].element_height==1;
    if(!packed)d->gpu_import_refusals|=DRE_GPU_SURFACE_FORMAT_UNSUPPORTED;
    return DRE_GPU_OK;
}
static void dre_surface_heap(uint64_t *a,uint32_t n,uint32_t at)
{
    for(;;) {uint32_t child=at*2+1;if(child>=n)return;
        if(child+1<n&&a[child+1]>a[child])child++;
        if(a[at]>=a[child])return;uint64_t swap=a[at];a[at]=a[child];a[child]=swap;at=child;}
}
static void dre_surface_sort(uint64_t *a,uint32_t n)
{
    for(uint32_t i=n/2;i;i--)dre_surface_heap(a,n,i-1);
    for(uint32_t i=n;i>1;i--) {uint64_t swap=a[0];a[0]=a[i-1];a[i-1]=swap;dre_surface_heap(a,i-1,0);}
}
static int dre_surface_retain_impl(task_t task,uint32_t port,uint32_t,void **out,dre_gpu_surface_description *description)
{
    OSObject *object=NULL;
    IOReturn ret=IOUserClient::copyObjectForPortNameInTask(task,(mach_port_name_t)port,&object);
    SweetSurface *s=OSDynamicCast(SweetSurface,object);
    if(ret!=kIOReturnSuccess||!s) {OSSafeReleaseNULL(object);return DRE_GPU_INVALID;}
    /* Reject oversized/invalid layouts before creating a retained export pin.
     * That keeps partial pins within the control client's reserved byte cap. */
    dre_gpu_surface_description descriptionCopy={};
    IOMemoryDescriptor *memory=NULL;
    IOLockLock(sweet_surface_lock);
    int status=dre_surface_layout(s,&descriptionCopy);
    if(status==DRE_GPU_OK) {memory=s->memory;memory->retain();}
    IOLockUnlock(sweet_surface_lock);
    if(status!=DRE_GPU_OK) {object->release();return status;}
    DRESweetBackingPin *pin=OSTypeAlloc(DRESweetBackingPin);
    if(!pin||!pin->init()) {OSSafeReleaseNULL(pin);memory->release();object->release();return DRE_GPU_NO_MEMORY;}
    pin->surface=s; /* copyObject's actual retain, not an ID lookup */
    pin->memory=memory;pin->description=descriptionCopy;
    *out=pin;       /* preserve ownership even if a later step fails */
    if(!sweet_surface_get(s))return DRE_GPU_STALE;
    pin->tableUser=true;
    pin->description.pin_flags=DRE_GPU_SURFACE_RETAINED;
    IOBufferMemoryDescriptor *buffer=OSDynamicCast(IOBufferMemoryDescriptor,pin->memory);
    if(!buffer) {
        pin->description.descriptor_kind=2;
        pin->description.gpu_import_refusals|=DRE_GPU_SURFACE_PHYSICAL_BACKING;
        pin->ready=true;*description=pin->description;return DRE_GPU_OK;
    }
    pin->description.descriptor_kind=1;
    uint32_t count=(uint32_t)(pin->description.allocation_bytes/4096);
    pin->pageBytes=(size_t)count*2*sizeof(uint64_t);
    pin->pages=(uint64_t *)IOMallocZeroData(pin->pageBytes);
    if(!pin->pages)return DRE_GPU_NO_MEMORY;
    if(pin->memory->prepare(kIODirectionInOut)!=kIOReturnSuccess)return DRE_GPU_FAULT;
    pin->prepared=true;pin->description.pin_flags|=DRE_GPU_SURFACE_DESCRIPTOR_PREPARED;
    const uint8_t *cpu=(const uint8_t *)buffer->getBytesNoCopy();
    if(!cpu||((uintptr_t)cpu&16383))return DRE_GPU_INVALID;
    for(uint32_t i=0;i<count;i++) {
        IOByteCount length=0;uint64_t offset=(uint64_t)i*4096;
        uint64_t pa=pin->memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!pa||(pa&4095)||pa>=(1ull<<40)||length<4096||kvtophys((vm_offset_t)cpu+offset)!=pa||
           ((i&3)&&pa!=pin->pages[i-(i&3)]+(uint64_t)(i&3)*4096)||(!(i&3)&&(pa&16383)))
            return DRE_GPU_INVALID;
        pin->pages[i]=pin->pages[count+i]=pa;
    }
    dre_surface_sort(pin->pages+count,count);
    for(uint32_t i=1;i<count;i++)if(pin->pages[count+i]==pin->pages[count+i-1])return DRE_GPU_INVALID;
    pin->description.page_count=count;
    pin->description.pin_flags|=DRE_GPU_SURFACE_PAGE_INVENTORY;
    /* Existing WB/default kernel and task aliases stay untouched. Inventory
     * and prepare are lifetime facts, not a GPU cache/VM import permission. */
    pin->ready=true;*description=pin->description;return DRE_GPU_OK;
}
static int dre_surface_describe_impl(void *object,dre_gpu_surface_description *description)
{
    DRESweetBackingPin *pin=(DRESweetBackingPin *)object;
    if(!pin||!pin->ready||!pin->surface||!pin->memory)return DRE_GPU_NOT_READY;
    IOLockLock(sweet_surface_lock);
    bool same=pin->surface->memory==pin->memory&&pin->memory->getLength()==pin->description.allocation_bytes;
    IOLockUnlock(sweet_surface_lock);
    if(!same)return DRE_GPU_STALE;
    *description=pin->description;return DRE_GPU_OK;
}
static int dre_surface_close_impl(void *object)
{
    DRESweetBackingPin *pin=(DRESweetBackingPin *)object;
    if(!pin)return DRE_GPU_INVALID;
    if(pin->prepared) {
        if(!pin->memory||pin->memory->complete(kIODirectionInOut)!=kIOReturnSuccess)return DRE_GPU_FAULT;
        pin->prepared=false;
    }
    pin->release();return DRE_GPU_OK;
}
static int dre_sweet_surface_bridge_install(IOService *owner)
{
    static const dre_surface_bridge ops={1,sizeof(dre_surface_bridge),
        dre_surface_retain_impl,dre_surface_describe_impl,dre_surface_close_impl};
    return dre_surface_install_bridge(owner,&ops);
}
#endif
