/* SPDX-License-Identifier: MIT
 * GPI offsets and EE adjustment: matching gpi.c / msm_gpi_mmio.h. */
#include <libfdt.h>
#include "gpi_preflight.h"
#include "dma_proof.h"
#include "touch/spi_plan.h"
extern int dre_read32(uintptr_t address, uint32_t *out);
static volatile uint8_t spi_arena[8192] __attribute__((aligned(4096)));
static int cell(const void *dt, int node, const char *name, uint32_t expected)
{
    int n; const fdt32_t *p = fdt_getprop(dt, node, name, &n);
    return p && n == 4 && fdt32_to_cpu(*p) == expected;
}
const char *dre_gpi_preflight(const void *dt, struct dre_gpi_preflight *s)
{
    *s = (struct dre_gpi_preflight){.dma_state = "SKIP", .map_state = "SKIP", .spi_plan = "SKIP"};
    int spi = fdt_path_offset(dt, "/soc/spi@4a80000"), n;
    const fdt32_t *dma = fdt_getprop(dt, spi, "dmas", &n);
    if (!dma || n != 48) return "DT_MISMATCH";
    int gpi = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(dma[0]));
    const fdt32_t *reg = fdt_getprop(dt, gpi, "reg", &n);
    if (!reg || n != 8 || fdt32_to_cpu(reg[0]) != 0x04a00000 || fdt32_to_cpu(reg[1]) != 0x60000 ||
        fdt_node_check_compatible(dt, gpi, "qcom,gpi-dma") ||
        !cell(dt, gpi, "#dma-cells", 5) || !cell(dt, gpi, "qcom,max-num-gpii", 10) ||
        !cell(dt, gpi, "qcom,gpii-mask", 31) || !cell(dt, gpi, "qcom,gpi-ee-offset", 0x10000))
        return "DT_MISMATCH";
    for (unsigned ch = 0; ch < 2; ch++) {
        const fdt32_t *p = dma + ch * 6;
        if (p[0] != dma[0] || fdt32_to_cpu(p[1]) != ch || p[2] ||
            fdt32_to_cpu(p[3]) != 1 || fdt32_to_cpu(p[4]) != 64 || p[5]) return "DT_MISMATCH";
    }
    const char *names = fdt_getprop(dt, spi, "dma-names", &n);
    if (!names || n != 6 || memcmp(names, "tx\0rx\0", 6)) return "DT_MISMATCH";
    /* Linux subtracts qcom,gpi-ee-offset before applying context offsets.
     * Only mask bits 0..4 belong to this AP profile. */
    const uintptr_t ee = 0x049f0000;
    for (unsigned i = 0; i < 5; i++) {
        uint32_t tx, rx, event;
        if (!dre_read32(ee + 0x20000 + i * 0x4000, &tx) ||
            !dre_read32(ee + 0x20080 + i * 0x4000, &rx) ||
            !dre_read32(ee + 0x21000 + i * 0x4000, &event)) return "READ_FAULT";
        tx = (tx >> 20) & 15; rx = (rx >> 20) & 15; event = (event >> 20) & 15;
        if (tx > 4 || rx > 4 || event > 1) return "CHANNEL_ERROR";
        if (!(tx | rx | event)) s->idle_mask |= 1u << i;
        s->states |= (tx | rx | event) << (i * 4);
    }
    struct dre_dma_route route;
    s->dma_state = dre_dma_route(dt, "/soc/qcom,gpi-dma@4a00000", 0x16, &route);
    s->route = route.s2cr;
    struct dre_dma_walk walk;
    s->map_state = dre_dma_proof(dt, &route, "/soc/qcom,gpi-dma@4a00000", 0x16,
                                (uintptr_t)spi_arena, sizeof(spi_arena), &walk);
    uint32_t source, clock;
    if (!dre_read32(0x04a8007c, &source) || !dre_read32(0x04a80048, &clock)) return "CLOCK_READ_FAULT";
    struct dre_spi_plan plan;
    s->spi_plan = (source & ~7u) || !(clock & 1) ? "SPI_CLOCK_UNSET" :
        dre_spi_plan(&plan, (uintptr_t)spi_arena, (uintptr_t)spi_arena + 4096,
                     65, 0, source & 7, (clock >> 4) & 4095);
    return s->idle_mask ? "IDLE" : "BUSY";
}
