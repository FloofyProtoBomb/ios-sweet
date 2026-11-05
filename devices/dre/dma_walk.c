/* SPDX-License-Identifier: MIT
 * AArch64 descriptor facts: matching Holi io-pgtable-arm.c and arm-smmu.h.
 * This is a bounded table inspection routine, never a DMA admission gate. */
#include "dma_walk.h"

#define PA_MASK UINT64_C(0x0000fffffffff000)

const char *dre_dma_walk(const struct dre_dma_context *c, uint64_t iova,
                         uint64_t size, dre_table_read read, void *cookie,
                         struct dre_dma_walk *s)
{
    *s = (struct dre_dma_walk){0};
    if (!c->captured || ((c->cbar >> 16) & 3) != 1 || !(c->cba2r & 1) ||
        !(c->control & 1) || (c->control & 16)) return "CONTEXT_UNSUPPORTED";
    if (((c->tcr >> 14) & 3) || (c->tcr & 128)) return "TCR_UNSUPPORTED";
    unsigned tsz = c->tcr & 63, ips = c->tcr2 & 7;
    if (tsz < 16 || tsz > 39 || ips > 5) return "TCR_UNSUPPORTED";
    unsigned bits = 64 - tsz, levels = (bits - 12 + 8) / 9, start = 4 - levels;
    static const unsigned pa_bits[] = {32, 36, 40, 42, 44, 48};
    uint64_t pa_limit = UINT64_C(1) << pa_bits[ips];
    uint64_t ttbr = ((uint64_t)c->ttbr_hi << 32) | c->ttbr_lo;
    if ((ttbr & 4095) || (ttbr & PA_MASK) >= pa_limit) return "ROOT_UNSUPPORTED";
    uint64_t root = ttbr & PA_MASK;
    if (!root) return "ROOT_UNSUPPORTED";
    if (!size || size > 65536 || size - 1 > UINT64_MAX - iova ||
        iova + size - 1 >= (UINT64_C(1) << bits) ||
        ((iova & 4095) + size + 4095) / 4096 > 16) return "RANGE_UNSUPPORTED";
    uint64_t last = iova + size - 1;
    for (uint64_t page = iova & ~UINT64_C(4095); page <= last; page += 4096) {
        uint64_t table = root, addresses[4], values[4], physical = 0;
        unsigned count = 0;
        int leaf = 0;
        for (unsigned level = start; level < 4; level++) {
            unsigned shift = 12 + 9 * (3 - level);
            uint64_t address = table + ((page >> shift) & 511) * 8, pte;
            if (!read(cookie, address, &pte)) return "TABLE_UNREADABLE";
            s->descriptors++;
            addresses[count] = address; values[count++] = pte;
            unsigned kind = pte & 3;
            if (!kind) return "UNMAPPED";
            if (pte & UINT64_C(0x000f000000000000)) return "DESCRIPTOR_UNSUPPORTED";
            if (level < 3 && kind == 3) {
                /* APTable denies EL0/write access: do not infer a device's
                 * privilege override. Execute restrictions are irrelevant. */
                if (pte & (UINT64_C(3) << 61)) return "TABLE_PERMISSION";
                table = pte & PA_MASK;
                if (!table || table >= pa_limit) return "TABLE_ADDRESS";
                continue;
            }
            if ((level == 3 && kind != 3) || (level < 3 && (kind != 1 || !level)))
                return "DESCRIPTOR_UNSUPPORTED";
            uint64_t block_mask = (UINT64_C(1) << shift) - 1;
            if ((pte & PA_MASK & block_mask) || !(pte & (UINT64_C(1) << 10)))
                return "LEAF_UNSUPPORTED";
            if ((pte & 192) != 64) return "LEAF_PERMISSION";
            physical = (pte & PA_MASK & ~block_mask) | (page & block_mask);
            if (physical >= pa_limit) return "OUTPUT_ADDRESS";
            leaf = 1;
            break;
        }
        if (!leaf) return "DESCRIPTOR_UNSUPPORTED";
        /* Reject descriptors changed during inspection, not just torn loads. */
        for (unsigned i = 0; i < count; i++) {
            uint64_t again;
            if (!read(cookie, addresses[i], &again)) return "TABLE_UNREADABLE";
            s->descriptors++;
            if (again != values[i]) return "TABLE_CHANGED";
        }
        if (!s->pages) s->first_pa = physical + (iova & 4095);
        s->last_pa = physical + (last - page < 4095 ? last - page : 4095);
        s->pages++;
        if (physical != page) return "MAPPED_OTHER";
    }
    return "IDENTITY_RW";
}
