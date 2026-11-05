/* SPDX-License-Identifier: MIT */
#pragma once
#include "a619_backend.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_A619_SMMU_BASE UINT64_C(0x05940000)
#define DRE_A619_SMMU_POWERED (1u<<0)
#define DRE_A619_SMMU_QUIESCENT (1u<<1)
#define DRE_A619_SMMU_CB_OWNED (1u<<2)
#define DRE_A619_SMMU_TABLES_NC (1u<<3)
#define DRE_A619_SMMU_GLOBAL_READY (1u<<4)
#define DRE_A619_SMMU_ASID_OWNED (1u<<5)
#define DRE_A619_SMMU_PREREQUISITES 63u
#define DRE_A619_SMMU_READ 1u
#define DRE_A619_SMMU_WRITE 2u
#define DRE_A619_SMMU_EXECUTE 4u
struct dre_a619_smmu_page { uint64_t pa; uint32_t permissions; };
struct dre_a619_smmu_ops {
    int (*guard)(void *cookie);
    int (*owns_ram)(void *cookie,uint64_t pa,size_t bytes);
    int (*read32)(void *cookie,uint64_t address,uint32_t *value);
    int (*write32)(void *cookie,uint64_t address,uint32_t value);
    int (*read64)(void *cookie,uint64_t address,uint64_t *value);
    int (*write64)(void *cookie,uint64_t address,uint64_t value);
    void (*barrier)(void *cookie);
    uint64_t (*now)(void *cookie);
    void *cookie;
};
struct dre_a619_smmu {
    uint32_t chip_id, prerequisites, tick_hz, context_bank, smr_index, asid;
    uint64_t va_base, table_pa;
    uint64_t *tables; /* three contiguous 4KiB pages, exclusively owned Normal-NC */
    const struct dre_a619_smmu_page *pages;
    uint32_t page_count;
    uint64_t cb_base, saved_ttbr[2];
    uint32_t owned_context_bank, owned_smr_index, owned_asid;
    uint32_t saved32[8], saved_smr, saved_s2cr;
    int active, changed, unsafe;
};
/* A restricted SID0-only 39-bit/4KiB stage1 domain. It constructs actual
 * translation tables and programs an exclusively owned disabled context bank.
 * No global CR0, global implementation register, other SMR/CB, or SID2 write.
 * Platform power, quiescent GPU, NC tables, global initialization and ASID/CB
 * reservation must be real providers, not DT claims. Live hardware, global
 * implementation state, SID0 matching, CB sharing and faults are also checked.
 * On unsafe failure retain all allocations and reset; never resume GPU DMA.
 * On normal release caller keeps GPU quiescent until power-down/reassignment.
 * This does not initialize the GPU, create Metal APIs, or prove native DMA. */
const char *dre_a619_smmu_activate(struct dre_a619_smmu *state,
    const struct dre_a619_smmu_ops *ops);
const char *dre_a619_smmu_release(struct dre_a619_smmu *state,
    const struct dre_a619_smmu_ops *ops);
#ifdef __cplusplus
}
#endif
