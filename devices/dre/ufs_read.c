/* SPDX-License-Identifier: MIT
 * Standalone read-only UFSHCI test. No host reset, media writes, UNMAP,
 * query writes, RPMB, crypto-engine changes or SMMU configuration. */
#include <stdint.h>
#include "ufs_read.h"

extern int dre_read32(uintptr_t address, uint32_t *out);
extern int dre_write32(uintptr_t address, uint32_t value);

#define HOST UINT64_C(0x04804000)
#define UTRL_LO 0x50
#define UTRL_HI 0x54
#define DOORBELL 0x58
#define CLEAR 0x5c
#define RUN_STOP 0x60
#define IE 0x24
#define BLOCK 4096u

/* 32 UTRDs, one 128-byte-aligned UCD and one block. MMU/caches are off. */
static volatile uint8_t arena[0x2000] __attribute__((aligned(4096)));
#define DMA_RING ((volatile uint32_t *)arena)
#define DMA_COMMAND (arena + 0x400)
#define DMA_RESPONSE (arena + 0x600)
#define DMA_PRDT ((volatile uint32_t *)(arena + 0x800))
#define DMA_DATA (arena + 0x1000)

static uint64_t ticks(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static void fence(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static int read_reg(uint32_t off, uint32_t *out) { return dre_read32(HOST + off, out); }
static int write_reg(uint32_t off, uint32_t value) { return dre_write32(HOST + off, value); }

static int wait_reg(uint32_t off, uint32_t mask, uint32_t expected)
{
    uint64_t start = ticks();
    uint32_t value;
    do {
        if (!read_reg(off, &value)) return 0;
        if ((value & mask) == expected) return 1;
    } while (ticks() - start < 9600000); /* 500 ms at the checked 19.2 MHz */
    return 0;
}

static uint64_t be(const volatile uint8_t *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; i++) value = (value << 8) | p[i];
    return value;
}

static uint64_t le(const volatile uint8_t *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static uint32_t crc_step(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((0u - (crc & 1)) & 0xedb88320);
    return crc;
}

static void clear_arena(void)
{
    volatile uint64_t *p = (volatile uint64_t *)arena;
    for (unsigned i = 0; i < sizeof(arena) / 8; i++) p[i] = 0;
}

/* This interface accepts only the four commands constructed here. */
static const char *submit(struct dre_ufs_read_state *s, unsigned op, uint32_t lba)
{
    s->ocs = UINT32_MAX; s->response = 0; s->status = 0;
    uint32_t len;
    if (op == 0xff || op == 0) len = 0; /* NOP UPIU or SCSI TEST UNIT READY */
    else if (op == 0x9e) len = 32;     /* READ CAPACITY(16) */
    else if (op == 0x28) len = BLOCK;  /* READ(10), exactly one block */
    else return "COMMAND_REJECTED";
    uint32_t doorbell;
    if (!read_reg(DOORBELL, &doorbell) || doorbell) return "COMMAND_BUSY";
    clear_arena();
    uint8_t tag = (uint8_t)++s->commands;
    DMA_COMMAND[0] = op == 0xff ? 0 : 1;
    DMA_COMMAND[1] = len ? 0x40 : 0;
    DMA_COMMAND[2] = 0;                  /* LUN 0, never an RPMB well-known LUN */
    DMA_COMMAND[3] = tag;
    for (unsigned i = 0; i < 4; i++) DMA_COMMAND[12 + i] = len >> (24 - 8 * i);
    if (op != 0xff) DMA_COMMAND[16] = (uint8_t)op;
    if (op == 0x9e) { DMA_COMMAND[17] = 0x10; DMA_COMMAND[29] = 32; }
    if (op == 0x28) {
        for (unsigned i = 0; i < 4; i++) DMA_COMMAND[18 + i] = lba >> (24 - 8 * i);
        DMA_COMMAND[24] = 1;
    }
    uintptr_t ucd = (uintptr_t)DMA_COMMAND, block = (uintptr_t)DMA_DATA;
    DMA_RING[0] = (1u << 28) | (1u << 24) | (len ? 2u << 25 : 0);
    DMA_RING[2] = 0xf;
    DMA_RING[4] = (uint32_t)ucd; DMA_RING[5] = (uint64_t)ucd >> 32;
    DMA_RING[6] = (128u << 16) | 128u;
    if (len) {
        DMA_RING[7] = (256u << 16) | 1;
        DMA_PRDT[0] = (uint32_t)block; DMA_PRDT[1] = (uint64_t)block >> 32;
        DMA_PRDT[3] = len - 1;
    }
    fence();
    if (!write_reg(DOORBELL, 1)) return "DOORBELL_FAULT";
    if (!wait_reg(DOORBELL, 1, 0)) return "COMMAND_TIMEOUT";
    fence();
    if (!write_reg(0x20, 1)) return "ACK_FAULT";
    s->ocs = DMA_RING[2] & 255;
    s->response = DMA_RESPONSE[6]; s->status = DMA_RESPONSE[7];
    if (s->ocs || DMA_RESPONSE[3] != tag || DMA_RESPONSE[2] ||
        (DMA_RESPONSE[0] & 0x3f) != (op == 0xff ? 0x20 : 0x21) || s->response)
        return "BAD_RESPONSE";
    if (s->status) return "SCSI_STATUS";
    if (len && ((DMA_RESPONSE[1] & 0x60) || be(DMA_RESPONSE + 12, 4)))
        return "SHORT_TRANSFER";
    return 0;
}

static const char *read_gpt(struct dre_ufs_read_state *s)
{
    const char *error = submit(s, 0x28, 1);
    if (error) return error;
    static const char signature[] = "EFI PART";
    for (unsigned i = 0; i < 8; i++) if (DMA_DATA[i] != signature[i]) return "NO_GPT";
    uint32_t header_len = le(DMA_DATA + 12, 4), expected_crc = le(DMA_DATA + 16, 4);
    uint64_t first_usable = le(DMA_DATA + 40, 8), last_usable = le(DMA_DATA + 48, 8);
    if (le(DMA_DATA + 8, 4) != 0x10000 || header_len < 92 || header_len > BLOCK ||
        le(DMA_DATA + 20, 4) || le(DMA_DATA + 24, 8) != 1 ||
        le(DMA_DATA + 32, 8) != s->last_lba || first_usable > last_usable ||
        first_usable < 2 || last_usable >= s->last_lba) return "BAD_GPT_HEADER";
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < header_len; i++) crc = crc_step(crc, i >= 16 && i < 20 ? 0 : DMA_DATA[i]);
    if (~crc != expected_crc) return "GPT_HEADER_CRC";
    uint64_t entries_lba = le(DMA_DATA + 72, 8);
    uint32_t entries = le(DMA_DATA + 80, 4), size = le(DMA_DATA + 84, 4), array_crc = le(DMA_DATA + 88, 4);
    if (!entries || entries > 128 || size != 128 || entries_lba < 2 ||
        entries_lba > UINT32_MAX || entries_lba > s->last_lba ||
        (entries * size + BLOCK - 1) / BLOCK - 1 > s->last_lba - entries_lba ||
        (entries * size + BLOCK - 1) / BLOCK - 1 > UINT32_MAX - entries_lba ||
        entries_lba + (entries * size + BLOCK - 1) / BLOCK > first_usable)
        return "GPT_LAYOUT_UNSUPPORTED";
    unsigned left = entries * size;
    crc = UINT32_MAX;
    for (unsigned block = 0; left; block++) {
        error = submit(s, 0x28, (uint32_t)entries_lba + block);
        if (error) return error;
        unsigned n = left < BLOCK ? left : BLOCK;
        for (unsigned i = 0; i < n; i++) crc = crc_step(crc, DMA_DATA[i]);
        for (unsigned off = 0; off < n; off += 128) {
            unsigned present = 0;
            for (unsigned i = 0; i < 16; i++) present |= DMA_DATA[off + i];
            if (!present) continue; /* An unused GPT entry has a zero type GUID. */
            static const char name[] = "userdata";
            unsigned i;
            for (i = 0; i < sizeof(name); i++)
                if (DMA_DATA[off + 56 + i * 2] != name[i] || DMA_DATA[off + 57 + i * 2]) break;
            if (i != sizeof(name)) continue;
            uint64_t first = le(DMA_DATA + off + 32, 8), last = le(DMA_DATA + off + 40, 8);
            if (s->userdata_blocks || first < first_usable ||
                first > last || last > last_usable) return "BAD_USERDATA_RANGE";
            s->userdata_first = first;
            s->userdata_blocks = last - first + 1;
        }
        left -= n;
    }
    if (~crc != array_crc) return "GPT_ARRAY_CRC";
    return s->userdata_blocks ? 0 : "NO_USERDATA";
}

const char *dre_ufs_read_test(const struct dre_dma_route *route, struct dre_ufs_read_state *s)
{
    *s = (struct dre_ufs_read_state){.safe_to_handoff = 1};
    if (!route->physical_dma) return "DMA_BLOCKED";
    uint64_t control;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(control));
    if (control & 5) return "CACHE_OR_MMU_ON"; /* Arena uses physical, uncached CPU accesses. */
    uint32_t enable, status, busy, task_busy, lo, hi, run, interrupts;
    if (!read_reg(0, &s->capabilities) || !read_reg(0x34, &enable) || !read_reg(0x30, &status) ||
        !read_reg(DOORBELL, &busy) || !read_reg(0x78, &task_busy) ||
        !read_reg(UTRL_LO, &lo) || !read_reg(UTRL_HI, &hi) ||
        !read_reg(RUN_STOP, &run) || !read_reg(IE, &interrupts)) return "HCI_READ_FAULT";
    if (!(enable & 1) || (status & 3) != 3 || busy || task_busy || run > 1) return "HOST_NOT_IDLE";
    uintptr_t dma = (uintptr_t)arena;
    if (((uint64_t)dma + sizeof(arena) - 1) >> 32 && !(s->capabilities & (1u << 24)))
        return "ADDRESS_WIDTH";
    uint64_t old_ring = (uint64_t)lo | ((uint64_t)hi << 32);
    if (old_ring > UINT64_MAX - 1024 ||
        (old_ring < (uint64_t)dma + sizeof(arena) && old_ring + 1024 > dma))
        return "RING_OVERLAP";
    s->safe_to_handoff = 0;
    const char *error = "ATTACH_FAILED";
    if (!write_reg(IE, 0) || !write_reg(RUN_STOP, 0) || !wait_reg(RUN_STOP, 1, 0) ||
        !write_reg(UTRL_LO, (uint32_t)dma) || !write_reg(UTRL_HI, (uint64_t)dma >> 32) ||
        !wait_reg(UTRL_LO, UINT32_MAX, (uint32_t)dma) ||
        !wait_reg(UTRL_HI, UINT32_MAX, (uint64_t)dma >> 32) ||
        !write_reg(RUN_STOP, 1) || !wait_reg(RUN_STOP, 1, 1)) goto restore;
    error = submit(s, 0xff, 0);
    if (error) goto restore;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        error = submit(s, 0, 0);
        if (!error || s->status != 2) break; /* Bounded UNIT ATTENTION / readiness retry */
    }
    if (error) goto restore;
    error = submit(s, 0x9e, 0);
    if (error) goto restore;
    s->last_lba = be(DMA_DATA, 8); s->block_size = be(DMA_DATA + 8, 4);
    if (!s->last_lba || s->last_lba == UINT64_MAX || s->block_size != BLOCK) {
        error = "CAPACITY_UNSUPPORTED"; goto restore;
    }
    error = read_gpt(s);
restore:
    /* Stop before restoring addresses. Never enter XNU with unowned DMA active. */
    if (!write_reg(CLEAR, ~1u) || !wait_reg(DOORBELL, UINT32_MAX, 0) ||
        !write_reg(RUN_STOP, 0) || !wait_reg(RUN_STOP, 1, 0) ||
        !write_reg(UTRL_LO, lo) || !write_reg(UTRL_HI, hi) ||
        !wait_reg(UTRL_LO, UINT32_MAX, lo) || !wait_reg(UTRL_HI, UINT32_MAX, hi) ||
        !write_reg(RUN_STOP, run) || !wait_reg(RUN_STOP, UINT32_MAX, run) ||
        !write_reg(IE, interrupts) || !wait_reg(IE, UINT32_MAX, interrupts)) return "RESTORE_FAILED";
    if (!read_reg(0x34, &enable) || !read_reg(0x30, &status) ||
        !(enable & 1) || (status & 3) != 3) return "RESTORE_FAILED";
    s->safe_to_handoff = 1;
    clear_arena();
    return error ? error : "GPT_PASS";
}
