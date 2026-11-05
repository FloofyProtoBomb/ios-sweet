/* SPDX-License-Identifier: MIT
 * UNVALIDATED real allocator/VM glue. No tests, compilation or execution. */
#include "boot_memory.h"
#include <string.h>

static int equal(const char *a,const char *b)
{ if(!a||!b)return 0;while(*a&&*a==*b){++a;++b;}return *a==*b; }
static const char *error(struct dre_gpu_boot_memory *b,const char *why)
{ b->error=why;return why; }
static int owned(struct dre_gpu_boot_memory *b,unsigned slot,size_t need)
{
    const struct dre_gpu_boot_buffer *a=&b->buffer[slot];
    const struct dre_gpu_native_memory *m=&a->memory;
    return a->allocation && m->cpu && !((uintptr_t)m->cpu&16383) &&
        m->bytes>=need && m->bytes<=DRE_GPU_BO_MAX_BYTES && !(m->bytes&16383) &&
        m->page_count==m->bytes/4096 && m->pages &&
        m->cache_contract==DRE_GPU_CACHE_OWNED_NORMAL_NC_EXCLUSIVE &&
        b->allocator.verify_memory(b->allocator.cookie,m)==1 &&
        b->allocator.owns_cpu(b->allocator.cookie,m->cpu,(size_t)m->bytes)==1 &&
        b->allocator.owns_cpu(b->allocator.cookie,m->pages,
            (size_t)m->page_count*sizeof(*m->pages))==1;
}
/* Purpose comes from this actual fixed constructor, never a CPU/PA range,
 * a requested access combination, a user's label, or a later catalog guess.
 * Every role is set once by the real registry before any VM publication.
 */
static enum dre_gpu_allocation_purpose purpose(unsigned slot)
{
    switch(slot) {
    case DRE_GPU_BOOT_TABLES:return DRE_GPU_ALLOC_BOOT_TABLES;
    case DRE_GPU_BOOT_PAGES:
    case DRE_GPU_BOOT_LEASES:
    case DRE_GPU_BOOT_REVERSE:return DRE_GPU_ALLOC_CPU_METADATA;
    case DRE_GPU_BOOT_RING:return DRE_GPU_ALLOC_RING;
    case DRE_GPU_BOOT_SQE:return DRE_GPU_ALLOC_SQE_UCODE;
    case DRE_GPU_BOOT_SCRATCH:return DRE_GPU_ALLOC_CP_SCRATCH;
    case DRE_GPU_BOOT_POWERUP:return DRE_GPU_ALLOC_CP_POWERUP;
    case DRE_GPU_BOOT_RESULT:return DRE_GPU_ALLOC_PRIVATE_COMPLETION;
    default:return DRE_GPU_ALLOC_UNKNOWN;
    }
}
static int allocate(struct dre_gpu_boot_memory *b,unsigned slot,uint64_t bytes,
    uint32_t permissions,int contiguous)
{
    enum dre_gpu_allocation_purpose role=purpose(slot);
    if(slot>=DRE_GPU_BOOT_SLOTS||role==DRE_GPU_ALLOC_UNKNOWN)return 0;
    struct dre_gpu_boot_buffer *a=&b->buffer[slot];
    if(a->allocation || a->memory.cpu || !bytes || bytes>DRE_GPU_BO_MAX_BYTES)return 0;
    /* Native XNU allocations are whole16KiB CPU granules. Logical GPU table
     * prefixes/SQE code and small metadata arrays may be only4KiB aligned. */
    uint64_t allocation_bytes=(bytes+16383)&~UINT64_C(16383);
    int result=contiguous?b->allocator.allocate_contiguous(b->allocator.cookie,allocation_bytes,permissions,
            &a->allocation,&a->memory):
        b->allocator.allocate(b->allocator.cookie,allocation_bytes,permissions,&a->allocation,&a->memory);
    /* A failed allocator may still return retained ownership. Never erase
     * that record or call a speculative free from this preparation helper. */
    if(a->allocation)b->held|=1u<<slot;
    if(result!=DRE_GPU_OK||a->memory.bytes!=allocation_bytes||
       !owned(b,slot,(size_t)allocation_bytes))return 0;
    for(uint32_t i=0;i<a->memory.page_count;i++)
        if(a->memory.pages[i].permissions!=permissions)return 0;
    /* Tag the exact verified handle/view, including its rounded allocation
     * padding. A refusal retains the allocation/held bit with no retry/free.
     */
    if(b->allocator.classify(b->allocator.cookie,a->allocation,&a->memory,role)!=DRE_GPU_OK) {
        b->error="BOOT_MEMORY_ALLOCATION_PURPOSE_REJECTED_RETAINED";return 0;
    }
    return 1;
}
int dre_gpu_boot_table_mapping(struct dre_gpu_boot_memory *b,uint64_t pa,
    const void *cpu,size_t bytes)
{
    if(!b || !b->attempted || b->unsafe || !bytes ||
        bytes!=b->domain.table_capacity_bytes || cpu!=b->domain.tables ||
        pa!=b->domain.table_pa || !owned(b,DRE_GPU_BOOT_TABLES,bytes))return 0;
    const struct dre_gpu_native_memory *m=&b->buffer[DRE_GPU_BOOT_TABLES].memory;
    if(cpu!=m->cpu || pa!=m->pages[0].pa || (pa&4095) ||
        pa>=(UINT64_C(1)<<40) || bytes>(UINT64_C(1)<<40)-pa)return 0;
    for(size_t off=0;off<bytes;off+=4096)
        if(m->pages[off/4096].pa!=pa+off ||
           b->allocator.owns_ram(b->allocator.cookie,pa+off,4096)!=1)return 0;
    return 1;
}
static int stage_buffer(struct dre_gpu_boot_memory *b,unsigned number,unsigned slot,
    uint32_t first,uint32_t pages,uint32_t permissions)
{
    struct dre_gpu_native_memory *m=&b->buffer[slot].memory;
    if(!owned(b,slot,(size_t)pages*4096))return 0;
    for(uint32_t i=0;i<pages;i++)if(m->pages[i].permissions!=permissions)return 0;
    const char *result=dre_a619_vm_map(&b->vm,&b->ops,first,m->pages,pages,&b->mapping[number]);
    if(!equal(result,"VM_MAPPING_STAGED")) {
        b->error=result;b->unsafe|=b->vm.unsafe;return 0;
    }
    struct dre_a619_vm_mapping *mapping=&b->mapping[number];
    if(!mapping->lease||mapping->retired||mapping->generation!=b->vm.generation||
       mapping->first_page!=first||mapping->page_count!=pages||b->domain.active||
       b->domain.changed||b->domain.unsafe||
       b->allocator.record_mapping(b->allocator.cookie,b->buffer[slot].allocation,
            &b->vm,&b->ops,mapping)!=DRE_GPU_OK) {
        /* vm_map already recorded this genuine staged mapping. Preserve its
         * exact handle, all allocations and metadata even if catalog binding
         * refuses. No guessed GPUVA/slot is accepted as mapping authority.
         */
        b->error="BOOT_MEMORY_REAL_MAPPING_RECORD_REJECTED_RETAINED";return 0;
    }
    b->expected[number]=(struct dre_a619_cp_buffer){
        {b->domain.va_base+(uint64_t)first*4096,(uint64_t)pages*4096},
        (volatile uint32_t *)m->cpu};
    b->expected_permissions[number]=permissions;
    return 1;
}
const char *dre_gpu_boot_memory_prepare(struct dre_gpu_boot_memory *b,
    const struct dre_gpu_native_allocator *a,const struct dre_a619_vm_ops *ops,
    const struct dre_a619_smmu *reservation,uint32_t n,
    const void *file,size_t file_bytes)
{
    if(!b || !a || !ops || !reservation || !a->allocate || !a->allocate_contiguous ||
        !a->owns_cpu || !a->owns_ram || !a->verify_memory || !ops->owns_cpu ||
        !ops->table_mapping || !ops->quiescent || !ops->mmu.guard ||
        !ops->mmu.owns_ram || !ops->mmu.read32 || !ops->mmu.write32 ||
        !ops->mmu.read64 || !ops->mmu.write64 || !ops->mmu.barrier || !ops->mmu.now)
        return "BOOT_MEMORY_BINDING_INCOMPLETE";
    if(a->version!=DRE_GPU_ALLOCATOR_VERSION||a->struct_bytes!=sizeof(*a)||
       !a->classify||!a->record_mapping)
        return "BOOT_MEMORY_ALLOCATION_PURPOSE_API_REQUIRED";
    if(b->attempted || b->held || b->initialized || b->unsafe)
        return "BOOT_MEMORY_FRESH_OWNER_REQUIRED";
    if(ops->owns_cpu(ops->mmu.cookie,b,sizeof(*b))!=1 ||
        ops->mmu.guard(ops->mmu.cookie)!=1)return "BOOT_MEMORY_CONTROL_NOT_OWNED";
    if(reservation->chip_id!=DRE_A619_CHIP_ID || reservation->tick_hz!=19200000 ||
        reservation->prerequisites!=(DRE_A619_SMMU_CB_OWNED|DRE_A619_SMMU_ASID_OWNED) ||
        reservation->active || reservation->changed || reservation->unsafe ||
        n<1024 || n>DRE_A619_SMMU_MAX_PAGES || (n&(n-1)) ||
        (reservation->va_base&0x1fffff) || reservation->va_base>=(UINT64_C(1)<<39) ||
        (uint64_t)n*4096>(UINT64_C(1)<<30)-(reservation->va_base&0x3fffffff))
        return "BOOT_MEMORY_DOMAIN_RESERVATION_REQUIRED";
    struct dre_a619_sqe fw;
    const char *status=dre_a619_sqe_view(DRE_A619_CHIP_ID,file,file_bytes,&fw);
    if(!equal(status,"SQE_VIEW_READY") || !fw.bytes || fw.bytes>1024*1024)
        return "BOOT_MEMORY_PRIVATE_SQE_REQUIRED";
    b->attempted=1;b->allocator=*a;b->ops=*ops;b->error=0;
    b->domain=(struct dre_a619_smmu){
        .chip_id=reservation->chip_id,.prerequisites=reservation->prerequisites,
        .tick_hz=reservation->tick_hz,.context_bank=reservation->context_bank,
        .smr_index=reservation->smr_index,.asid=reservation->asid,
        .va_base=reservation->va_base,.page_count=n,.page_capacity=n,
        .table_capacity_bytes=(2u+(n+511u)/512u)*4096u};
    if(!allocate(b,DRE_GPU_BOOT_TABLES,b->domain.table_capacity_bytes,3,1) ||
       !allocate(b,DRE_GPU_BOOT_PAGES,(uint64_t)n*sizeof(struct dre_a619_smmu_page),3,0) ||
       !allocate(b,DRE_GPU_BOOT_LEASES,(uint64_t)n*8,3,0) ||
       !allocate(b,DRE_GPU_BOOT_REVERSE,(uint64_t)n*16,3,0))
        return error(b,b->error?b->error:"BOOT_MEMORY_TABLE_OR_BOOKKEEPING_ALLOCATION_FAILED");
    b->domain.tables=(uint64_t *)b->buffer[DRE_GPU_BOOT_TABLES].memory.cpu;
    b->domain.table_pa=b->buffer[DRE_GPU_BOOT_TABLES].memory.pages[0].pa;
    b->domain.pages=(struct dre_a619_smmu_page *)b->buffer[DRE_GPU_BOOT_PAGES].memory.cpu;
    b->vm=(struct dre_a619_vm){.domain=&b->domain,
        .pages=(struct dre_a619_smmu_page *)b->domain.pages,
        .leases=(uint64_t *)b->buffer[DRE_GPU_BOOT_LEASES].memory.cpu,
        .reverse_pa=(uint64_t *)b->buffer[DRE_GPU_BOOT_REVERSE].memory.cpu,
        .reverse_entries=n*2u};
    if(!dre_gpu_boot_table_mapping(b,b->domain.table_pa,b->domain.tables,
        b->domain.table_capacity_bytes))return error(b,"BOOT_MEMORY_TABLE_PREFIX_NOT_PROVEN");
    /* Derive only the fresh allocation's independently verified NC property.
     * POWERED/GLOBAL_READY/QUIESCENT belong to real post-power qualification,
     * not to a future promise while this helper touches CPU memory only. */
    b->domain.prerequisites|=DRE_A619_SMMU_TABLES_NC;
    status=dre_a619_vm_prepare(&b->vm,&b->ops);
    if(!equal(status,"VM_PREPARED"))return error(b,status);
    uint32_t sqe_pages=(uint32_t)((fw.bytes+4095)/4096);
    if(!allocate(b,DRE_GPU_BOOT_RING,16384,1,0) ||
       !allocate(b,DRE_GPU_BOOT_SQE,(uint64_t)sqe_pages*4096,5,0) ||
       !allocate(b,DRE_GPU_BOOT_SCRATCH,16384,3,0) ||
       !allocate(b,DRE_GPU_BOOT_POWERUP,16384,3,0) ||
       !allocate(b,DRE_GPU_BOOT_RESULT,16384,3,0))
        return error(b,b->error?b->error:"BOOT_MEMORY_CP_ALLOCATION_FAILED");
    uint32_t scratch=4+((sqe_pages+3)&~3u),powerup=scratch+4,result=powerup+4;
    if(result+4>512 || result+4>n)return error(b,"BOOT_MEMORY_PROTECTED_BLOCK_OVERFLOW");
    if(!stage_buffer(b,0,DRE_GPU_BOOT_RING,0,4,1) ||
       !stage_buffer(b,1,DRE_GPU_BOOT_SQE,4,sqe_pages,5) ||
       !stage_buffer(b,2,DRE_GPU_BOOT_SCRATCH,scratch,1,3) ||
       !stage_buffer(b,3,DRE_GPU_BOOT_POWERUP,powerup,1,3) ||
       !stage_buffer(b,4,DRE_GPU_BOOT_RESULT,result,1,3))
        return error(b,b->error?b->error:"BOOT_MEMORY_MAPPING_REJECTED");
    b->queue=(struct dre_a619_queue){.chip_id=DRE_A619_CHIP_ID,
        .ring=b->expected[0].cpu,.ring_region=b->expected[0].region,
        .result=b->expected[4].cpu,.result_region=b->expected[4].region,.ring_dwords=4096};
    b->cp=(struct dre_a619_cp){.chip_id=DRE_A619_CHIP_ID,.queue=&b->queue,
        .firmware=file,.firmware_bytes=file_bytes,.sqe=b->expected[1],
        .scratch=b->expected[2],.powerup=b->expected[3]};
    b->first_unused_gpu_va=b->domain.va_base+(uint64_t)(result+4)*4096;
    b->initialized=1;b->error=0;
    return "BOOT_MEMORY_STAGED";
}
int dre_gpu_boot_cp_mapping(struct dre_gpu_boot_memory *b,
    const struct dre_a619_cp_buffer *buffer,uint32_t permissions)
{
    static const unsigned slots[5]={DRE_GPU_BOOT_RING,DRE_GPU_BOOT_SQE,
        DRE_GPU_BOOT_SCRATCH,DRE_GPU_BOOT_POWERUP,DRE_GPU_BOOT_RESULT};
    if(!b || !buffer || !b->initialized || b->unsafe || b->vm.unsafe ||
        !b->domain.active || b->domain.unsafe ||
        !equal(dre_a619_smmu_check_live(&b->domain,&b->ops.mmu),"SMMU_LIVE_STATE_VERIFIED"))return 0;
    for(unsigned number=0;number<5;number++) {
        const struct dre_a619_cp_buffer *expected=&b->expected[number];
        if(buffer->cpu!=expected->cpu || buffer->region.gpu!=expected->region.gpu ||
            buffer->region.bytes!=expected->region.bytes)continue;
        if(permissions!=b->expected_permissions[number] ||
            !owned(b,slots[number],(size_t)expected->region.bytes))return 0;
        const struct dre_a619_vm_mapping *mapping=&b->mapping[number];
        const struct dre_gpu_native_memory *memory=&b->buffer[slots[number]].memory;
        if(mapping->retired || !mapping->lease || mapping->generation!=b->vm.generation ||
            mapping->first_page>=b->domain.page_count || !mapping->page_count ||
            mapping->page_count>b->domain.page_count-mapping->first_page ||
            expected->region.gpu!=b->domain.va_base+(uint64_t)mapping->first_page*4096 ||
            (uint64_t)mapping->page_count*4096!=expected->region.bytes)return 0;
        for(uint32_t i=0;i<mapping->page_count;i++) {
            uint32_t page=mapping->first_page+i;
            struct dre_a619_smmu_page actual=b->vm.pages[page];
            if(b->vm.leases[page]!=mapping->lease || actual.pa!=memory->pages[i].pa ||
                actual.permissions!=permissions)return 0;
            uint64_t pte=actual.pa|UINT64_C(0xe63);
            if(!(permissions&DRE_A619_SMMU_WRITE))pte|=0x80;
            if(!(permissions&DRE_A619_SMMU_EXECUTE))pte|=UINT64_C(3)<<53;
            if(b->domain.tables[1024+page]!=pte)return 0;
        }
        return b->ops.mmu.guard(b->ops.mmu.cookie)==1;
    }
    return 0;
}
const char *dre_gpu_boot_memory_bind(struct dre_gpu_boot_memory *b,
    struct dre_a619_start_plan *plan)
{
    if(!b || !plan || !b->initialized || b->unsafe || b->vm.unsafe || b->domain.active ||
        plan->io.cookie!=b->ops.mmu.cookie)return "BOOT_MEMORY_PLAN_BINDING_REJECTED";
    plan->vm=&b->vm;plan->vm_ops=&b->ops;plan->smmu=&b->domain;
    plan->smmu_ops=b->ops.mmu;plan->cp=&b->cp;
    return "BOOT_MEMORY_PLAN_BOUND";
}
