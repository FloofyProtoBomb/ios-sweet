/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Exact DE2118 SPI/GPI/Tianma profile, no MMIO or firmware export.
 */
#include "native.h"
#include <libfdt.h>
#include <string.h>

static int one(const void *f, int node, const char *key, uint32_t want)
{
    int n;
    const fdt32_t *p = fdt_getprop(f, node, key, &n);
    return p && n == 4 && fdt32_to_cpu(*p) == want;
}
static int pair(const void *f, int node, const char *key, uint32_t a, uint32_t b)
{
    int n;
    const fdt32_t *p = fdt_getprop(f, node, key, &n);
    return p && n == 8 && fdt32_to_cpu(p[0]) == a && fdt32_to_cpu(p[1]) == b;
}
static int gpio(const void *f, int node, int tlmm, const char *key,
                uint32_t pin, uint32_t flags)
{
    int n;
    const fdt32_t *p = fdt_getprop(f, node, key, &n);
    return p && n == 12 && fdt_node_offset_by_phandle(f, fdt32_to_cpu(p[0])) == tlmm &&
        fdt32_to_cpu(p[1]) == pin && fdt32_to_cpu(p[2]) == flags;
}
static unsigned panel_matches(const char *p, size_t n, const char *panel)
{
    const char key[] = "oplus_bsp_tp_custom.dsi_display0=";
    size_t m = strlen(panel);
    unsigned matches = 0;
    for (size_t i = 0; p && i < n && p[i]; ) {
        if (p[i] == ' ') { ++i; continue; }
        size_t end = i, start = i;
        while (end < n && p[end] && p[end] != ' ') ++end;
        if (end - i >= sizeof(key) - 1 && !memcmp(p + i, key, sizeof(key) - 1)) {
            start = end;
            for (size_t j = i + sizeof(key) - 1; j < end; ++j)
                if (p[j] == ':') start = j + 1;
        }
        if (end - start == m + 3 && !memcmp(p + start, "tp_", 3) &&
            !memcmp(p + start + 3, panel, m)) ++matches;
        i = end;
    }
    return matches;
}
enum dre_tn_result dre_tn_profile_from_fdt(const void *f, size_t bytes,
                                          struct dre_tn_profile *out)
{
    if (!f || !out || bytes < sizeof(struct fdt_header) || bytes > (2u << 20) ||
        fdt_check_full(f, bytes) || fdt_totalsize(f) != bytes) return DRE_TN_ARGUMENT;
    int soc = fdt_path_offset(f, "/soc"), n;
    const fdt32_t *p = fdt_getprop(f, soc, "ranges", &n);
    if (fdt_address_cells(f, 0) != 2 || fdt_address_cells(f, soc) != 1 ||
        fdt_size_cells(f, soc) != 1 || !p || n != 16 || p[0] || p[1] || p[2] ||
        fdt32_to_cpu(p[3]) < 0x04a84000) return DRE_TN_PROFILE;
    int spi = fdt_path_offset(f, "/soc/spi@4a80000");
    int touch = fdt_path_offset(f, "/soc/spi@4a80000/novatek@0");
    int gpi = fdt_path_offset(f, "/soc/qcom,gpi-dma@4a00000");
    int tlmm = fdt_path_offset(f, "/soc/pinctrl@400000");
    if (fdt_node_check_compatible(f, spi, "qcom,spi-geni") ||
        fdt_node_check_compatible(f, touch, "novatek,nf_nt36672c") ||
        fdt_node_check_compatible(f, gpi, "qcom,gpi-dma") ||
        fdt_node_check_compatible(f, tlmm, "qcom,holi-pinctrl") ||
        !pair(f, spi, "reg", 0x04a80000, 0x4000) ||
        !pair(f, gpi, "reg", 0x04a00000, 0x60000) ||
        !pair(f, tlmm, "reg", 0x00400000, 0x800000) ||
        !one(f, gpi, "#dma-cells", 5) || !one(f, gpi, "qcom,max-num-gpii", 10) ||
        !one(f, gpi, "qcom,gpii-mask", 31) || !one(f, gpi, "qcom,ev-factor", 2) ||
        !one(f, gpi, "qcom,gpi-ee-offset", 0x10000) ||
        !pair(f, gpi, "qcom,iommu-dma-addr-pool", 0x100000, 0x100000) ||
        !one(f, touch, "reg", 0) || !one(f, touch, "spi-max-frequency", 12000000) ||
        !one(f, touch, "chip-num", 2) || !one(f, touch, "touchpanel,max-num-support", 10) ||
        !pair(f, touch, "touchpanel,panel-coords", 1080, 2400) ||
        !pair(f, touch, "touchpanel,display-coords", 1080, 2400) ||
        !gpio(f, touch, tlmm, "irq-gpio", 22, 0x2002) ||
        !gpio(f, touch, tlmm, "reset-gpio", 21, 0) ||
        !gpio(f, touch, tlmm, "cs-gpio", 3, 0)) return DRE_TN_PROFILE;
    if (fdt_getprop(f, spi, "qcom,shared-se", &n) ||
        fdt_getprop(f, spi, "spi-slave", &n) ||
        fdt_getprop(f, touch, "spi-cpha", &n) ||
        fdt_getprop(f, touch, "spi-cpol", &n) ||
        fdt_getprop(f, touch, "spi-cs-high", &n) ||
        fdt_getprop(f, touch, "spi-3wire", &n)) return DRE_TN_PROFILE;
    p = fdt_getprop(f, gpi, "qcom,static-gpii-mask", &n);
    if (p && (n != 4 || fdt32_to_cpu(*p))) return DRE_TN_PROFILE;
    p = fdt_getprop(f, spi, "dmas", &n);
    if (!p || n != 48) return DRE_TN_PROFILE;
    for (unsigned ch = 0; ch != 2; ++ch) {
        const fdt32_t *q = p + 6 * ch;
        if (fdt_node_offset_by_phandle(f, fdt32_to_cpu(q[0])) != gpi ||
            fdt32_to_cpu(q[1]) != ch || q[2] || fdt32_to_cpu(q[3]) != 1 ||
            fdt32_to_cpu(q[4]) != 64 || q[5]) return DRE_TN_PROFILE;
    }
    const char *names = fdt_getprop(f, spi, "dma-names", &n);
    if (!names || n != 6 || memcmp(names, "tx\0rx\0", 6)) return DRE_TN_PROFILE;
    p = fdt_getprop(f, gpi, "iommus", &n);
    if (!p || n != 12 || fdt32_to_cpu(p[1]) != 0x16 || p[2]) return DRE_TN_PROFILE;
    int smmu = fdt_node_offset_by_phandle(f, fdt32_to_cpu(p[0]));
    if (smmu < 0 || !one(f, smmu, "#iommu-cells", 2)) return DRE_TN_PROFILE;
    p = fdt_getprop(f, spi, "interrupts", &n);
    if (!p || n != 12 || p[0] || fdt32_to_cpu(p[1]) != 327 ||
        fdt32_to_cpu(p[2]) != 4) return DRE_TN_PROFILE;
    p = fdt_getprop(f, gpi, "interrupts", &n);
    if (!p || n != 120) return DRE_TN_PROFILE;
    for (unsigned i = 0; i != 10; ++i)
        if (p[3*i] || fdt32_to_cpu(p[3*i+1]) != 335+i ||
            fdt32_to_cpu(p[3*i+2]) != 4) return DRE_TN_PROFILE;
    int chosen = fdt_path_offset(f, "/chosen");
    const char *args = fdt_getprop(f, chosen, "bootargs", &n);
    if (!args || n <= 0 || args[n-1] ||
        panel_matches(args, (size_t)n, "nt36672c_tm") != 1 ||
        panel_matches(args, (size_t)n, "nt36672c_dsjm")) return DRE_TN_PROFILE;
    int plen;
    const char *selected = fdt_stringlist_get(f, touch,
        "platform_support_project_commandline", 0, &plen);
    if (!selected || plen != 11 || memcmp(selected, "nt36672c_tm", 11))
        return DRE_TN_PROFILE;
    struct dre_tn_profile result = {
        .spi_base = 0x04a80000, .gpi_base = 0x04a00000, .gpi_ee_base = 0x049f0000,
        .iova_base = 0x100000, .iova_size = 0x100000,
        .gpi_irq = {367,368,369,370,371}, .spi_irq = 359, .gpi_mask = 31,
        .event_factor = 2, .stream_id = 0x16, .irq_gpio = 22, .reset_gpio = 21,
        .cs_gpio = 3, .width = 1080, .height = 2400, .max_hz = 12000000,
        .firmware_size = 139264,
        .firmware_sha256 = {0xe4,0x4e,0x4e,0xa6,0xd7,0xd1,0xdd,0x23,
            0xb1,0x0e,0xe2,0x6a,0x31,0xb5,0x0a,0x39,0xcf,0x11,0x1f,0x61,
            0x8a,0x75,0xea,0x5b,0x23,0x35,0xf6,0xe2,0xca,0x48,0x9a,0xda}
    };
    *out = result;
    return DRE_TN_OK;
}
