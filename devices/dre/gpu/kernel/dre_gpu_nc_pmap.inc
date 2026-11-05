/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight draft. Include only at the END of XNU's actual
 * osfmk/arm/pmap/pmap.c; these private helpers must use its real locking and
 * geometry, not copies of private struct offsets or fake requested flags.
 * No pmap state, cache attributes, PTE or TLB is modified by these proofs. */
#if defined(DRE)
#include <kern/task.h>
#include <vm/vm_map.h>

#if !XNU_MONITOR && __ARM_PTE_PHYSMAP__
static int
dre_gpu_nc_leaf(pmap_t map,uint64_t address,uint64_t expected_pa,uint32_t access)
{
    if(!map||pt_attr_page_size(pmap_get_pt_attr(map))!=16384||
       (address&16383)||(expected_pa&16383))return 0;
    int ok=0;
    /* One CPU page per bounded lock acquisition. pmap_lock disables
     * preemption; never hold it across allocation, callbacks or sleeps. */
    pmap_lock(map,PMAP_LOCK_SHARED);
    pt_entry_t *pointer=pmap_pte(map,address);
    if(pointer!=PT_ENTRY_NULL) {
        pt_entry_t leaf=__atomic_load_n(pointer,__ATOMIC_RELAXED);
        uint32_t ap=(uint32_t)((leaf&ARM_PTE_APMASK)>>6);
        int permissions=access?(ap==(access&2u?AP_RWRW:AP_RORO)):ap==AP_RWNA;
        ok=(leaf&3)==ARM_PTE_TYPE_VALID&&
            (leaf&ARM_PTE_ATTRINDXMASK)==ARM_PTE_ATTRINDX(CACHE_ATTRINDX_WRITECOMB)&&
            (leaf&(ARM_PTE_NX|ARM_PTE_PNX))==(ARM_PTE_NX|ARM_PTE_PNX)&&
            ((uint64_t)pte_to_pa(leaf)&~UINT64_C(16383))==expected_pa&&permissions;
    }
    pmap_unlock(map,PMAP_LOCK_SHARED);
    return ok;
}
static int
dre_gpu_nc_verify(pmap_t map,uint64_t address,const void *metadata,
    uint32_t count,uint32_t stride,uint32_t access)
{
    if(PAGE_SIZE!=16384||!map||!metadata||((uintptr_t)metadata&7)||
       !count||count>16384||(count&3)||stride<8||stride>64||(stride&7)||
       (uintptr_t)metadata>UINTPTR_MAX-(size_t)count*stride||
       (address&16383)||address>UINT64_MAX-(uint64_t)count*4096||
       (map==kernel_pmap?access!=0:(!access||(access&~3u))))return 0;
    uint64_t mair;
    __asm__ volatile("mrs %0,mair_el1":"=r"(mair));
    /* Normal memory, Inner NC / Outer NC. AttrIndx1 alone is insufficient if
     * the actual CPU MAIR was configured differently. Never accept Device. */
    if(((mair>>(CACHE_ATTRINDX_WRITECOMB*8))&255)!=0x44)return 0;
    const uint8_t *pages=(const uint8_t *)metadata;
    for(uint32_t first=0;first<count;first+=4) {
        uint64_t pa=*(const uint64_t *)(pages+(size_t)first*stride);
        if(!pa||(pa&16383)||pa>=(UINT64_C(1)<<40)||!pa_valid(pa))return 0;
        /* A single 16KiB CPU PTE must agree with every constituent GPU4K
         * page. Never certify an unrelated expected physical-page sequence. */
        for(uint32_t i=1;i<4;i++)if(*(const uint64_t *)(pages+(size_t)(first+i)*stride)!=
            pa+(uint64_t)i*4096)return 0;
        if((pmap_cache_attributes((ppnum_t)(pa>>PAGE_SHIFT))&VM_WIMG_MASK)!=VM_WIMG_WCOMB)
            return 0;
        if(!dre_gpu_nc_leaf(map,address+(uint64_t)first*4096,pa,access))return 0;
        /* Actual managed physical aperture must also be Normal-NC. The
         * selected XNU pmap_update_cache_attributes_locked updates this PTE
         * and all managed PV aliases. A WB direct-map alias is rejected. */
        if(!dre_gpu_nc_leaf(kernel_pmap,phystokv(pa),pa,0))return 0;
    }
    return 1;
}
#endif

int
dre_gpu_pmap_verify_kernel_nc(uint64_t address,const void *pages,uint32_t count,uint32_t stride)
{
#if !XNU_MONITOR && __ARM_PTE_PHYSMAP__
    return dre_gpu_nc_verify(kernel_pmap,address,pages,count,stride,0);
#else
    /* Protected pmap access / unknown physical-aperture scheme has no proof
     * in this implementation. Refuse rather than assume uncached aliases. */
    (void)address;(void)pages;(void)count;(void)stride;return 0;
#endif
}
int
dre_gpu_pmap_verify_task_nc(task_t task,uint64_t address,const void *pages,
    uint32_t count,uint32_t stride,uint32_t access)
{
#if !XNU_MONITOR && __ARM_PTE_PHYSMAP__
    if(!task||task==kernel_task)return 0;
    /* A task reference alone does not pin its current vm_map across an exec.
     * Hold an actual map reference while selecting and inspecting its pmap. */
    vm_map_t map=get_task_map_reference(task);
    if(map==VM_MAP_NULL)return 0;
    pmap_t pmap=vm_map_pmap(map);
    int result=pmap!=kernel_pmap&&dre_gpu_nc_verify(pmap,address,pages,count,stride,access);
    vm_map_deallocate(map);return result;
#else
    (void)task;(void)address;(void)pages;(void)count;(void)stride;(void)access;return 0;
#endif
}
#endif /* DRE */
