/* SPDX-License-Identifier: MIT */
#include <libfdt.h>
#include "storage_handoff.h"
#include "storage_abi.h"
#include "dma_memory.h"
#include "ufs_read.h"
extern const char _head[], _end[];
static uint64_t handoff[8];

const char *dre_storage_prepare(const void *dt)
{
    handoff[0] = DRE_STORAGE_MAGIC;
    handoff[1] = dre_ufs_probe_arena(); handoff[2] = dre_ufs_probe_size();
    handoff[3] = (uintptr_t)dt; handoff[4] = fdt_totalsize(dt);
    handoff[5] = (uintptr_t)_head; handoff[6] = (uintptr_t)_end - (uintptr_t)_head;
    handoff[7] = dre_storage_crc(handoff, 56);
    if (!dre_storage_abi_valid(handoff) ||
        !dre_dma_ram_allowed(dt, handoff[1], handoff[2]) ||
        !dre_dma_ram_allowed(dt, handoff[3], handoff[4])) return "storage ownership unavailable";
    return 0;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* Locate exactly one fixed slot, after validating the entire ADT. */
static int walk(uint8_t *adt, uint32_t size, uint32_t *off, unsigned depth,
                unsigned *nodes, uint8_t **slot)
{
    if (depth > 16 || ++*nodes > 256 || *off > size || size - *off < 8) return 0;
    unsigned props = le32(adt + *off), children = le32(adt + *off + 4);
    *off += 8;
    if (props > 256 || children > 256) return 0;
    uint8_t *name = 0, *value = 0;
    for (unsigned i = 0; i < props; i++) {
        if (size - *off < 36) return 0;
        uint8_t *key = adt + *off;
        if (!memchr(key, 0, 32)) return 0;
        uint32_t len = le32(key + 32) & 0x7fffffff;
        *off += 36;
        uint32_t aligned = (len + 3) & ~3u;
        if (aligned > size - *off) return 0;
        if (!strcmp((const char *)key, "name")) {
            if (name || !len || !memchr(adt + *off, 0, len)) return 0;
            name = adt + *off;
        }
        if (!strcmp((const char *)key, "dre,handoff")) {
            if (value || len != 64) return 0;
            value = adt + *off;
        }
        *off += aligned;
    }
    if (!name) return 0;
    if (value) {
        if (depth != 1 || strcmp((const char *)name, "dre-ufs") || *slot) return 0;
        *slot = value;
    }
    for (unsigned i = 0; i < children; i++)
        if (!walk(adt, size, off, depth + 1, nodes, slot)) return 0;
    return 1;
}
const char *dre_storage_fixup(void *adt, uint32_t size)
{
    unsigned nodes = 0;
    uint32_t off = 0;
    uint8_t *slot = 0;
    if (!dre_storage_abi_valid(handoff) || !walk(adt, size, &off, 0, &nodes, &slot) ||
        off != size || !slot) return "invalid storage ADT slot";
    memcpy(slot, handoff, sizeof(handoff));
    return 0;
}
