/* SM4350 boot-handoff snapshot. Peripheral probes are read-only. The
 * optional UFS clock step changes only GCC branch-enable bits. */
#include <libfdt.h>
#include "console.h"
#include "ufs_clock.h"
#include "peripheral_state.h"

/* Returns 0 on a synchronous data abort, 1 on success. */
extern int dre_read32(uintptr_t addr, uint32_t *out);

static char report[496];
static unsigned used;

static void add(const char *s)
{
    while (*s && used < sizeof(report) - 1)
        report[used++] = *s++;
    report[used] = 0;
}

static void hex32(uint32_t word)
{
    char value[9];
    for (int i = 0; i < 8; i++)
        value[i] = "0123456789abcdef"[(word >> ((7 - i) * 4)) & 15];
    value[8] = 0;
    add(value);
}

static int has_reg(const void *dt, int node, uint32_t base, uint32_t size)
{
    int len;
    const fdt32_t *reg = fdt_getprop(dt, node, "reg", &len);
    return reg && len >= 8 && fdt32_to_cpu(reg[0]) == base && fdt32_to_cpu(reg[1]) == size;
}

static void check_ufs(const void *dt)
{
    int host = fdt_path_offset(dt, "/soc/ufshc@4804000");
    int gcc = fdt_path_offset(dt, "/soc/clock-controller@1400000");
    int soc = fdt_path_offset(dt, "/soc"), len;
    const fdt32_t *ranges = fdt_getprop(dt, soc, "ranges", &len);
    if (fdt_address_cells(dt, soc) != 1 || fdt_size_cells(dt, soc) != 1 ||
        !ranges || len != 16 || ranges[0] || ranges[1] || ranges[2] ||
        fdt32_to_cpu(ranges[3]) < 0x04807000 ||
        !has_reg(dt, host, 0x04804000, 0x3000) || !has_reg(dt, gcc, 0x01400000, 0x1f0000) ||
        fdt_node_check_compatible(dt, host, "qcom,ufshc") ||
        fdt_node_check_compatible(dt, gcc, "qcom,holi-gcc")) {
        add("DT_MISMATCH");
        return;
    }
    uint32_t power;
    if (!dre_read32(0x01445004, &power)) {
        add("CLOCK_READ_FAULT");
        return;
    }
    if (!(power & (1u << 31))) { add("POWER_OFF"); return; }
    struct dre_ufs_clock_state clocks = {0};
#ifdef DRE_UFS_CLOCK_ENABLE
    const char *clock_state = dre_ufs_clocks(&clocks, 1);
#else
    const char *clock_state = dre_ufs_clocks(&clocks, 0);
#endif
    if (strcmp(clock_state, "READY") && strcmp(clock_state, "ENABLED")) {
        add("CLOCK_"); add(clock_state);
        goto clock_report;
    }
    uint32_t version, enable, status, busy;
    if (!dre_read32(0x04804008, &version) || !dre_read32(0x04804034, &enable) ||
        !dre_read32(0x04804030, &status) || !dre_read32(0x04804058, &busy)) {
        add("HCI_READ_FAULT");
        goto clock_report;
    }
    if (version == 0 || version == UINT32_MAX || status == UINT32_MAX) add("NO_HCI");
    else if (!(enable & 1)) add("HOST_DISABLED");
    else if (!(status & 1)) add("NO_DEVICE");
    else if (!(status & 2)) add("LIST_NOT_READY");
    else add(busy ? "LINK_BUSY" : "LINK_READY");
    add("|UFS_VER="); hex32(version);
    add("_HCS="); hex32(status);
    add("_DBR="); hex32(busy);
clock_report:
    add("|UCLK_PRE=");
    for (unsigned i = 0; i < DRE_UFS_CLOCKS; i++) {
        if (i) add(",");
        hex32(clocks.before[i]);
    }
    add("|UCLK_NOW=");
    for (unsigned i = 0; i < DRE_UFS_CLOCKS; i++) {
        if (i) add(",");
        hex32(clocks.after[i]);
    }
    add("|ROOT_AXI="); hex32(clocks.axi_cmd); add("/"); hex32(clocks.axi_cfg);
    add("_UNI="); hex32(clocks.unipro_cmd); add("/"); hex32(clocks.unipro_cfg);
    add("|UCLK="); add(clock_state);
}

static void check_peripherals(const void *dt)
{
    struct dre_usb_state usb = {0};
    struct dre_touch_state touch = {0};
    struct dre_gpu_state gpu = {0};
    add("|USB="); add(dre_usb_state(dt, &usb));
    if (usb.id) {
        add("_"); hex32(usb.id); add("/"); hex32(usb.control);
    }
    add("|USB_CLK="); hex32(usb.power); add("/"); hex32(usb.master);
    add("/"); hex32(usb.config);
    add("|TOUCH_SPI="); add(dre_touch_state(dt, &touch));
    add("_"); hex32(touch.votes); add("/"); hex32(touch.revision);
    add("/"); hex32(touch.status);
    add("|GPU_HW="); add(dre_gpu_state(dt, &gpu));
    add("_"); hex32(gpu.cx); add("/"); hex32(gpu.gx);
    add("/"); hex32(gpu.status);
}

const char *dre_board_report(const void *dt)
{
    used = 0;
    add("dre_report=UFS=");
    con_str("UFS", "reading power/clock state");
    check_ufs(dt);
    check_peripherals(dt);
    int gpu = fdt_node_offset_by_compatible(dt, -1, "qcom,adreno-gpu-a619-holi");
    int len;
    const fdt32_t *chip = fdt_getprop(dt, gpu, "qcom,chipid", &len);
    add(chip && len == 4 && fdt32_to_cpu(*chip) == 0x06010900 ?
        "|GPU_DT=A619v1" : "|GPU_DT=NO_MATCH");
    int tp = fdt_path_offset(dt, "/soc/spi@4a80000/novatek@0");
    add(!fdt_node_check_compatible(dt, tp, "novatek,nf_nt36672c") ?
        "|TOUCH_DT=NT36672C_SPI0" : "|TOUCH_DT=NO_MATCH");
    int chosen = fdt_path_offset(dt, "/chosen");
    const char *args = fdt_getprop(dt, chosen, "bootargs", &len);
    const char want[] = "tp_nt36672c_tm";
    int selected = 0;
    for (int i = 0; args && i + (int)sizeof(want) - 1 <= len; i++)
        if (!memcmp(args + i, want, sizeof(want) - 1)) { selected = 1; break; }
    add(selected ? "|PANEL_SEL=TIANMA_NT36672C" : "|PANEL_SEL=UNCONFIRMED");
#ifdef DRE_EXPECT_SERIAL
    const char serial[] = "androidboot.serialno=" DRE_EXPECT_SERIAL;
    int match = 0;
    for (int i = 0; args && i + (int)sizeof(serial) <= len; i++) {
        char after = args[i + sizeof(serial) - 1];
        if ((!i || args[i - 1] == ' ') && (after == ' ' || !after) &&
            !memcmp(args + i, serial, sizeof(serial) - 1)) { match = 1; break; }
    }
    add(match ? "|SERIAL=ABL_MATCH" : "|SERIAL=UNCONFIRMED");
#endif
    return report;
}
