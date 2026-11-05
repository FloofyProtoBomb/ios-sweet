/* SPDX-License-Identifier: MIT */
#include <libfdt.h>
#include "dma_memory.h"

static uint64_t pair(const fdt32_t *p)
{
    return ((uint64_t)fdt32_to_cpu(p[0]) << 32) | fdt32_to_cpu(p[1]);
}

static int clear(uint64_t base, uint64_t bytes, uint64_t address, uint64_t size)
{
    return bytes <= UINT64_MAX - base &&
        (!bytes || base >= address + size || base + bytes <= address);
}

static int allowed(const void *dt, uint64_t address, uint64_t size, int boot_owned)
{
    if (!size || size > UINT64_MAX - address || fdt_address_cells(dt, 0) != 2 ||
        fdt_size_cells(dt, 0) != 2) return 0;
    int node, n, covered = 0;
    fdt_for_each_subnode(node, dt, 0) {
        if (fdt_stringlist_search(dt, node, "device_type", "memory") < 0) continue;
        if (fdt_getprop(dt, node, "linux,usable-memory", NULL)) return 0;
        const fdt32_t *p = fdt_getprop(dt, node, "reg", &n);
        if (!p || !n || n % 16) return 0;
        for (int i = 0; i < n / 4; i += 4) {
            uint64_t base = pair(p + i), bytes = pair(p + i + 2);
            if (bytes > UINT64_MAX - base) return 0;
            if (base <= address && base + bytes >= address + size) covered = 1;
        }
    }
    if (!covered) return 0;
    int count = fdt_num_mem_rsv(dt);
    if (count < 0) return 0;
    for (int i = 0; i < count; i++) {
        uint64_t base, bytes;
        if (fdt_get_mem_rsv(dt, i, &base, &bytes) || !clear(base, bytes, address, size)) return 0;
    }
    int parent = fdt_path_offset(dt, "/reserved-memory");
    if (parent < 0 || fdt_address_cells(dt, parent) != 2 || fdt_size_cells(dt, parent) != 2 ||
        !fdt_getprop(dt, parent, "ranges", &n) || n) return 0;
    fdt_for_each_subnode(node, dt, parent) {
        const fdt32_t *p = fdt_getprop(dt, node, "reg", &n);
        int dynamic = !p;
        if (dynamic) p = fdt_getprop(dt, node, "alloc-ranges", &n);
        if (!p || !n || n % 16) return 0;
        for (int i = 0; i < n / 4; i += 4) {
            uint64_t base = pair(p + i), bytes = pair(p + i + 2);
            if (bytes > UINT64_MAX - base ||
                (!(boot_owned && dynamic) && !clear(base, bytes, address, size))) return 0;
        }
        if (boot_owned && dynamic) {
            /* A dynamic node describes a future Linux allocation, not an
             * occupied interval. Only accept this exception for positively
             * owned boot image / input FDT bytes, before any Linux allocator.
             * Fixed reg and memreserve entries above are never exempted. */
            const fdt32_t *bytes = fdt_getprop(dt, node, "size", &n);
            if (!bytes || n != 8 || !pair(bytes) ||
                fdt_node_check_compatible(dt, node, "shared-dma-pool")) return 0;
        }
    }
    return 1;
}

int dre_dma_ram_allowed(const void *dt, uint64_t address, uint64_t size)
{ return allowed(dt, address, size, 0); }
int dre_dma_boot_owned_ram_allowed(const void *dt, uint64_t address, uint64_t size)
{ return allowed(dt, address, size, 1); }
