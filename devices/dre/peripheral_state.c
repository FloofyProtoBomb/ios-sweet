/* Read-only readiness checks for the Holi USB, touch bus and Adreno blocks.
 * Register offsets are from the matching published OnePlus/LineageOS driver.
 * Each peripheral aperture is accessed only after its clock/power checks. */
#include <libfdt.h>
#include "peripheral_state.h"

extern int dre_read32(uintptr_t addr, uint32_t *out);
#define GCC UINT64_C(0x01400000)
#define POWER_ON (1u << 31)

static int reg_is_raw(const void *dt, const char *path, const char *compat, uint32_t base, uint32_t size)
{
    int node = fdt_path_offset(dt, path), len;
    const fdt32_t *p = fdt_getprop(dt, node, "reg", &len);
    return p && len >= 8 && !fdt_node_check_compatible(dt, node, compat) &&
        fdt32_to_cpu(p[0]) == base && fdt32_to_cpu(p[1]) == size;
}

static int reg_is(const void *dt, const char *path, const char *compat, uint32_t base, uint32_t size)
{
    int soc = fdt_path_offset(dt, "/soc"), len;
    const fdt32_t *ranges = fdt_getprop(dt, soc, "ranges", &len);
    return fdt_address_cells(dt, 0) == 2 && fdt_address_cells(dt, soc) == 1 &&
        fdt_size_cells(dt, soc) == 1 && ranges && len == 16 &&
        !ranges[0] && !ranges[1] && !ranges[2] &&
        (uint64_t)fdt32_to_cpu(ranges[3]) >= (uint64_t)base + size &&
        reg_is_raw(dt, "/soc/clock-controller@1400000", "qcom,holi-gcc", 0x01400000, 0x1f0000) &&
        reg_is_raw(dt, path, compat, base, size);
}

static int clock_ready(uint32_t value, int hwcg)
{
    return (value & 1) && (!(value & POWER_ON) || (hwcg && (value & 2)));
}

const char *dre_usb_state(const void *dt, struct dre_usb_state *s)
{
    if (!reg_is(dt, "/soc/ssusb@4e00000/dwc3@4e00000", "snps,dwc3", 0x04e00000, 0xcd00))
        return "DT_MISMATCH";
    if (!dre_read32(GCC + 0x1a004, &s->power) ||
        !dre_read32(GCC + 0x1a010, &s->master) ||
        !dre_read32(GCC + 0x1a084, &s->config)) return "CLOCK_READ_FAULT";
    if (!(s->power & POWER_ON)) return "POWER_OFF";
    if (!clock_ready(s->master, 0) || !clock_ready(s->config, 1)) return "CLOCK_OFF";
    if (!dre_read32(0x04e0c120, &s->id) || !dre_read32(0x04e0c110, &s->control))
        return "DWC_READ_FAULT";
    if ((s->id >> 16) != 0x5533) return "NO_DWC3";
    return ((s->control >> 12) & 3) == 2 ? "DWC3_DEVICE" : "DWC3_OTHER_MODE";
}

const char *dre_touch_state(const void *dt, struct dre_touch_state *s)
{
    if (!reg_is(dt, "/soc/spi@4a80000", "qcom,spi-geni", 0x04a80000, 0x4000))
        return "DT_MISMATCH";
    uint32_t master, slave, serial;
    if (!dre_read32(GCC + 0x7900c, &s->votes) || !dre_read32(GCC + 0x1f004, &master) ||
        !dre_read32(GCC + 0x1f008, &slave) || !dre_read32(GCC + 0x1f144, &serial))
        return "CLOCK_READ_FAULT";
    if ((s->votes & 0x4c0) != 0x4c0 || (master & POWER_ON) ||
        (slave & POWER_ON) || (serial & POWER_ON)) return "CLOCK_OFF";
    if (!dre_read32(0x04a80068, &s->revision) || !dre_read32(0x04a80040, &s->status))
        return "GENI_READ_FAULT";
    return ((s->revision >> 8) & 0xff) == 1 ? "SPI_FW_READY" : "SPI_FW_MISSING";
}

const char *dre_gpu_state(const void *dt, struct dre_gpu_state *s)
{
    if (!reg_is(dt, "/soc/clock-controller@5990000", "qcom,holi-gpucc", 0x05990000, 0x9000))
        return "DT_MISMATCH";
    uint32_t cfg, gfx;
    if (!dre_read32(GCC + 0x36004, &cfg)) return "CLOCK_READ_FAULT";
    if (!clock_ready(cfg, 1)) return "CONFIG_CLOCK_OFF";
    if (!dre_read32(0x0599106c, &s->cx) || !dre_read32(0x0599100c, &s->gx))
        return "POWER_READ_FAULT";
    if (!(s->cx & POWER_ON) || !(s->gx & POWER_ON)) return "POWER_OFF";
    if (!dre_read32(0x05991054, &gfx)) return "CLOCK_READ_FAULT";
    if (!clock_ready(gfx, 0)) return "GFX_CLOCK_OFF";
    /* A6XX register numbers are DWORD indices. RBBM_STATUS is 0x210. */
    if (!dre_read32(0x05900000 + 0x210 * 4, &s->status)) return "RBBM_READ_FAULT";
    return "RBBM_ACCESSIBLE";
}
