/* SPDX-License-Identifier: MIT -- UNVALIDATED optional read-only proof.
 * Include at END of actual osfmk/arm/pmap/pmap.c. Only supports independently
 * owned, unmanaged low-DDR full16KiB CPU granules. No source/DT presence alone
 * supplies RAM ownership, PAS exclusivity or adjacent-owner/cache policy. */
#if defined(DRE)
int
dre_gpu_fw_pmap_verify_boot_nc(uint64_t cpu,uint64_t pa,uint64_t bytes)
{
#if !XNU_MONITOR
    if(PAGE_SIZE!=16384||!bytes||bytes>1024*1024||((cpu|pa|bytes)&16383)||
       pa<UINT64_C(0x80000000)||pa>=UINT64_C(0x100000000)||
       bytes>UINT64_C(0x100000000)-pa||cpu>UINT64_MAX-bytes||
       pt_attr_page_size(pmap_get_pt_attr(kernel_pmap))!=16384)return 0;
    uint64_t mair;__asm__ volatile("mrs %0,mair_el1":"=r"(mair));
    if(((mair>>(CACHE_ATTRINDX_WRITECOMB*8))&255)!=0x44)return 0;
    for(uint64_t off=0;off<bytes;off+=16384) {
        /* XNU-managed memory has a separate physical aperture/PV alias
         * contract. This low-DDR helper cannot prove or bypass that contract. */
        if(pa_valid(pa+off))return 0;
        int ok=0;pmap_lock(kernel_pmap,PMAP_LOCK_SHARED);
        pt_entry_t *pointer=pmap_pte(kernel_pmap,cpu+off);
        if(pointer!=PT_ENTRY_NULL) {
            pt_entry_t leaf=__atomic_load_n(pointer,__ATOMIC_RELAXED);
            ok=(leaf&3)==ARM_PTE_TYPE_VALID&&
                (leaf&ARM_PTE_ATTRINDXMASK)==ARM_PTE_ATTRINDX(CACHE_ATTRINDX_WRITECOMB)&&
                (leaf&ARM_PTE_APMASK)==ARM_PTE_AP(AP_RWNA)&&
                (leaf&(ARM_PTE_NX|ARM_PTE_PNX))==(ARM_PTE_NX|ARM_PTE_PNX)&&
                ((uint64_t)pte_to_pa(leaf)&~UINT64_C(16383))==pa+off;
        }
        pmap_unlock(kernel_pmap,PMAP_LOCK_SHARED);
        if(!ok)return 0;
    }
    return 1;
#else
    (void)cpu;(void)pa;(void)bytes;return 0;
#endif
}
#endif
