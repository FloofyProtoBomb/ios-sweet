/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Private SMMUv2 stage1 descriptor arithmetic, not publication.
 */
#ifndef DRE_TASKVM_PERMISSIONS_H
#define DRE_TASKVM_PERMISSIONS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum dre_taskvm_privilege { DRE_TASKVM_PRIVILEGED_ONLY=0,DRE_TASKVM_USER_ACCESSIBLE=1 };
/* Pure encoding only. PA ownership, actual MAIR0=Normal-NC, transaction PRIV/
 * instruction attributes, context/security and cache/TLB lifetimes must be
 * established separately. This function can neither map nor protect memory.
 * Zero success; no write+instruction-fetch leaf is accepted. */
int dre_taskvm_encode_leaf(uint64_t pa,enum dre_taskvm_privilege,
                           int writable,int instruction_fetch,uint64_t *leaf);
#ifdef __cplusplus
}
#endif
#endif
