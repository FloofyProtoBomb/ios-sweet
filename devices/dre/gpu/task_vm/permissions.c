/* SPDX-License-Identifier: MIT -- UNVALIDATED source only.
 * Pinned io-pgtable-arm.c ARM_LPAE_PTE_AP_* and arm_lpae_prot_to_pte:
 * AP00 privilegedRW; AP10 privilegedRO; AP01 userRW; AP11 userRO.
 * Keep separate from public BO READ/WRITE/EXECUTE/immutable-command semantics.
 */
#include "permissions.h"
int dre_taskvm_encode_leaf(uint64_t pa,enum dre_taskvm_privilege privilege,
                           int writable,int instruction_fetch,uint64_t *out)
{
    if(!out||!pa||(pa&4095)||pa>=(UINT64_C(1)<<40)||
       (privilege!=DRE_TASKVM_PRIVILEGED_ONLY&&privilege!=DRE_TASKVM_USER_ACCESSIBLE)||
       (writable!=0&&writable!=1)||(instruction_fetch!=0&&instruction_fetch!=1)||
       (writable&&instruction_fetch))return -1;
    /* Nonsecure, AF, outer-shareable, nG, MAIR index0, 4KiB page descriptor.
     * Actual table geometry/MAIR agreement is not inferred by encoding it. */
    uint64_t descriptor=pa|UINT64_C(0xe23);
    if(privilege==DRE_TASKVM_USER_ACCESSIBLE)descriptor|=UINT64_C(0x40);
    if(!writable)descriptor|=UINT64_C(0x80);
    if(!instruction_fetch)descriptor|=UINT64_C(3)<<53;
    *out=descriptor;return 0;
}
