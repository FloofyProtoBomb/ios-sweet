/* SM4350 handoff diagnostics. Optional read-only UFS commands use a private
 * DMA ring only on a verified bypass route. Touch clock votes are restored. */
#include <libfdt.h>
#include "console.h"
#include "ufs_clock.h"
#include "peripheral_state.h"
#include "identity.h"
#ifdef DRE_SCM_INFO_PROBE
#include "gpu/platform/scm_probe.h"
static struct dre_scm_probe_result scm_result;
static const void *scm_fdt;
static int scm_prepared;

static int scm_result_valid(void)
{
    return scm_prepared&&scm_result.version==DRE_SCM_PROBE_VERSION&&
        scm_result.struct_bytes==sizeof(scm_result)&&
        scm_result.status>DRE_SCM_PROBE_NOT_RUN&&scm_result.status<=DRE_SCM_PROBE_ARCH_UNAVAILABLE&&
        scm_result.query_count<=DRE_SCM_PROBE_QUERIES&&
        !((scm_result.available_mask|scm_result.unavailable_mask)&~63u)&&
        !(scm_result.available_mask&scm_result.unavailable_mask)&&
        scm_result.uncertain<=1&&scm_result.boot_must_stop<=1;
}

const char *dre_board_scm_prepare(const void *dt)
{
    if(scm_prepared)return "SCM INFO preflight already consumed; no repeated secure calls";
    scm_prepared=1;scm_fdt=dt;
    if(!dt||fdt_check_header(dt))return "SCM INFO original FDT unavailable";
    const char *status=dre_gpu_scm_probe_early(dt,fdt_totalsize(dt),&scm_result);
    con_str("SCM_INFO",status);
    con_hex("SCM_AVAIL",scm_result.available_mask);
    con_hex("SCM_UNAVAIL",scm_result.unavailable_mask);
    con_hex("SCM_UNCERTAIN",scm_result.uncertain);
    if(scm_result.query_count&&scm_result.query_count<=DRE_SCM_PROBE_QUERIES) {
        const struct dre_scm_probe_reply_record *last=&scm_result.replies[scm_result.query_count-1];
        con_hex("SCM_LAST_T",(uint64_t)last->transport);
        con_hex("SCM_LAST_S",(uint64_t)last->service);
    }
    if(!scm_result_valid()||scm_result.uncertain||scm_result.boot_must_stop||
       scm_result.status==DRE_SCM_PROBE_ARGUMENT||scm_result.status==DRE_SCM_PROBE_ARCH_UNAVAILABLE)
        return "SCM INFO preflight stopped; no later device operations or XNU handoff";
    return 0;
}
#endif
#ifdef DRE_DRIVER_PREP
#include "dma_proof.h"
#include "usb/prepare.h"
#ifdef DRE_UFS_HANDOFF_READ
#include "ufs_handoff_dma.h"
#endif
#endif
#ifdef DRE_DMA_PROBE
#include "dma_route.h"
#include "ufs_read.h"
#ifdef DRE_DMA_CONTEXT
#include "dma_context.h"
#endif
#endif
#ifdef DRE_TOUCH_PREFLIGHT
#include "touch_preflight.h"
#endif

/* Returns 0 on a synchronous data abort, 1 on success. */
extern int dre_read32(uintptr_t addr, uint32_t *out);

#ifdef DRE_DRIVER_PREP
static char report[496];
#elif defined(DRE_DMA_CONTEXT)
/* Leave room for the UI profile's additional msgbuf boot argument. */
static char report[512];
#else
static char report[496];
#endif
static unsigned used;
static int overflow, ufs_ready;
static const char *handoff_error;

static void add(const char *s)
{
    while (*s && used < sizeof(report) - 1)
        report[used++] = *s++;
    if (*s) overflow = 1;
    report[used] = 0;
}

static void hex32(uint32_t word)
{
    char value[9];
    for (int i = 0; i < 8; i++)
        value[i] = "0123456789abcdef"[(word >> ((7 - i) * 4)) & 15];
    value[8] = 0;
#ifdef DRE_DRIVER_PREP
    unsigned first = 0;
    while (first < 7 && value[first] == '0') first++;
    add(value + first);
#else
    add(value);
#endif
}

#ifdef DRE_SCM_INFO_PROBE
static void scm_hex(uint32_t value)
{
    char text[9];unsigned first=0;
    for(unsigned i=0;i<8;i++)text[i]="0123456789abcdef"[(value>>((7-i)*4))&15];
    text[8]=0;while(first<7&&text[first]=='0')first++;add(text+first);
}
static void scm_report(void)
{
    /* Schema1: status/availableMask/unavailableMask/uncertain/queryCount.
     * INFO callable functions only: no PAS permission/authentication claim. */
    add("|SCM_I=");scm_hex(scm_result.status);add("/");scm_hex(scm_result.available_mask);
    add("/");scm_hex(scm_result.unavailable_mask);add("/");scm_hex(scm_result.uncertain);
    add("/");scm_hex(scm_result.query_count);
}
#endif

#ifdef DRE_DMA_CONTEXT
static void hex64(uint64_t word)
{
    char value[17];
    for (unsigned i = 0; i < 16; i++) value[i] = "0123456789abcdef"[(word >> ((15 - i) * 4)) & 15];
    value[16] = 0;
#ifdef DRE_DRIVER_PREP
    unsigned first = 0;
    while (first < 15 && value[first] == '0') first++;
    add(value + first);
#else
    add(value);
#endif
}
#endif

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
    else { add(busy ? "LINK_BUSY" : "LINK_READY"); ufs_ready = !busy; }
#ifndef DRE_COMPACT_REPORT
    add("|UFS_VER="); hex32(version);
    add("_HCS="); hex32(status);
    add("_DBR="); hex32(busy);
#endif
clock_report:
#ifndef DRE_COMPACT_REPORT
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
#endif
    add("|UCLK="); add(clock_state);
}

#ifdef DRE_DMA_PROBE
#ifdef DRE_DMA_CONTEXT
static void context_report(const char *key, const struct dre_dma_route *route)
{
    struct dre_dma_context ctx;
    const char *state = dre_dma_context(route, &ctx);
    add(key); add(state);
    if (!ctx.captured) {
        if (!strcmp(state, "LAYOUT")) { add("/"); hex32(route->id1); }
        return;
    }
    add("/"); hex32(ctx.cbar);
#ifndef DRE_DRIVER_PREP
    add("/"); hex32(ctx.cba2r); add("/"); hex32(ctx.control);
#endif
    add("/"); hex32(ctx.tcr); add("/"); hex32(ctx.tcr2);
    add("/"); hex64(((uint64_t)ctx.ttbr_hi << 32) | ctx.ttbr_lo);
}
#endif

static void check_dma_ufs(const void *dt)
{
    struct dre_dma_route route = {0};
    add("|DMA_UFS=");
    add(ufs_ready ? dre_dma_route(dt, "/soc/ufshc@4804000", 0x60, &route) : "NOT_READY");
    add("/"); hex32(route.s2cr);
#ifdef DRE_DMA_CONTEXT
    context_report("|UCTX=", &route);
#ifdef DRE_DRIVER_PREP
    struct dre_dma_walk walk;
    add("|UMAP="); add(dre_dma_proof(dt, &route, "/soc/ufshc@4804000", 0x60,
                                       dre_ufs_probe_arena(), 8192, &walk));
    add("/"); hex32(walk.pages);
#endif
#else
    add("|IOMMU="); hex32(route.id0); add("/"); hex32(route.control);
#endif
#ifdef DRE_UFS_READ_TEST
    struct dre_ufs_read_state io = {.safe_to_handoff = 1};
#ifdef DRE_UFS_HANDOFF_READ
    struct dre_ufs_handoff_dma handoff;
    const char *admit = route.physical_dma ? "S2CR_BYPASS" : dre_ufs_handoff_begin(dt, &route, &handoff);
    add("|UFS_ADMIT="); add(admit);
#endif
    add("|UFS_IO=");
#ifdef DRE_UFS_HANDOFF_READ
    add(!ufs_ready ? "NOT_READY" : !strcmp(admit, "HANDOFF_BYPASS") ?
        dre_ufs_read_guarded(&route, &io, dre_ufs_handoff_valid, &handoff) : dre_ufs_read_test(&route, &io));
#else
    add(ufs_ready ? dre_ufs_read_test(&route, &io) : "NOT_READY");
#endif
#ifndef DRE_DMA_CONTEXT
    add("/"); hex32(io.ocs); add("/"); hex32(io.status);
#else
    if (io.ocs) { add("/OCS="); hex32(io.ocs); }
    if (io.status) { add("/S="); hex32(io.status); }
#endif
    add("/N="); hex32(io.commands);
#ifndef DRE_DMA_CONTEXT
    add("/B="); hex32(io.block_size);
#endif
    if (io.userdata_blocks) {
        add("/UD="); hex32((uint32_t)io.userdata_first); add("/"); hex32((uint32_t)io.userdata_blocks);
    }
    if (!io.safe_to_handoff) handoff_error = "UFS restore failed; reset required";
#endif
}
#endif

static void check_peripherals(const void *dt)
{
    struct dre_usb_state usb = {0};
#ifndef DRE_TOUCH_PREFLIGHT
    struct dre_touch_state touch = {0};
#endif
    struct dre_gpu_state gpu = {0};
    const char *usb_state = dre_usb_state(dt, &usb);
    add("|USB="); add(usb_state);
#ifndef DRE_COMPACT_REPORT
    if (usb.id) {
        add("_"); hex32(usb.id); add("/"); hex32(usb.control);
    }
    add("|USB_CLK="); hex32(usb.power); add("/"); hex32(usb.master);
    add("/"); hex32(usb.config);
#else
    struct dre_usb_detail detail = {0};
    add("|USB_STATE="); add(dre_usb_detail(dt, &detail));
#ifndef DRE_DRIVER_PREP
    add("/"); hex32(detail.control); add("/"); hex32(detail.status);
#endif
#ifndef DRE_DMA_CONTEXT
    add("/"); hex32(detail.config); add("/"); hex32(detail.endpoints);
    add("/"); hex32(detail.event_size);
#endif
#ifdef DRE_DMA_PROBE
    struct dre_dma_route route = {0};
    add("|DMA_USB=");
    add(!strcmp(usb_state, "DWC3_DEVICE") ?
        dre_dma_route(dt, "/soc/ssusb@4e00000", 0xe0, &route) : "NOT_READY");
    add("/"); hex32(route.s2cr);
#ifdef DRE_DMA_CONTEXT
    context_report("|XCTX=", &route);
#ifdef DRE_DRIVER_PREP
    struct dre_dma_walk walk;
    add("|XMAP="); add(dre_dma_proof(dt, &route, "/soc/ssusb@4e00000", 0xe0,
                                       dre_usb_probe_arena(), 8192, &walk));
    add("/"); hex32(walk.pages);
    struct dre_usb_prepare plan;
    add("|USB_PLAN="); add(dre_usb_prepare(dt, dre_usb_probe_arena(), &plan));
    add("/"); hex32(plan.address_bits); add("/"); hex32(plan.endpoints); add("/"); hex32(plan.events);
#endif
#endif
#endif
#endif
#ifdef DRE_TOUCH_PREFLIGHT
    struct dre_touch_preflight tp;
    add("|TOUCH="); add(dre_touch_preflight(dt, &tp));
#ifndef DRE_DRIVER_PREP
    add("/"); hex32(tp.revision); add("/P="); hex32(tp.pin_match);
    add("/F="); hex32(tp.tx_geometry); add("/"); hex32(tp.rx_geometry);
#else
    add("|GPI="); add(tp.gpi_state); add("/"); hex32(tp.gpi.idle_mask); add("/"); hex32(tp.gpi.states);
    add("|GDMA="); add(tp.gpi.dma_state); add("/"); hex32(tp.gpi.route);
    add("|GMAP="); add(tp.gpi.map_state); add("|SPI_PLAN="); add(tp.gpi.spi_plan);
#endif
#ifndef DRE_DMA_CONTEXT
    add("|TVOTE="); hex32(tp.votes_before); add("/"); hex32(tp.votes_during);
    add("/"); hex32(tp.votes_after);
#else
    add(tp.safe_to_handoff ? "|TVOTE=RESTORED" : "|TVOTE=RESTORE_FAILED");
#endif
    add("|TOUCH_FW="); add(tp.firmware_state);
    add("/"); hex32(tp.firmware_index); add("/"); hex32(tp.firmware_version);
    add("/"); hex32(tp.partitions);
    if (!tp.safe_to_handoff) handoff_error = "touch clock restore failed; reset required";
#else
    add("|TOUCH_SPI="); add(dre_touch_state(dt, &touch));
    add("_"); hex32(touch.votes); add("/"); hex32(touch.revision);
    add("/"); hex32(touch.status);
#endif
    add("|GPU_HW="); add(dre_gpu_state(dt, &gpu));
#ifndef DRE_COMPACT_REPORT
    add("_"); hex32(gpu.cx); add("/"); hex32(gpu.gx);
    add("/"); hex32(gpu.status);
#endif
}

const char *dre_board_report(const void *dt)
{
    used = 0;
    overflow = ufs_ready = 0;
    handoff_error = 0;
#ifdef DRE_SCM_INFO_PROBE
    if(!scm_result_valid()||scm_fdt!=dt||scm_result.uncertain||scm_result.boot_must_stop) {
        add("dre_report=SCM_PRECONDITION_FAILED");scm_report();
        handoff_error="SCM INFO precondition missing; no peripheral probes performed";
        return report;
    }
#endif
    add("dre_report=UFS=");
#ifndef DRE_XNU_DIRECT
    con_str("UFS", "reading power/clock state");
#endif
    check_ufs(dt);
#ifdef DRE_DMA_PROBE
    check_dma_ufs(dt);
#endif
    check_peripherals(dt);
#ifndef DRE_COMPACT_REPORT
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
#elif defined(DRE_EXPECT_SERIAL)
    int len, chosen = fdt_path_offset(dt, "/chosen");
    const char *args = fdt_getprop(dt, chosen, "bootargs", &len);
#endif
#ifdef DRE_EXPECT_SERIAL
    const char serial[] = "androidboot.serialno=" DRE_EXPECT_SERIAL;
    int match = 0;
    for (int i = 0; args && i + (int)sizeof(serial) <= len; i++) {
        char after = args[i + sizeof(serial) - 1];
        if ((!i || args[i - 1] == ' ') && (after == ' ' || !after) &&
            !memcmp(args + i, serial, sizeof(serial) - 1)) { match = 1; break; }
    }
    add(match ? "|SERIAL=ABL_MATCH" : "|SERIAL=UNCONFIRMED");
#else
    add(dre_identity_serial() ? "|SERIAL=ABL_DETECTED" : "|SERIAL=UNAVAILABLE");
#endif
#ifdef DRE_SCM_INFO_PROBE
    scm_report();
#endif
    return report;
}

const char *dre_board_error(void)
{
    if (handoff_error) { con_str("BUNDLE", report); return handoff_error; }
    return overflow ? "diagnostic report exceeds command-line budget" : 0;
}
