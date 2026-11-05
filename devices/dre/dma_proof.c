/* SPDX-License-Identifier: MIT
 * Inspect inherited tables only. A matching table is not permission to DMA:
 * ownership, hardware translation caches and XPU permissions remain separate. */
#include <libfdt.h>
#include "dma_proof.h"
#include "dma_memory.h"
extern int dre_read32(uintptr_t address, uint32_t *out);
extern const char _head[], _end[];
static volatile uint8_t usb_arena[8192] __attribute__((aligned(4096)));
uintptr_t dre_usb_probe_arena(void) { return (uintptr_t)usb_arena; }

struct reader { const void *dt; uint64_t initrd, initrd_end; const char *error; };
static int overlap(uint64_t address, uint64_t bytes, uint64_t start, uint64_t end)
{
    return address < end && address + bytes > start;
}
static int table_read(void *cookie, uint64_t address, uint64_t *out)
{
    struct reader *r = cookie;
    r->error = "TABLE_RAM_BLOCKED";
    if ((address & 7) || !dre_dma_ram_allowed(r->dt, address, 8) ||
        overlap(address, 8, (uintptr_t)_head, (uintptr_t)_end) ||
        overlap(address, 8, (uintptr_t)r->dt, (uintptr_t)r->dt + fdt_totalsize(r->dt)) ||
        overlap(address, 8, r->initrd, r->initrd_end)) return 0;
    uint32_t lo, hi, lo2, hi2;
    r->error = "TABLE_READ_FAULT";
    if (!dre_read32(address, &lo) || !dre_read32(address + 4, &hi) ||
        !dre_read32(address, &lo2) || !dre_read32(address + 4, &hi2)) return 0;
    r->error = "TABLE_TORN";
    if (lo != lo2 || hi != hi2) return 0;
    *out = ((uint64_t)hi << 32) | lo;
    r->error = NULL;
    return 1;
}
static int dt_address(const void *dt, const char *key, uint64_t *out)
{
    int n; const fdt32_t *p = fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), key, &n);
    if (!p || (n != 4 && n != 8)) return 0;
    *out = fdt32_to_cpu(p[0]);
    if (n == 8) *out = (*out << 32) | fdt32_to_cpu(p[1]);
    return 1;
}
const char *dre_dma_proof(const void *dt, const struct dre_dma_route *route,
                          const char *path, uint32_t sid, uintptr_t buffer,
                          uint64_t bytes, struct dre_dma_walk *s)
{
    *s = (struct dre_dma_walk){0};
    if (route->physical_dma) return "BYPASS";
    struct dre_dma_context before, after;
    const char *mode = dre_dma_context(route, &before);
    if (strcmp(mode, "S1_64")) return mode;
    uint64_t cpu_control;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(cpu_control));
    if (cpu_control & 5) return "CPU_CACHE_OR_MMU_ON";
    /* Only inspect a buffer owned by this shim. Never accept arbitrary PAs. */
    if (!bytes || bytes > (uintptr_t)_end - (uintptr_t)_head || buffer < (uintptr_t)_head ||
        buffer > (uintptr_t)_end - bytes) return "BUFFER_NOT_OWNED";
    struct reader reader = {.dt = dt};
    if (!dt_address(dt, "linux,initrd-start", &reader.initrd) ||
        !dt_address(dt, "linux,initrd-end", &reader.initrd_end) || reader.initrd_end < reader.initrd)
        return "INITRD_UNKNOWN";
    const char *result = dre_dma_walk(&before, buffer, bytes, table_read, &reader, s);
    struct dre_dma_route check;
    const char *route_state = dre_dma_route(dt, path, sid, &check);
    if (strcmp(route_state, "TRANSLATED") || memcmp(route, &check, sizeof(check))) return "ROUTE_CHANGED";
    if (strcmp(dre_dma_context(&check, &after), "S1_64") || memcmp(&before, &after, sizeof(before)))
        return "CONTEXT_CHANGED";
    return !strcmp(result, "TABLE_UNREADABLE") && reader.error ? reader.error : result;
}
