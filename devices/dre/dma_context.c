/* SPDX-License-Identifier: MIT
 * SMMUv2 register-layout facts: published Holi arm-smmu.c/arm-smmu.h at
 * kernel 98a38783f42f5588bf00cd933e299aedc589bc10. */
#include "dma_context.h"

extern int dre_read32(uintptr_t address, uint32_t *out);
#define BASE UINT64_C(0x0c600000)

const char *dre_dma_context(const struct dre_dma_route *r, struct dre_dma_context *s)
{
    *s = (struct dre_dma_context){0};
    if (!r->translated || r->matches != 1 || r->physical_dma || (r->s2cr & (3u << 16))) return "SKIP";
    unsigned pgshift = r->id1 & (1u << 31) ? 16 : 12;
    uint64_t span = UINT64_C(1) << (((r->id1 >> 28) & 7) + 1 + pgshift);
    s->bank = r->s2cr & 255;
    /* The validated Holi resource is 1 MiB, half global / half context.
     * Refuse a different ID1 layout rather than guessing context offsets. */
    if (span != 0x80000 || s->bank >= (r->id1 & 255) ||
        ((uint64_t)s->bank << pgshift) + 0x34 > span) return "LAYOUT";
    uintptr_t global = BASE + (UINT64_C(1) << pgshift);
    uintptr_t context = BASE + span + ((uint64_t)s->bank << pgshift);
    uint32_t high;
    if (!dre_read32(global + s->bank * 4, &s->cbar) ||
        !dre_read32(global + 0x800 + s->bank * 4, &s->cba2r) ||
        !dre_read32(context, &s->control) || !dre_read32(context + 0x30, &s->tcr) ||
        !dre_read32(context + 0x10, &s->tcr2) ||
        !dre_read32(context + 0x24, &s->ttbr_hi) ||
        !dre_read32(context + 0x20, &s->ttbr_lo) ||
        !dre_read32(context + 0x24, &high)) return "FAULT";
    if (high != s->ttbr_hi) return "CHANGED";
    s->captured = 1;
    if (s->control == UINT32_MAX || s->tcr == UINT32_MAX || s->cbar == UINT32_MAX)
        return "BAD_REG";
    if (((s->cbar >> 16) & 3) == 0) return "S2";
    if (((s->cbar >> 16) & 3) != 1) return "COMBINED";
    if (!(s->control & 1)) return "M_OFF";
    if (s->control & 16) return "BIG_ENDIAN";
    return s->cba2r & 1 ? "S1_64" : "S1_32";
}
