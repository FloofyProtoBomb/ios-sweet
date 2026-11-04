/* Stage one. Read only the live DT and architectural registers until the
 * board and its reserved splash range have both been identified. */
#include <libfdt.h>
#include "console.h"

#define READ_SYS(reg) ({ uint64_t v; __asm__ volatile("mrs %0, " #reg : "=r"(v)); v; })
#define MAX_DTB (2 * 1024 * 1024) /* Linux arm64 boot protocol */
extern const char _head[], _end[];
#ifdef DRE_XNU
const char *dre_handoff(const void *dt);
#endif

static uint64_t pair(const fdt32_t *p)
{
    return ((uint64_t)fdt32_to_cpu(p[0]) << 32) | fdt32_to_cpu(p[1]);
}

static int is_dre(const void *dt)
{
    int len;
    const fdt32_t *projects = fdt_getprop(dt, 0, "oplus,dtsi_no", &len);
    if (fdt_node_check_compatible(dt, 0, "qcom,holi") ||
        !projects || len < 4 || len % 4) return 0;
    for (int i = 0; i < len / 4; i++) {
        uint32_t id = fdt32_to_cpu(projects[i]);
        if (id == 133144 || id == 133180) return 1;
    }
    return 0;
}

static uint64_t splash(const void *dt)
{
    int len, parent = fdt_path_offset(dt, "/reserved-memory");
    if (parent < 0 || fdt_address_cells(dt, parent) != 2 ||
        fdt_size_cells(dt, parent) != 2 ||
        !fdt_getprop(dt, parent, "ranges", &len) || len != 0) return 0;
    int node;
    fdt_for_each_subnode(node, dt, parent) {
        if (fdt_stringlist_search(dt, node, "label", "cont_splash_region") < 0)
            continue;
        const fdt32_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (!reg || len != 16) return 0;
        uint64_t base = pair(reg), size = pair(reg + 2);
        /* Limit writes to the documented Holi region. A firmware override
         * outside it needs a new reviewed profile. */
        if (base != 0x85200000 || size < FB_STRIDE * FB_H ||
            size > 0xc00000) return 0;
        uint64_t end = base + FB_STRIDE * FB_H;
        if ((base < (uintptr_t)_end && (uintptr_t)_head < end) ||
            (base < (uintptr_t)dt + fdt_totalsize(dt) && (uintptr_t)dt < end))
            return 0;
        return base;
    }
    return 0;
}

void dre_main(const void *dt)
{
    if (!dt || ((uintptr_t)dt & 7) || fdt_check_header(dt) ||
        fdt_totalsize(dt) > MAX_DTB || fdt_check_full(dt, fdt_totalsize(dt)) ||
        !is_dre(dt)) return;
    uintptr_t base = splash(dt);
    if (!base) return;
    con_fb = (volatile uint32_t *)base;
    con_clear(BLUE);
    con_str("DRE", "N200 / SM4350");
    con_str("STAGE", "Boot shim running");
    con_hex("SPLASH", base);
    con_hex("FDT", (uintptr_t)dt);
    con_hex("EL", READ_SYS(CurrentEL) >> 2);
    con_hex("CPU", READ_SYS(mpidr_el1));
    con_hex("CNTFRQ", READ_SYS(cntfrq_el0));
    con_hex("MMFR0", READ_SYS(id_aa64mmfr0_el1));
    int node, len;
    fdt_for_each_subnode(node, dt, 0) {
        if (fdt_stringlist_search(dt, node, "device_type", "memory") < 0 ||
            fdt_address_cells(dt, 0) != 2 || fdt_size_cells(dt, 0) != 2)
            continue;
        const fdt32_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (!reg || len % 16) continue;
        for (int i = 0; i < len / 4; i += 4) {
            con_hex("RAM", pair(reg + i));
            con_hex("SIZE", pair(reg + i + 2));
        }
    }
#ifdef DRE_XNU
    con_str("XNU", dre_handoff(dt));
#else
    con_str("RESULT", "DRE PROBE OK");
#endif
}
