/* SPDX-License-Identifier: MIT
 * Holi register facts: gcc-holi.c, pinctrl-holi.c and msm-geni-se.h at
 * kernel 98a38783f42f5588bf00cd933e299aedc589bc10. */
#include <libfdt.h>
#include "touch_preflight.h"
#include "touch/firmware.h"

extern int dre_read32(uintptr_t address, uint32_t *out);
extern int dre_write32(uintptr_t address, uint32_t value);
#define VOTES UINT64_C(0x0147900c)
#define MASK 0x4c0u
#define GENI UINT64_C(0x04a80000)
#define OFF (1u << 31)

static uint64_t ticks(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static int resource(const void *dt, const char *path, const char *compat, uint32_t base, uint32_t size)
{
    int node = fdt_path_offset(dt, path), n;
    const fdt32_t *p = fdt_getprop(dt, node, "reg", &n);
    return p && n >= 8 && fdt32_to_cpu(p[0]) == base && fdt32_to_cpu(p[1]) == size &&
        !fdt_node_check_compatible(dt, node, compat);
}

static int panel_token(const char *args, int length, const char *suffix)
{
    /* ABL selects the panel in oplus_bsp_tp_custom.dsi_display0=<display>:tp_...
     * Accept that complete suffix (or an explicit standalone tp_... token).
     * Do not match text embedded in an unrelated boot-argument value. */
    unsigned n = strlen(suffix);
    const char key[] = "oplus_bsp_tp_custom.dsi_display0=";
    for (int i = 0; args && i < length && args[i]; ) {
        if (args[i] == ' ') { i++; continue; }
        int end = i;
        while (end < length && args[end] && args[end] != ' ') end++;
        int start = i;
        if (end - i >= (int)sizeof(key) - 1 && !memcmp(args + i, key, sizeof(key) - 1)) {
            start = end;
            for (int j = i + sizeof(key) - 1; j < end; j++) if (args[j] == ':') start = j + 1;
        }
        if (end - start == 3 + (int)n && !memcmp(args + start, "tp_", 3) &&
            !memcmp(args + start + 3, suffix, n)) return 1;
        i = end;
    }
    return 0;
}

static const char *firmware(const void *dt, struct dre_touch_preflight *s)
{
    int node = fdt_path_offset(dt, "/soc/spi@4a80000/novatek@0"), n;
    if (fdt_node_check_compatible(dt, node, "novatek,nf_nt36672c")) return "DT_MISMATCH";
    int chosen = fdt_path_offset(dt, "/chosen"), length;
    const char *args = fdt_getprop(dt, chosen, "bootargs", &length);
    const fdt32_t *count = fdt_getprop(dt, node, "chip-num", &n);
    if (!count || n != 4 || fdt32_to_cpu(*count) != 2) return "PANEL_COUNT";
    unsigned selected = 0, matches = 0;
    for (unsigned i = 0; i < 2; i++) {
        const char *panel = fdt_stringlist_get(dt, node, "platform_support_project_commandline", i, &n);
        if (!panel || n < 1 || n > 63) return "PANEL_LIST";
        if (panel_token(args, length, panel)) { matches++; selected = i; }
    }
    if (matches != 1) return "PANEL_UNCONFIRMED";
    const uint8_t *p = fdt_getprop(dt, node, selected ? "firmware-data-1" : "firmware-data-0", &n);
    struct dre_touch_firmware fw;
    const char *error = dre_touch_parse_firmware(p, p ? (unsigned)n : 0, &fw);
    if (error) return error;
    s->firmware_index = selected; s->firmware_bytes = fw.write_length;
    s->firmware_version = fw.version; s->partitions = fw.count;
    return "LAYOUT_OK";
}

const char *dre_touch_preflight(const void *dt, struct dre_touch_preflight *s)
{
    *s = (struct dre_touch_preflight){.safe_to_handoff = 1, .firmware_index = UINT32_MAX};
    s->firmware_state = firmware(dt, s);
    int soc = fdt_path_offset(dt, "/soc"), n;
    const fdt32_t *ranges = fdt_getprop(dt, soc, "ranges", &n);
    if (fdt_address_cells(dt, 0) != 2 || fdt_address_cells(dt, soc) != 1 ||
        fdt_size_cells(dt, soc) != 1 || !ranges || n != 16 || ranges[0] || ranges[1] || ranges[2] ||
        fdt32_to_cpu(ranges[3]) < 0x04a84000 ||
        !resource(dt, "/soc/spi@4a80000", "qcom,spi-geni", 0x04a80000, 0x4000) ||
        !resource(dt, "/soc/clock-controller@1400000", "qcom,holi-gcc", 0x01400000, 0x1f0000) ||
        !resource(dt, "/soc/pinctrl@400000", "qcom,holi-pinctrl", 0x00400000, 0x800000) ||
        fdt_node_check_compatible(dt, fdt_path_offset(dt, "/soc/spi@4a80000/novatek@0"),
                                  "novatek,nf_nt36672c"))
        return "DT_MISMATCH";
    int node = fdt_path_offset(dt, "/soc/spi@4a80000/novatek@0");
    int tlmm = fdt_path_offset(dt, "/soc/pinctrl@400000");
    const fdt32_t *gpio = fdt_getprop(dt, node, "irq-gpio", &n);
    if (!gpio || n != 12 || fdt_node_offset_by_phandle(dt, fdt32_to_cpu(gpio[0])) != tlmm ||
        fdt32_to_cpu(gpio[1]) != 22) return "DT_MISMATCH";
    gpio = fdt_getprop(dt, node, "reset-gpio", &n);
    if (!gpio || n != 12 || fdt_node_offset_by_phandle(dt, fdt32_to_cpu(gpio[0])) != tlmm ||
        fdt32_to_cpu(gpio[1]) != 21) return "DT_MISMATCH";
    if (!dre_read32(VOTES, &s->votes_before)) return "CLOCK_READ_FAULT";
    s->votes_during = s->votes_after = s->votes_before;
    unsigned changed = MASK & ~s->votes_before;
    const char *error = "CLOCK_WRITE_FAULT";
    if (changed) {
        s->safe_to_handoff = 0;
        if (!dre_write32(VOTES, s->votes_before | MASK)) goto restore;
    }
    static const uint32_t halt[] = {0x0141f004, 0x0141f008, 0x0141f144};
    error = "CLOCK_TIMEOUT";
    if (!dre_read32(VOTES, &s->votes_during)) { error = "CLOCK_READ_FAULT"; goto restore; }
    if ((s->votes_during & MASK) != MASK) goto restore;
    for (unsigned i = 0; i < 3; i++) {
        uint64_t start = ticks();
        uint32_t value;
        for (;;) {
            if (!dre_read32(halt[i], &value)) { error = "CLOCK_READ_FAULT"; goto restore; }
            if (!(value & OFF) || (i < 2 && (value & 2))) break;
            if (ticks() - start >= 38400) goto restore;
        }
    }
    error = "GENI_READ_FAULT";
    if (!dre_read32(GENI + 0x68, &s->revision) || !dre_read32(GENI + 0x40, &s->status) ||
        !dre_read32(GENI + 0x64, &s->fifo_disabled) ||
        !dre_read32(GENI + 0xe24, &s->tx_geometry) || !dre_read32(GENI + 0xe28, &s->rx_geometry))
        goto restore;
    error = "PIN_READ_FAULT";
    for (unsigned i = 0; i < 4; i++) {
        uint32_t value;
        if (!dre_read32(0x00500000 + i * 0x1000, &value)) goto restore;
        if (((value >> 2) & 15) == (i < 2 ? 2u : 1u)) s->pin_match |= 1u << i;
    }
    /* Read the IRQ input only; never assert the touch reset pin. */
    if (!dre_read32(0x00516004, &s->irq_level)) goto restore;
    if (((s->revision >> 8) & 255) != 1) error = "SPI_FW_MISSING";
    else if (s->status & 0x1001) error = "SPI_BUSY";
    else if (s->fifo_disabled & 1) error = "SPI_GPI_ONLY";
    else if (((s->tx_geometry >> 24) & 63) != 32 || ((s->rx_geometry >> 24) & 63) != 32 ||
             ((s->tx_geometry >> 16) & 63) < 2 || ((s->rx_geometry >> 16) & 63) < 2)
        error = "SPI_FIFO_UNSUPPORTED";
    else error = "SPI_PIO_READY";
restore:
    if (changed) {
        uint32_t value;
        if (!dre_read32(VOTES, &value) || !dre_write32(VOTES, value & ~changed) ||
            !dre_read32(VOTES, &s->votes_after) ||
            (s->votes_after & MASK) != (s->votes_before & MASK)) return "RESTORE_FAILED";
    }
    s->safe_to_handoff = 1;
    return error;
}
