/* Validate the entire XNU window, including its 1 MiB panic-log reservation,
 * against this boot's live FDT before handing the embedded payload to XNU. */
#include <libfdt.h>
#include "loader.h"
#include "identity.h"
#ifdef DRE_BOARD_TESTS
const char *dre_board_report(const void *dt);
#endif

#define BASE UINT64_C(0x100000000)
#define SIZE UINT64_C(0x20000000)
#define LIMIT (BASE + SIZE)
extern const char _head[], _end[], _payload_start[], _payload_end[];

static uint64_t be64(const fdt32_t *p)
{
    return ((uint64_t)fdt32_to_cpu(p[0]) << 32) | fdt32_to_cpu(p[1]);
}

static int conflicts(uint64_t start, uint64_t n)
{
    return n && (n > UINT64_MAX - start || (start < LIMIT && start + n > BASE));
}

static int ram_ok(const void *dt)
{
    if (fdt_address_cells(dt, 0) != 2 || fdt_size_cells(dt, 0) != 2) return 0;
    int node, n, covered = 0;
    fdt_for_each_subnode(node, dt, 0) {
        if (fdt_stringlist_search(dt, node, "device_type", "memory") < 0) continue;
        if (fdt_getprop(dt, node, "linux,usable-memory", NULL)) return 0;
        const fdt32_t *reg = fdt_getprop(dt, node, "reg", &n);
        if (!reg || n % 16) return 0;
        for (int i = 0; i < n / 4; i += 4) {
            uint64_t base = be64(reg + i), size = be64(reg + i + 2);
            if (base <= BASE && size <= UINT64_MAX - base && base + size >= LIMIT)
                covered = 1;
        }
    }
    return covered;
}

static int reservations_clear(const void *dt)
{
    int count = fdt_num_mem_rsv(dt);
    if (count < 0) return 0;
    for (int i = 0; i < count; i++) {
        uint64_t base, size;
        if (fdt_get_mem_rsv(dt, i, &base, &size) || conflicts(base, size)) return 0;
    }
    int parent = fdt_path_offset(dt, "/reserved-memory"), node;
    /* dre_main already validated the reserved-memory cells and empty ranges. */
    fdt_for_each_subnode(node, dt, parent) {
        int n;
        const fdt32_t *ranges = fdt_getprop(dt, node, "reg", &n);
        if (!ranges) ranges = fdt_getprop(dt, node, "alloc-ranges", &n);
        if (!ranges || !n || n % 16) return 0;
        for (int i = 0; i < n / 4; i += 4)
            if (conflicts(be64(ranges + i), be64(ranges + i + 2))) return 0;
    }
    return 1;
}

static int address(const void *dt, int node, const char *name, uint64_t *value)
{
    int len;
    const fdt32_t *p = fdt_getprop(dt, node, name, &len);
    if (!p || (len != 4 && len != 8)) return 0;
    *value = len == 8 ? be64(p) : fdt32_to_cpu(*p);
    return 1;
}

const char *dre_handoff(const void *dt)
{
    uint64_t el, granules, mpidr, freq;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(granules));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (el != 4 || !((granules >> 20) & 15) ||
        (mpidr & UINT64_C(0xff00ffffff)) || freq != 19200000)
        return "CPU handoff mismatch";
    int chosen = fdt_path_offset(dt, "/chosen");
    uint64_t start, end;
    if (chosen < 0 || fdt_getprop(dt, chosen, "linux,usable-memory-range", NULL) ||
        !address(dt, chosen, "linux,initrd-start", &start) ||
        !address(dt, chosen, "linux,initrd-end", &end) || end < start ||
        conflicts(start, end - start) ||
        conflicts((uintptr_t)_head, (uintptr_t)_end - (uintptr_t)_head) ||
        conflicts((uintptr_t)dt, fdt_totalsize(dt)) ||
        !ram_ok(dt) || !reservations_clear(dt))
        return "RAM window unavailable";
    const uint64_t *p = (const uint64_t *)_payload_start;
    if ((uintptr_t)_payload_end - (uintptr_t)p < 72 ||
        p[7] != BASE || p[8] != SIZE - 0x100000)
        return "payload RAM mismatch";
    const char *identity_error = dre_identity_detect(dt);
    if (identity_error) return identity_error;
    const char *extra = NULL;
#ifdef DRE_BOARD_TESTS
    extra = dre_board_report(dt);
#endif
    return loader_boot_range_with_adt((uintptr_t)p, (uintptr_t)_payload_end,
                                      extra, dre_identity_fixup);
}
