/* SPDX-License-Identifier: MIT */
#include <libfdt.h>
#include "cpu_topology.h"

const char *dre_cpu_topology(const void *dt, unsigned requested)
{
    if (!requested || requested > 8) return "unsupported CPU count";
    int psci = fdt_path_offset(dt, "/soc/psci"), n;
    const char *method = fdt_getprop(dt, psci, "method", &n);
    if (fdt_node_check_compatible(dt, psci, "arm,psci-1.0") ||
        !method || n != 4 || memcmp(method, "smc\0", 4)) return "PSCI method mismatch";
    int cpus = fdt_path_offset(dt, "/cpus"), cells = fdt_address_cells(dt, cpus);
    if (cpus < 0 || (cells != 1 && cells != 2) || fdt_size_cells(dt, cpus) != 0)
        return "CPU cells mismatch";
    unsigned present = 0, count = 0;
    int node;
    fdt_for_each_subnode(node, dt, cpus) {
        if (fdt_stringlist_search(dt, node, "device_type", "cpu") < 0) continue;
        const char *enable = fdt_getprop(dt, node, "enable-method", &n);
        if (fdt_node_check_compatible(dt, node, "qcom,kryo") ||
            !enable || n != 5 || memcmp(enable, "psci\0", 5))
            return "CPU enable method mismatch";
        const fdt32_t *reg = fdt_getprop(dt, node, "reg", &n);
        if (!reg || n != cells * 4 || (cells == 2 && reg[0])) return "CPU reg mismatch";
        uint32_t id = fdt32_to_cpu(reg[cells - 1]);
        if ((id & 255) || id > 0x700 || (present & (1u << (id >> 8))))
            return "CPU affinity mismatch";
        const char *status = fdt_getprop(dt, node, "status", &n);
        if ((id >> 8) < requested && status &&
            !((n == 3 && !memcmp(status, "ok\0", 3)) ||
              (n == 5 && !memcmp(status, "okay\0", 5)))) return "selected CPU disabled";
        present |= 1u << (id >> 8); count++;
    }
    return present == 255 && count == 8 ? 0 : "incomplete eight-core topology";
}
