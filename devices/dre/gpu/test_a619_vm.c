/* SPDX-License-Identifier: MIT; independent CPU/SMMU/GPU model, no handset. */
#define main legacy_smmu_test_main
#include "test_a619_smmu.c" /* shared independent register model, not driver code */
#undef main
#include "a619_vm.h"
#define MAX_PAGES 65536u
#define MAX_TABLE_BYTES ((2u+MAX_PAGES/512u)*4096u)
static _Alignas(4096) uint64_t vm_tables[MAX_TABLE_BYTES/8];
struct vm_fixture {
    struct fixture base;
    struct dre_a619_vm vm;
    struct dre_a619_vm_ops io;
    struct dre_a619_smmu_page metadata[MAX_PAGES],input[MAX_PAGES];
    uint64_t leases[MAX_PAGES],reverse[MAX_PAGES*2];
    struct dre_a619_vm_mapping boot,front,back,extra;
    uint32_t gpu_status,gpu_irq,gpu_fault,gpu_rptr,gpu_wptr;
    int quiescent,cpu_owned,table_owned,retirement_expected,corrupt_leaf,mutate_input;
    unsigned retirement_syncs;
};
static int vm_owns(void *cookie,uint64_t pa,size_t bytes)
{
    struct vm_fixture *f=cookie;
    if (f->mutate_input && pa==f->input[2].pa) { f->input[2].pa=0x1000;f->mutate_input=0; }
    return !f->base.unowned && pa>=0x90000000 && pa<0xb0000000 && bytes<=0xb0000000-pa;
}
static int vm_cpu(void *cookie,const void *p,size_t bytes)
{ return ((struct vm_fixture *)cookie)->cpu_owned && p && bytes && (uintptr_t)p<=UINTPTR_MAX-bytes; }
static int vm_table_mapping(void *cookie,uint64_t pa,const void *p,size_t bytes)
{
    struct vm_fixture *f=cookie;
    return f->table_owned && pa==0x90000000 && p==vm_tables && bytes<=sizeof(vm_tables);
}
static int vm_quiescent(void *cookie) { return ((struct vm_fixture *)cookie)->quiescent; }
static int vm_read32(void *cookie,uint64_t address,uint32_t *value)
{
    struct vm_fixture *f=cookie;
    if (address==DRE_A619_GPU_BASE+0x210*4)*value=f->gpu_status;
    else if (address==DRE_A619_GPU_BASE+0x201*4)*value=f->gpu_irq;
    else if (address==DRE_A619_GPU_BASE+0x821*4)*value=f->gpu_fault;
    else if (address==DRE_A619_RB_RPTR)*value=f->gpu_rptr;
    else if (address==DRE_A619_RB_WPTR)*value=f->gpu_wptr;
    else return read32(cookie,address,value);
    return 1;
}
static int vm_write32(void *cookie,uint64_t address,uint32_t value)
{
    struct vm_fixture *f=cookie;
    if (address==BASE+CB+0x7f0 && f->retirement_expected) {
        struct dre_a619_vm_mapping *m=&f->front;
        assert(!m->retired && f->vm.leases[m->first_page]==m->lease);
        for (uint32_t i=0;i<m->page_count;i++)assert(!vm_tables[1024+m->first_page+i]);
        f->retirement_syncs++;
    }
    if (address==BASE+CB+0x7f0 && f->corrupt_leaf)vm_tables[1024+512]^=0x1000;
    return write32(cookie,address,value);
}
static void vm_init(struct vm_fixture *f,uint32_t n)
{
    memset(f,0,sizeof(*f));init(&f->base);memset(vm_tables,0xaa,sizeof(vm_tables));
    f->quiescent=f->cpu_owned=f->table_owned=1;
    struct dre_a619_smmu *s=&f->base.state;
    s->tables=vm_tables;s->pages=f->metadata;s->page_count=s->page_capacity=n;
    s->table_capacity_bytes=(2u+(n+511u)/512u)*4096u;s->va_base=0x80000000;
    f->vm=(struct dre_a619_vm){.domain=s,.pages=f->metadata,.leases=f->leases,
        .reverse_pa=f->reverse,.reverse_entries=n*2u};
    f->io=(struct dre_a619_vm_ops){.mmu={guard,vm_owns,vm_read32,vm_write32,
        read64,write64,barrier,now,f},.owns_cpu=vm_cpu,.table_mapping=vm_table_mapping,.quiescent=vm_quiescent};
    for (unsigned i=0;i<MAX_PAGES;i++)f->input[i]=(struct dre_a619_smmu_page){0x91000000+(uint64_t)i*8192,3};
}
static const char *prep(struct vm_fixture *f) { return dre_a619_vm_prepare(&f->vm,&f->io); }
static const char *map_bo(struct vm_fixture *f,uint32_t first,uint32_t start,uint32_t count,struct dre_a619_vm_mapping *m)
{ return dre_a619_vm_map(&f->vm,&f->io,first,f->input+start,count,m); }
static const char *unmap_bo(struct vm_fixture *f,struct dre_a619_vm_mapping *m)
{ return dre_a619_vm_unmap(&f->vm,&f->io,m); }
static void bootstrap(struct vm_fixture *f)
{
    assert(!strcmp(prep(f),"VM_PREPARED"));
    assert(!strcmp(map_bo(f,0,0,2,&f->boot),"VM_MAPPING_STAGED"));
    assert(!strcmp(dre_a619_vm_activate(&f->vm,&f->io),"VM_ACTIVE"));
}
/* Independent architectural walk through actual generated L1/L2/L3 tables. */
static uint64_t vm_translate(const struct vm_fixture *f,uint64_t va,int write,int execute)
{
    uint64_t pointer=f->base.state.table_pa;
    for (unsigned level=1;level<=3;level++) {
        unsigned shift=level==1?30:level==2?21:12;
        assert(pointer>=0x90000000 && pointer<0x90000000+f->base.state.table_capacity_bytes);
        uint64_t pte=vm_tables[(pointer-0x90000000)/8+((va>>shift)&511)];
        if ((pte&3)!=3)return 0;
        if (level<3) { pointer=pte&UINT64_C(0x000000fffffff000);continue; }
        if (write && ((pte>>6)&3)==3)return 0;
        if (execute && ((pte>>53)&3))return 0;
        return (pte&UINT64_C(0x000000fffffff000))|(va&4095);
    }
    return 0;
}
int main(void)
{
    static struct vm_fixture f;unsigned checks=0;
#define EXPECT(call,want) do {const char *got=(call);if (strcmp(got,want))fprintf(stderr,"expected %s got %s at line %u\n",want,got,__LINE__);assert(!strcmp(got,want));checks++;} while (0)
    vm_init(&f,16384);EXPECT(prep(&f),"VM_PREPARED");assert(!f.base.writes);
    EXPECT(map_bo(&f,0,0,2,&f.boot),"VM_MAPPING_STAGED");assert(!f.base.writes);
    /* Actual CPU-frame IOSurface:1125x2436 BGRA,4608-byte row stride. The
     * packed 2677-page payload is smaller than this observed 2741-page BO. */
    const uint32_t surface_pages=(4608u*2436u+4095u)/4096u;assert(surface_pages==2741);
    EXPECT(map_bo(&f,256,2,surface_pages,&f.front),"VM_MAPPING_STAGED");
    EXPECT(dre_a619_vm_activate(&f.vm,&f.io),"VM_ACTIVE");
    assert(f.vm.mapped_pages==surface_pages+2 && f.base.state.active);
    for (uint32_t i=0;i<surface_pages;i++)assert(vm_translate(&f,0x80100000+(uint64_t)i*4096+123,1,0)==f.input[i+2].pa+123);
    assert(!vm_translate(&f,0x80100000,0,1));assert(!vm_translate(&f,0x80002000,0,0));
    EXPECT(map_bo(&f,8192,3000,surface_pages,&f.back),"VM_MAPPING_ACTIVE");
    for (uint32_t i=0;i<surface_pages;i++)assert(vm_translate(&f,0x82000000+(uint64_t)i*4096+4095,1,0)==f.input[i+3000].pa+4095);
    assert(f.vm.mapped_pages==2*surface_pages+2);
    EXPECT(dre_a619_vm_release(&f.vm,&f.io),"VM_MAPPINGS_LIVE");
    struct dre_a619_smmu_view view={.table_pa=0x90040000,.table_bytes=16384,
        .asid=9,.task_generation=3,.reservation_lease=71};
    struct dre_a619_smmu_view_ops view_ops={&f.base,view_held,view_drained};
    f.base.task_owner=f.base.task_drained=1;
    EXPECT(dre_a619_smmu_enter_view(&f.base.state,&f.io.mmu,&view,&view_ops),"SMMU_TASK_VIEW_ENTERED");
    unsigned task_writes=f.base.writes;
    EXPECT(map_bo(&f,512,6500,1,&f.extra),"VM_TASK_VIEW_ACTIVE");
    EXPECT(unmap_bo(&f,&f.front),"VM_TASK_VIEW_ACTIVE");
    EXPECT(dre_a619_vm_release(&f.vm,&f.io),"VM_TASK_VIEW_ACTIVE");
    assert(!f.extra.lease && !f.front.retired && f.base.writes==task_writes);
    EXPECT(dre_a619_smmu_leave_view(&f.base.state,&f.io.mmu,3,71),"SMMU_BOOTSTRAP_VIEW_RESTORED");
    struct dre_a619_vm_mapping partial=f.front;partial.first_page++;partial.page_count--;
    EXPECT(unmap_bo(&f,&partial),"VM_PARTIAL_HANDLE");assert(!partial.retired);
    struct dre_a619_vm_mapping stale=f.front;stale.lease++;
    EXPECT(unmap_bo(&f,&stale),"VM_STALE_HANDLE");
    stale=f.front;f.retirement_expected=1;
    EXPECT(unmap_bo(&f,&f.front),"VM_UNMAPPED_RETIRED");
    assert(f.front.retired && f.retirement_syncs==1 && !vm_translate(&f,0x80100000,0,0));f.retirement_expected=0;
    f.front=(struct dre_a619_vm_mapping){0};
    EXPECT(map_bo(&f,256,2,surface_pages,&f.front),"VM_MAPPING_ACTIVE");
    EXPECT(unmap_bo(&f,&stale),"VM_STALE_HANDLE");
    EXPECT(unmap_bo(&f,&f.front),"VM_UNMAPPED_RETIRED");
    EXPECT(unmap_bo(&f,&f.back),"VM_UNMAPPED_RETIRED");
    EXPECT(unmap_bo(&f,&f.boot),"VM_UNMAPPED_RETIRED");
    EXPECT(dre_a619_vm_release(&f.vm,&f.io),"VM_RELEASED");restored(&f.base);
    /* A new VM generation prevents ABA even if address/lease1 are reused. */
    stale=f.boot;stale.retired=0;
    EXPECT(prep(&f),"VM_PREPARED");f.boot=(struct dre_a619_vm_mapping){0};
    EXPECT(map_bo(&f,0,0,2,&f.boot),"VM_MAPPING_STAGED");
    EXPECT(unmap_bo(&f,&stale),"VM_INVALID_HANDLE");
    EXPECT(unmap_bo(&f,&f.boot),"VM_UNMAPPED_RETIRED");
    EXPECT(dre_a619_vm_release(&f.vm,&f.io),"VM_RELEASED");
    /* Largest configured256MiB window, including final leaf of L3 table 127. */
    vm_init(&f,MAX_PAGES);EXPECT(prep(&f),"VM_PREPARED");
    EXPECT(map_bo(&f,MAX_PAGES-1,0,1,&f.front),"VM_MAPPING_STAGED");
    EXPECT(dre_a619_vm_activate(&f.vm,&f.io),"VM_ACTIVE");
    assert(vm_translate(&f,0x8fffffff,1,0)==f.input[0].pa+4095 && !vm_translate(&f,0x90000000,0,0));
    EXPECT(unmap_bo(&f,&f.front),"VM_UNMAPPED_RETIRED");
    EXPECT(dre_a619_vm_release(&f.vm,&f.io),"VM_RELEASED");
    /* Dense256MiB mapping: all 65536 pages, bounded hash and O(n log n)
     * activation validation, every physical leaf checked independently. */
    vm_init(&f,MAX_PAGES);EXPECT(prep(&f),"VM_PREPARED");
    for (uint32_t i=0;i<MAX_PAGES;i++)f.input[i].pa=0x91000000+(uint64_t)i*4096;
    EXPECT(map_bo(&f,0,0,MAX_PAGES,&f.front),"VM_MAPPING_STAGED");
    EXPECT(dre_a619_vm_activate(&f.vm,&f.io),"VM_ACTIVE");
    for (uint32_t i=0;i<MAX_PAGES;i++)assert(vm_translate(&f,0x80000000+(uint64_t)i*4096+13,1,0)==f.input[i].pa+13);
    EXPECT(unmap_bo(&f,&f.front),"VM_UNMAPPED_RETIRED");
    EXPECT(dre_a619_vm_release(&f.vm,&f.io),"VM_RELEASED");
    vm_init(&f,16384);f.base.state.table_capacity_bytes-=4096;EXPECT(prep(&f),"VM_INVALID_GEOMETRY");assert(!f.base.writes);
    vm_init(&f,16384);f.base.state.va_base=0xffe00000;EXPECT(prep(&f),"VM_INVALID_GEOMETRY");
    vm_init(&f,16384);f.vm.leases=(uint64_t *)f.metadata;EXPECT(prep(&f),"VM_CPU_ALIAS");
    vm_init(&f,16384);f.table_owned=0;EXPECT(prep(&f),"VM_UNOWNED_TABLES");
    vm_init(&f,16384);f.cpu_owned=0;EXPECT(prep(&f),"VM_UNOWNED_CPU_MEMORY");
    vm_init(&f,16384);EXPECT(prep(&f),"VM_PREPARED");
    EXPECT(dre_a619_vm_activate(&f.vm,&f.io),"VM_EMPTY");
    f.input[0].permissions=7;EXPECT(map_bo(&f,0,0,1,&f.front),"VM_INVALID_PHYSICAL_PAGE");assert(!f.base.writes);
    f.input[0].permissions=2;EXPECT(map_bo(&f,0,0,1,&f.front),"VM_INVALID_PHYSICAL_PAGE");
    f.input[0].permissions=3;f.input[0].pa=0x90000000;
    EXPECT(map_bo(&f,0,0,1,&f.front),"VM_INVALID_PHYSICAL_PAGE");
    f.input[0].pa=0x91000000;f.input[2].pa=f.input[0].pa;
    EXPECT(map_bo(&f,0,0,3,&f.front),"VM_PHYSICAL_ALIAS");assert(!f.vm.mapped_pages && !f.front.lease && !f.metadata[0].pa);
    EXPECT(map_bo(&f,0,0,2,&f.front),"VM_MAPPING_STAGED");
    EXPECT(map_bo(&f,0,3,1,&f.back),"VM_RANGE_OCCUPIED");
    EXPECT(map_bo(&f,512,0,2,&f.back),"VM_PHYSICAL_ALIAS");
    EXPECT(dre_a619_vm_map(&f.vm,&f.io,512,f.metadata,1,&f.back),"VM_INVALID_RANGE");
    vm_init(&f,16384);bootstrap(&f);
    for (unsigned i=0;i<f.vm.reverse_entries;i++)f.vm.reverse_pa[i]=0x1000;
    EXPECT(map_bo(&f,512,3,1,&f.front),"VM_REVERSE_PROBE_LIMIT");assert(!f.metadata[512].pa);
    /* Each unsafe mapping/removal case retains the lease and pins: no retired
     * handle can be returned before a completed real TLB synchronization. */
    vm_init(&f,16384);bootstrap(&f);EXPECT(map_bo(&f,512,2,8,&f.front),"VM_MAPPING_ACTIVE");
    f.retirement_expected=1;f.base.timeouts=f.base.tlb_count+1;
    EXPECT(unmap_bo(&f,&f.front),"VM_RESET_REQUIRED");assert(!f.front.retired && f.leases[512]==f.front.lease && f.vm.unsafe && f.base.state.unsafe);
    EXPECT(dre_a619_vm_release(&f.vm,&f.io),"VM_RESET_REQUIRED");
    vm_init(&f,16384);bootstrap(&f);EXPECT(map_bo(&f,512,2,8,&f.front),"VM_MAPPING_ACTIVE");
    f.base.fail_write=f.base.writes+1;EXPECT(unmap_bo(&f,&f.front),"VM_RESET_REQUIRED");assert(!f.front.retired && f.metadata[512].pa);
    vm_init(&f,16384);bootstrap(&f);f.base.timeouts=f.base.tlb_count+1;
    EXPECT(map_bo(&f,512,2,8,&f.front),"VM_RESET_REQUIRED");assert(!f.front.retired && f.front.lease && f.leases[512]==f.front.lease);
    vm_init(&f,16384);bootstrap(&f);f.corrupt_leaf=1;
    EXPECT(map_bo(&f,512,2,8,&f.front),"VM_RESET_REQUIRED");assert(!f.front.retired && f.front.lease);
    vm_init(&f,16384);bootstrap(&f);EXPECT(map_bo(&f,512,2,8,&f.front),"VM_MAPPING_ACTIVE");
    f.gpu_wptr=1;unsigned writes=f.base.writes;
    EXPECT(unmap_bo(&f,&f.front),"VM_RESET_REQUIRED");assert(f.base.writes==writes && !f.front.retired && vm_tables[1024+512]);
    vm_init(&f,16384);bootstrap(&f);EXPECT(map_bo(&f,512,2,8,&f.front),"VM_MAPPING_ACTIVE");
    f.base.regs[(CB+0x58)/4]=2;writes=f.base.writes;
    EXPECT(unmap_bo(&f,&f.front),"VM_RESET_REQUIRED");assert(f.base.writes==writes && !f.front.retired);
    vm_init(&f,16384);bootstrap(&f);EXPECT(map_bo(&f,512,2,8,&f.front),"VM_MAPPING_ACTIVE");
    vm_tables[1024+512]^=0x1000;EXPECT(unmap_bo(&f,&f.front),"VM_RESET_REQUIRED");assert(!f.front.retired);
    vm_init(&f,16384);bootstrap(&f);f.base.regs[(CB+0x20)/4]^=0x4000;
    EXPECT(map_bo(&f,512,2,8,&f.front),"VM_RESET_REQUIRED");assert(!f.front.lease);
    vm_init(&f,16384);bootstrap(&f);f.base.owner=0;
    EXPECT(map_bo(&f,512,2,8,&f.front),"VM_RESET_REQUIRED");assert(!f.front.lease);
    vm_init(&f,16384);bootstrap(&f);f.quiescent=0;
    EXPECT(map_bo(&f,512,2,8,&f.front),"VM_RESET_REQUIRED");assert(!f.front.lease);
    vm_init(&f,16384);bootstrap(&f);f.gpu_irq=1;
    EXPECT(map_bo(&f,512,2,8,&f.front),"VM_RESET_REQUIRED");assert(!f.front.lease);
    vm_init(&f,16384);bootstrap(&f);f.gpu_fault=2;
    EXPECT(map_bo(&f,512,2,8,&f.front),"VM_RESET_REQUIRED");assert(!f.front.lease);
    vm_init(&f,16384);bootstrap(&f);f.base.regs[0x48/4]=2;
    EXPECT(map_bo(&f,512,2,8,&f.front),"VM_RESET_REQUIRED");assert(!f.front.lease && f.base.regs[0x48/4]==2);
    vm_init(&f,16384);EXPECT(prep(&f),"VM_PREPARED");
    EXPECT(map_bo(&f,0,0,2,&f.boot),"VM_MAPPING_STAGED");f.base.regs[0x48/4]=2;
    EXPECT(dre_a619_vm_activate(&f.vm,&f.io),"SMMU_GLOBAL_FAULT");assert(!f.base.writes);
    vm_init(&f,16384);bootstrap(&f);
    EXPECT(dre_a619_smmu_sync_live(&f.base.state,0),"INVALID_ARGUMENT");
    EXPECT(dre_a619_smmu_sync_live(0,&f.io.mmu),"INVALID_ARGUMENT");
    vm_init(&f,16384);bootstrap(&f);f.input[2].permissions=5;
    EXPECT(map_bo(&f,512,2,1,&f.front),"VM_MAPPING_ACTIVE");
    assert(vm_translate(&f,0x80200000,0,1)==f.input[2].pa && !vm_translate(&f,0x80200000,1,0));
    f.input[3].permissions=1;EXPECT(map_bo(&f,513,3,1,&f.back),"VM_MAPPING_ACTIVE");
    assert(vm_translate(&f,0x80201000,0,0)==f.input[3].pa && !vm_translate(&f,0x80201000,1,0) && !vm_translate(&f,0x80201000,0,1));
    vm_init(&f,16384);bootstrap(&f);f.mutate_input=1;
    EXPECT(map_bo(&f,512,2,1,&f.front),"VM_MAPPING_ACTIVE");
    assert(f.input[2].pa==0x1000 && vm_translate(&f,0x80200000,1,0)==0x91004000);
    vm_init(&f,16384);EXPECT(prep(&f),"VM_PREPARED");
    EXPECT(map_bo(&f,0,0,2,&f.boot),"VM_MAPPING_STAGED");f.metadata[1].pa=f.metadata[0].pa;
    EXPECT(dre_a619_vm_activate(&f.vm,&f.io),"SMMU_PHYSICAL_ALIAS");assert(!f.base.writes);
    printf("PASS %u larger BO/dual-framebuffer/lease/TLB-retirement cases; synthetic hardware only\n",checks);
}
