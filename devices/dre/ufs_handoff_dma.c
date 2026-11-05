/* SPDX-License-Identifier: MIT
 * ARM IHI0062D.c sections 2.3 / 16.5: stage-1 M=0 uses input addresses,
 * CBAR TYPE=1 bypasses stage 2; SCTLR.M is not cached in TLB entries.
 * No SMMU configuration, TLB operation or fault-clear write is performed. */
#include <libfdt.h>
#include "ufs_handoff_dma.h"
#include "ufs_read.h"
extern int dre_read32(uintptr_t address, uint32_t *out);

int dre_ufs_handoff_valid(void *cookie)
{
    struct dre_ufs_handoff_dma *s = cookie;
    struct dre_dma_route route;
    if (strcmp(dre_dma_route(s->dt, "/soc/ufshc@4804000", 0x60, &route), "TRANSLATED") ||
        memcmp(&route, &s->route, sizeof(route))) return 0;
    struct dre_dma_context context;
    if (strcmp(dre_dma_context(&route, &context), "M_OFF") ||
        memcmp(&context, &s->context, sizeof(context))) return 0;
    unsigned shift = route.id1 & (1u << 31) ? 16 : 12;
    uint64_t span = UINT64_C(1) << (((route.id1 >> 28) & 7) + 1 + shift);
    uintptr_t bank = UINT64_C(0x0c600000) + span + ((uint64_t)context.bank << shift);
    uint32_t fsr;
    /* Ignore FSR's non-fault format bits; reject all active/stalled faults. */
    return dre_read32(bank + 0x58, &fsr) && !(fsr & 0xc00001feu);
}

const char *dre_ufs_handoff_begin(const void *dt, const struct dre_dma_route *route,
                                 struct dre_ufs_handoff_dma *s)
{
    *s = (struct dre_ufs_handoff_dma){.dt = dt, .route = *route};
    const char *mode = dre_dma_context(route, &s->context);
    if (strcmp(mode, "M_OFF")) return mode;
    /* Limit the first implementation to observed normal-memory/default
     * attribute behavior. Do not reinterpret reserved or forced-cache modes. */
    if ((s->context.cbar & 0x3ff00u) != 0x1f000u || (s->context.control & 0x3f100000u))
        return "ATTRIBUTES_UNSUPPORTED";
    if ((uint64_t)dre_ufs_probe_arena() + 8191 >= (UINT64_C(1) << 32)) return "ARENA_WIDTH";
    return dre_ufs_handoff_valid(s) ? "HANDOFF_BYPASS" : "STATE_OR_FAULT_BLOCKED";
}
