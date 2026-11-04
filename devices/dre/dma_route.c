/* SPDX-License-Identifier: MIT
 * ARM SMMUv2 stream-routing facts from the matching published Holi driver.
 * This is a bounded, read-only query, not an IOMMU configuration driver. */
#include <libfdt.h>
#include "dma_route.h"

extern int dre_read32(uintptr_t address, uint32_t *out);
#define BASE UINT64_C(0x0c600000)

const char *dre_dma_route(const void *dt, const char *device_path,
                          uint32_t sid, struct dre_dma_route *s)
{
    *s = (struct dre_dma_route){0};
    s->slot = UINT32_MAX;
    int dev = fdt_path_offset(dt, device_path), len;
    const fdt32_t *iommu = fdt_getprop(dt, dev, "iommus", &len);
    if (!iommu || len != 12 || fdt32_to_cpu(iommu[1]) != sid || iommu[2])
        return "DT_MISMATCH";
    int node = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(iommu[0]));
    const fdt32_t *reg = fdt_getprop(dt, node, "reg", &len);
    if (!reg || len < 8 || fdt32_to_cpu(reg[0]) != BASE ||
        fdt32_to_cpu(reg[1]) != 0x100000 ||
        fdt_node_check_compatible(dt, node, "qcom,qsmmu-v500")) return "DT_MISMATCH";
    const fdt32_t *cells = fdt_getprop(dt, node, "#iommu-cells", &len);
    if (!cells || len != 4 || fdt32_to_cpu(*cells) != 2) return "DT_MISMATCH";
    int soc = fdt_path_offset(dt, "/soc");
    const fdt32_t *ranges = fdt_getprop(dt, soc, "ranges", &len);
    if (fdt_address_cells(dt, 0) != 2 || fdt_address_cells(dt, soc) != 1 ||
        fdt_size_cells(dt, soc) != 1 || !ranges || len != 16 ||
        ranges[0] || ranges[1] || ranges[2] || fdt32_to_cpu(ranges[3]) < BASE + 0x100000)
        return "DT_MISMATCH";
    if (!dre_read32(BASE + 0x20, &s->id0) || !dre_read32(BASE + 0x24, &s->id1) ||
        !dre_read32(BASE, &s->control)) return "READ_FAULT";
    if (!s->id0 || s->id0 == UINT32_MAX || s->id1 == UINT32_MAX ||
        !(s->id0 & ((1u << 30) | (1u << 29)))) return "BAD_ID";
    /* Refuse a disabled client interface, including uncertain bypass semantics. */
    if (s->control & 1) return "CLIENT_DISABLED";
    if (!(s->id0 & (1u << 27))) return "INDEXED_UNSUPPORTED";
    unsigned groups = s->id0 & 255;
    if (!groups || groups > 128) return "GROUPS_UNSUPPORTED";
    int extended = !!((s->id0 & (1u << 8)) && (s->control & (1u << 3)));
    unsigned sid_bits = extended ? 16 : ((s->id0 >> 9) & 15);
    if (!sid_bits || sid >= (1u << sid_bits)) return "SID_UNSUPPORTED";
    for (unsigned i = 0; i < groups; i++) {
        uint32_t smr, route;
        if (!dre_read32(BASE + 0x800 + i * 4, &smr) ||
            !dre_read32(BASE + 0xc00 + i * 4, &route)) return "READ_FAULT";
        if (!(extended ? (route & (1u << 10)) : (smr & (1u << 31)))) continue;
        uint32_t mask = (smr >> 16) & (extended ? 0xffff : 0x7fff);
        uint32_t stream = smr & (extended ? 0xffff : 0x7fff);
        if ((sid ^ stream) & ~mask) continue;
        s->matches++;
        s->slot = i; s->smr = smr; s->s2cr = route;
    }
    if (s->matches > 1) return "AMBIGUOUS";
    if (!s->matches) {
        s->physical_dma = !(s->control & (1u << 10));
        return s->physical_dma ? "UNMATCHED_BYPASS" : "UNMATCHED_FAULT";
    }
    switch ((s->s2cr >> 16) & 3) {
    case 0: return "TRANSLATED";
    case 1: s->physical_dma = 1; return "BYPASS";
    case 2: return "FAULT";
    default: return "RESERVED";
    }
}
