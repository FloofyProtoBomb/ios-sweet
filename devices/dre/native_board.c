/* SPDX-License-Identifier: MIT
 * UNVALIDATED source-only integration. Reuses the corrected transient FDT
 * mapping pattern without changing or calling native UFS initialization. */
#include <sys/param.h>
#include <sys/errno.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <vm/pmap.h>
#include <vm/memory_types.h>
#include "native_board.h"
#include "storage_abi.h"
#include "storage_live.h"

extern int dre_ufs_get_handoff(uint64_t words[8]);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,unsigned int);
extern void ml_io_unmap(vm_offset_t,vm_size_t);
extern uint64_t kvtophys(vm_offset_t);

static LCK_GRP_DECLARE(dre_native_board_group,"dre_native_board");
static LCK_MTX_DECLARE(dre_native_board_lock,&dre_native_board_group);
static const void *board_fdt;
static size_t board_bytes;

static int holi_board(const void *dt)
{
    int bytes=0,found=0;
    const fdt32_t *ids=fdt_getprop(dt,0,"oplus,dtsi_no",&bytes);
    if(fdt_node_check_compatible(dt,0,"qcom,holi") || !ids || bytes<4 || bytes%4)
        return 0;
    for(int i=0;i<bytes/4;i++) {
        uint32_t value=fdt32_to_cpu(ids[i]);
        if(value==133144 || value==133180)found=1;
    }
    return found;
}

int dre_native_board_snapshot(const void **view,size_t *bytes)
{
    if(!view || !bytes)return EINVAL;
    *view=NULL;*bytes=0;
    lck_mtx_lock(&dre_native_board_lock);
    if(board_fdt) {
        *view=board_fdt;*bytes=board_bytes;
        lck_mtx_unlock(&dre_native_board_lock);
        return 0;
    }
    uint64_t words[8],frequency;
    __asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
    if(frequency!=19200000 || !dre_ufs_get_handoff(words) ||
       !dre_storage_abi_valid(words) || words[4]<40 || words[4]>(2u<<20)) {
        lck_mtx_unlock(&dre_native_board_lock);
        return ENXIO;
    }
    /* The removable mapping is intentional. ml_io_map() creates permanent
     * io_submap entries and must not be used with this transient unmap path. */
    vm_offset_t mapping=ml_io_map_unmappable(words[3],words[4],VM_WIMG_IO);
    if(!mapping) {
        lck_mtx_unlock(&dre_native_board_lock);
        return ENOMEM;
    }
    vm_offset_t map_base=trunc_page(mapping);
    vm_size_t map_bytes=round_page(words[4]+(mapping&PAGE_MASK));
    if(kvtophys(mapping)!=words[3] ||
       kvtophys(mapping+words[4]-1)!=words[3]+words[4]-1) {
        ml_io_unmap(map_base,map_bytes);
        lck_mtx_unlock(&dre_native_board_lock);
        return ENXIO;
    }
    void *copy=kalloc_data(words[4],Z_WAITOK|Z_ZERO);
    if(!copy) {
        ml_io_unmap(map_base,map_bytes);
        lck_mtx_unlock(&dre_native_board_lock);
        return ENOMEM;
    }
    /* Device mappings require byte loads, not vector/unaligned libc copies. */
    for(size_t i=0;i<words[4];i++)((uint8_t *)copy)[i]=((volatile uint8_t *)mapping)[i];
    __asm__ volatile("dsb sy":::"memory");
    ml_io_unmap(map_base,map_bytes);
    if(fdt_check_full(copy,words[4]) || fdt_totalsize(copy)!=words[4] ||
       !holi_board(copy) || !dre_storage_live_valid(copy,words)) {
        bzero(copy,words[4]);
        kfree_data(copy,words[4]);
        lck_mtx_unlock(&dre_native_board_lock);
        return ENXIO;
    }
    /* A successful view is held for the boot lifetime. All callers receive
     * the same snapshot; no caller owns a free or mutation operation. */
    board_fdt=copy;board_bytes=(size_t)words[4];
    *view=board_fdt;*bytes=board_bytes;
    lck_mtx_unlock(&dre_native_board_lock);
    return 0;
}
