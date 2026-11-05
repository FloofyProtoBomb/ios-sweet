/* SPDX-License-Identifier: MIT
 * Compile-only layout record for read-only, paused QEMU memory inspection.
 * It emits constants; it is never linked into or executed by the kernel. */
#include <stdint.h>
#define OFF(t,f) __builtin_offsetof(t,f)
#if DRE_STACK_LAYOUT_BSD
#ifndef BSD_KERNEL_PRIVATE
#define BSD_KERNEL_PRIVATE 1
#endif
#include <sys/proc_internal.h>
const uint64_t dre_saved_stack_layout[] = {
    sizeof(struct proc), OFF(struct proc,p_list), OFF(struct proc,p_proc_ro),
    OFF(struct proc,p_pid), OFF(struct proc,p_comm), sizeof(((struct proc *)0)->p_comm),
    OFF(struct proc,p_name), sizeof(((struct proc *)0)->p_name), OFF(struct proc_ro,pr_task)
};
#else
#ifndef MACH_KERNEL_PRIVATE
#define MACH_KERNEL_PRIVATE 1
#endif
#include <kern/task.h>
#include <kern/thread.h>
#include <vm/vm_map.h>
#include <vm/vm_shared_region.h>
#include <arm/pmap/pmap_internal.h>
#include <mach/arm/thread_status.h>
#if ARM_PARAMETERIZED_PMAP
#define PMAP_ATTR OFF(struct pmap,pmap_pt_attr)
#else
#define PMAP_ATTR UINT64_MAX
#endif
#if defined(HAS_APPLE_PAC)
#define WITH_PAC 1
#else
#define WITH_PAC 0
#endif
/* Keep labels in build_saved_stack_layout.py in precisely this order. */
const uint64_t dre_saved_stack_layout[] = {
    UINT64_C(0x445245535441434b), 2, WITH_PAC,
    sizeof(struct task), OFF(struct task,map),
    OFF(struct task,threads), OFF(struct task,thread_count),
    OFF(struct task,mach_header_vm_address), OFF(struct task,shared_region),
    OFF(struct vm_shared_region,sr_slide),
    sizeof(struct thread), OFF(struct thread,task_threads),
    OFF(struct thread,machine)+OFF(struct machine_thread,upcb),
    OFF(struct thread,thread_id), OFF(struct thread,state),
    OFF(struct thread,wait_event), OFF(struct thread,continuation),
    OFF(struct _vm_map,pmap), OFF(struct pmap,ttep), PMAP_ATTR,
    OFF(struct page_table_attr,pta_level_info), OFF(struct page_table_attr,pta_root_level),
    OFF(struct page_table_attr,pta_max_level), OFF(struct page_table_attr,pta_page_shift),
    sizeof(struct page_table_level_info), OFF(struct page_table_level_info,shift),
    OFF(struct page_table_level_info,index_mask), OFF(struct page_table_level_info,offmask),
    OFF(struct page_table_level_info,valid_mask), OFF(struct page_table_level_info,type_mask),
    OFF(struct page_table_level_info,type_block),
    sizeof(arm_saved_state_t), OFF(arm_saved_state_t,ash.flavor), ARM_SAVED_STATE64,
    OFF(arm_saved_state_t,ss_64.pc), OFF(arm_saved_state_t,ss_64.lr),
    OFF(arm_saved_state_t,ss_64.sp), OFF(arm_saved_state_t,ss_64.fp),
    OFF(arm_saved_state_t,ss_64.cpsr), OFF(arm_saved_state_t,ss_64.far),
    OFF(arm_saved_state_t,ss_64.esr), OFF(arm_saved_state_t,ss_64.exception),
    ARM_TT_ROOT_SHIFT, ARM_TT_ROOT_INDEX_MASK, ARM_TT_ROOT_OFFMASK,
    ARM_TT_L2_SHIFT, ARM_TT_L2_INDEX_MASK, ARM_TT_L2_OFFMASK,
    ARM_TT_L3_SHIFT, ARM_TT_L3_INDEX_MASK, ARM_TT_L3_OFFMASK,
    ARM_TTE_TABLE_MASK, ARM_PTE_PAGE_MASK
};

#endif /* DRE_STACK_LAYOUT_BSD */
