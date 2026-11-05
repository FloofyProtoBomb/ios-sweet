/* SPDX-License-Identifier: MIT
 * Guarded UFSHCI engine. Read-only entry points never issue media writes.
 * Explicit userdata-only write API requires a guard and flushes completion.
 * No host reset, UNMAP, query writes, RPMB, crypto or SMMU configuration. */
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

/* Kernel ownership uses an entire 16K page outside XNU's managed RAM.
 * The native adapter maps it Device/uncached and provides its physical
 * address separately: a kernel VA must never enter a hardware descriptor. */
#ifdef DRE_UFS_KERNEL
extern volatile uint8_t *dre_kernel_ufs_arena;
extern uint64_t dre_kernel_ufs_arena_pa;
#define arena dre_kernel_ufs_arena
#define ARENA_BYTES 0x4000u
#define ARENA_PA dre_kernel_ufs_arena_pa
#else
#ifdef DRE_NATIVE_UFS
#define ARENA_BYTES 0x4000u
#define ARENA_ALIGNMENT 0x4000
#else
#define ARENA_BYTES 0x2000u
#define ARENA_ALIGNMENT 4096
#endif
static volatile uint8_t arena[ARENA_BYTES] __attribute__((aligned(ARENA_ALIGNMENT)));
#define ARENA_PA ((uint64_t)(uintptr_t)arena)
#endif
uintptr_t dre_ufs_probe_arena(void) { return (uintptr_t)ARENA_PA; }
unsigned dre_ufs_probe_size(void) { return ARENA_BYTES; }
#define DMA_RING ((volatile uint32_t *)arena)
#define DMA_COMMAND (arena + 0x400)
#define DMA_RESPONSE (arena + 0x600)
#define DMA_PRDT ((volatile uint32_t *)(arena + 0x800))
#define DMA_DATA (arena + 0x1000)

static uint64_t ticks(void)
{
#ifdef DRE_UFS_HOST_TEST
    extern uint64_t dre_ufs_test_ticks(void);
    return dre_ufs_test_ticks();
#else
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(value));
    return value;
#endif
}

static void fence(void) {
#ifdef DRE_UFS_HOST_TEST
    __sync_synchronize();
#else
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
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
    for (unsigned i = 0; i < ARENA_BYTES / 8; i++) p[i] = 0;
}

static void timeout_snapshot(struct dre_ufs_read_state *s, uint64_t start)
{
    s->command_ticks = ticks() - start;
    read_reg(DOORBELL, &s->fault_doorbell);
    read_reg(0x20, &s->fault_interrupts);
    read_reg(0x30, &s->fault_hcs);
    s->fault_ocs = DMA_RING[2] & 255;
    for (unsigned i = 0; i < 5; i++) read_reg(0x38 + i * 4, &s->fault_uec[i]);
}

/* This interface accepts only the four commands constructed here. */
static const char *submit_data(struct dre_ufs_read_state *s, unsigned op, uint32_t lba,
                               const uint8_t *input)
{
    s->ocs = UINT32_MAX; s->response = 0; s->status = 0;
    if (s->guard && !s->guard(s->guard_cookie)) { s->guard_lost = 1; return "DMA_GUARD_CHANGED"; }
    uint32_t len;
    if (op == 0xff || op == 0 || op == 0x35) len = 0; /* NOP/TUR/cache flush */
    else if (op == 0x9e) len = 32;     /* READ CAPACITY(16) */
    else if (op == 0x28 || (op == 0x2a && input)) len = BLOCK;
    else return "COMMAND_REJECTED";
    uint32_t doorbell;
    if (!read_reg(DOORBELL, &doorbell) || doorbell) return "COMMAND_BUSY";
    clear_arena();
    uint8_t tag = (uint8_t)++s->commands;
    s->last_opcode = op; s->command_lba = lba;
    DMA_COMMAND[0] = op == 0xff ? 0 : 1;
    DMA_COMMAND[1] = len ? (op == 0x2a ? 0x20 : 0x40) : 0;
    DMA_COMMAND[2] = 0;                  /* LUN 0, never an RPMB well-known LUN */
    DMA_COMMAND[3] = tag;
    for (unsigned i = 0; i < 4; i++) DMA_COMMAND[12 + i] = len >> (24 - 8 * i);
    if (op != 0xff) DMA_COMMAND[16] = (uint8_t)op;
    if (op == 0x9e) { DMA_COMMAND[17] = 0x10; DMA_COMMAND[29] = 32; }
    if (op == 0x28 || op == 0x2a) {
        for (unsigned i = 0; i < 4; i++) DMA_COMMAND[18 + i] = lba >> (24 - 8 * i);
        DMA_COMMAND[24] = 1;
    }
    uint64_t ucd = ARENA_PA + 0x400, block = ARENA_PA + 0x1000;
    DMA_RING[0] = (1u << 28) | (1u << 24) | (len ? (op == 0x2a ? 1u : 2u) << 25 : 0);
    DMA_RING[2] = 0xf;
    DMA_RING[4] = (uint32_t)ucd; DMA_RING[5] = (uint64_t)ucd >> 32;
    DMA_RING[6] = (128u << 16) | 128u;
    if (len) {
        DMA_RING[7] = (256u << 16) | 1;
        DMA_PRDT[0] = (uint32_t)block; DMA_PRDT[1] = (uint64_t)block >> 32;
        DMA_PRDT[3] = len - 1;
    }
    if (op == 0x2a) for (unsigned i = 0; i < BLOCK; i++) DMA_DATA[i] = input[i];
    fence();
    if (!write_reg(DOORBELL, 1)) return "DOORBELL_FAULT";
    uint64_t start = ticks();
    if (!wait_reg(DOORBELL, 1, 0)) { timeout_snapshot(s, start); return "COMMAND_TIMEOUT"; }
    s->command_ticks = ticks() - start;
    fence();
    if (s->guard && !s->guard(s->guard_cookie)) { s->guard_lost = 1; return "DMA_GUARD_CHANGED"; }
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
static const char *submit(struct dre_ufs_read_state *s, unsigned op, uint32_t lba)
{ return submit_data(s, op, lba, 0); }

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
    return dre_ufs_read_guarded(route, s, 0, 0);
}

static const char *transaction(const struct dre_dma_route *route, struct dre_ufs_read_state *s,
                              dre_ufs_dma_guard guard, void *cookie, uint32_t lba,
                              uint8_t *block_out, const uint8_t *block_in, int flush)
{
    uint64_t last = s->last_lba, first = s->userdata_first, blocks = s->userdata_blocks;
    *s = (struct dre_ufs_read_state){.safe_to_handoff = 1, .guard = guard, .guard_cookie = cookie};
    if (block_out || block_in || flush) {
        s->last_lba = last; s->userdata_first = first; s->userdata_blocks = blocks;
        s->block_size = BLOCK;
        if (!last || (!flush && lba > last)) return "LBA_RANGE";
    }
    if (!route->physical_dma && !guard) return "DMA_BLOCKED";
    if (guard && !guard(cookie)) return "DMA_GUARD_BLOCKED";
#ifndef DRE_UFS_KERNEL
    uint64_t control;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(control));
    if (control & 5) return "CACHE_OR_MMU_ON"; /* Arena uses physical, uncached CPU accesses. */
#endif
    uint32_t enable, status, busy, task_busy, lo, hi, run, interrupts;
    if (!read_reg(0, &s->capabilities) || !read_reg(0x34, &enable) || !read_reg(0x30, &status) ||
        !read_reg(DOORBELL, &busy) || !read_reg(0x78, &task_busy) ||
        !read_reg(UTRL_LO, &lo) || !read_reg(UTRL_HI, &hi) ||
        !read_reg(RUN_STOP, &run) || !read_reg(IE, &interrupts)) return "HCI_READ_FAULT";
    if (!(enable & 1) || (status & 3) != 3 || busy || task_busy || run > 1) return "HOST_NOT_IDLE";
    uint64_t dma = ARENA_PA;
#ifdef DRE_UFS_KERNEL
    if (!arena) return "ARENA_INVALID";
#endif
    if (!dma || (dma & 4095) || dma > UINT64_MAX - ARENA_BYTES) return "ARENA_INVALID";
    if ((dma + ARENA_BYTES - 1) >> 32 && !(s->capabilities & (1u << 24)))
        return "ADDRESS_WIDTH";
    uint64_t old_ring = (uint64_t)lo | ((uint64_t)hi << 32);
    if (old_ring > UINT64_MAX - 1024 ||
        (old_ring < dma + ARENA_BYTES && old_ring + 1024 > dma))
        return "RING_OVERLAP";
    s->safe_to_handoff = 0;
    const char *error = "ATTACH_FAILED";
    if (!write_reg(IE, 0) || !write_reg(RUN_STOP, 0) || !wait_reg(RUN_STOP, 1, 0) ||
        !write_reg(UTRL_LO, (uint32_t)dma) || !write_reg(UTRL_HI, (uint64_t)dma >> 32) ||
        !wait_reg(UTRL_LO, UINT32_MAX, (uint32_t)dma) ||
        !wait_reg(UTRL_HI, UINT32_MAX, (uint64_t)dma >> 32) ||
        !write_reg(RUN_STOP, 1) || !wait_reg(RUN_STOP, 1, 1)) goto restore;
    if (block_out) {
        error = submit(s, 0x28, lba);
        goto restore;
    }
    if (block_in) {
        error = submit_data(s, 0x2a, lba, block_in);
        if (!error) error = submit(s, 0x35, 0);
        goto restore;
    }
    if (flush) { error = submit(s, 0x35, 0); goto restore; }
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
    if (s->guard && (s->guard_lost || !s->guard(s->guard_cookie))) return "DMA_GUARD_CHANGED";
    s->safe_to_handoff = 1;
    if (!error && block_out)
        for (unsigned i = 0; i < BLOCK; i++) block_out[i] = DMA_DATA[i];
    clear_arena();
    return error ? error : block_in ? "WRITE_PASS" : flush ? "FLUSH_PASS" : block_out ? "READ_PASS" : "GPT_PASS";
}

const char *dre_ufs_read_guarded(const struct dre_dma_route *route, struct dre_ufs_read_state *s,
                               dre_ufs_dma_guard guard, void *cookie)
{
    /* The probe caller need not initialize its output structure. */
    *s = (struct dre_ufs_read_state){0};
    return transaction(route, s, guard, cookie, 0, 0, 0, 0);
}

const char *dre_ufs_read_block_guarded(const struct dre_dma_route *route,
                                     struct dre_ufs_read_state *s,
                                     dre_ufs_dma_guard guard, void *cookie,
                                     uint32_t lba, uint8_t out[4096])
{
    if (!out) return "BUFFER_INVALID";
    return transaction(route, s, guard, cookie, lba, out, 0, 0);
}

static int writable_geometry(const struct dre_ufs_read_state *s)
{
    /* read_gpt accepts at most128 entries x128 bytes: reserve both4K GPT
     * arrays plus their headers even if the source table is smaller. */
    return s && s->block_size == BLOCK && s->last_lba > 10 &&
        s->userdata_first >= 6 && s->userdata_blocks &&
        s->userdata_first < s->last_lba - 4 &&
        s->userdata_blocks <= s->last_lba - 4 - s->userdata_first;
}
const char *dre_ufs_write_userdata_guarded(const struct dre_dma_route *route,
    struct dre_ufs_read_state *s, dre_ufs_dma_guard guard, void *cookie,
    uint64_t relative, const uint8_t data[4096])
{
    if (!data || !guard) return "WRITE_GUARD_REQUIRED";
    if (!writable_geometry(s) || relative >= s->userdata_blocks ||
        s->userdata_first + relative > UINT32_MAX) return "WRITE_RANGE";
    return transaction(route, s, guard, cookie, (uint32_t)(s->userdata_first + relative), 0, data, 0);
}
const char *dre_ufs_flush_guarded(const struct dre_dma_route *route,
    struct dre_ufs_read_state *s, dre_ufs_dma_guard guard, void *cookie)
{
    if (!guard || !writable_geometry(s)) return "WRITE_GUARD_REQUIRED";
    return transaction(route, s, guard, cookie, 0, 0, 0, 1);
}
