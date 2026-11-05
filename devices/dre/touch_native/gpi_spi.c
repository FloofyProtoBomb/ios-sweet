/* SPDX-License-Identifier: MIT
 * UNVALIDATED overnight implementation, intentionally absent from kernel build.
 * Register/descriptor facts: OnePlus kernel98a38783, gpi.c/msm_gpi_mmio.h,
 * spi-msm-geni.c/msm-geni-se.h. This is an independent bounded implementation.
 */
#include "gpi_spi.h"
#include "../touch/spi_plan.h"
#include <string.h>

enum { CH_BYTES = 64 * 16, EV_BYTES = (64 << 2) * 16, BOUNCE_BYTES = 4096 };
enum { CH0 = 0x20000, EV0 = 0x21000, CH_DB = 0x22000, EV_DB = 0x22100,
       CH_CMD = 0x23008, EV_CMD = 0x23010, IRQ_TYPE = 0x23080,
       TYPE_MASK = 0x23088, CH_SRC = 0x23090, EV_SRC = 0x23094,
       CH_MASK = 0x23098, EV_MASK = 0x2309c, CH_CLEAR = 0x230a0,
       EV_CLEAR = 0x230a4, IEOB_SRC = 0x230b0, IEOB_MASK = 0x230b8,
       IEOB_CLEAR = 0x230c0, GLOBAL_STATUS = 0x23100, GLOBAL_ENABLE = 0x23108,
       GLOBAL_CLEAR = 0x23110, GENERAL_STATUS = 0x23118,
       GENERAL_ENABLE = 0x23120, GENERAL_CLEAR = 0x23128,
       INTSET = 0x23180, MSI_LOW = 0x23188, MSI_HIGH = 0x2318c,
       ERROR_LOG = 0x23200, SCRATCH0 = 0x23400, SCRATCH1 = 0x23404 };

static enum dre_tn_result error(struct dre_tn_spi *s, enum dre_tn_result result)
{
    if (s->first_error == DRE_TN_OK) s->first_error = result;
    if (s->mutated) s->state = DRE_TN_QUARANTINED;
    return result;
}
static int live(struct dre_tn_spi *s)
{
    if (!s->lease || s->ops.check_lease(s->ops.cookie, s->lease)) {
        error(s, DRE_TN_OWNERSHIP);
        return 0;
    }
    return 1;
}
static uint64_t reg(struct dre_tn_spi *s, unsigned offset)
{
    return s->profile.gpi_ee_base + s->gpii * 0x4000u + offset;
}
static int read_abs(struct dre_tn_spi *s, uint64_t address, uint32_t *value)
{
    if (!live(s)) return 0;
    if (s->ops.read32(s->ops.cookie, s->lease, address, value)) {
        error(s, DRE_TN_MMIO);
        return 0;
    }
    return 1;
}
static int write_abs(struct dre_tn_spi *s, uint64_t address, uint32_t value)
{
    if (!live(s)) return 0;
    /* A reported bus fault does not prove that a posted write had no effect. */
    s->mutated = 1;
    if (s->ops.write32(s->ops.cookie, s->lease, address, value)) {
        error(s, DRE_TN_MMIO);
        return 0;
    }
    s->ops.barrier(s->ops.cookie, DRE_TN_MMIO_FINISH);
    return 1;
}
static int rd(struct dre_tn_spi *s, unsigned off, uint32_t *value)
{ return read_abs(s, reg(s, off), value); }
static int wr(struct dre_tn_spi *s, unsigned off, uint32_t value)
{ return write_abs(s, reg(s, off), value); }
static int fail(struct dre_tn_spi *s, enum dre_tn_result result)
{ error(s, result); return 0; }

static int usable_ops(const struct dre_tn_ops *o)
{
    return o && o->reserve && o->check_lease && o->get_clock && o->alloc_nc &&
        o->verify_nc && o->free_nc && o->read32 && o->write32 && o->barrier &&
        o->ticks && o->ticks_hz && o->delay_us && o->irq_mask_sync && o->release;
}
static int qualified_profile(const struct dre_tn_profile *p)
{
    return p && p->spi_base == 0x04a80000 && p->gpi_base == 0x04a00000 &&
        p->gpi_ee_base == 0x049f0000 && p->stream_id == 0x16 &&
        p->gpi_mask == 31 && p->event_factor == 2 && p->spi_irq == 359 &&
        p->irq_gpio == 22 && p->reset_gpio == 21 && p->cs_gpio == 3 &&
        p->width == 1080 && p->height == 2400 && p->max_hz == 12000000 &&
        p->iova_base == 0x100000 && p->iova_size == 0x100000 &&
        p->gpi_irq[0] == 367 && p->gpi_irq[1] == 368 && p->gpi_irq[2] == 369 &&
        p->gpi_irq[3] == 370 && p->gpi_irq[4] == 371;
}
static int same_clock(const struct dre_tn_clock *a, const struct dre_tn_clock *b)
{
    return a->source == b->source && a->divider == b->divider &&
        a->parent_hz == b->parent_hz;
}
static int clock_guard(struct dre_tn_spi *s, int initial)
{
    struct dre_tn_clock clock = {0};
    uint32_t selected, master, revision, fifo;
    if (!live(s)) return 0;
    if (s->ops.get_clock(s->ops.cookie, s->lease, &clock))
        return fail(s, DRE_TN_PROVIDER);
    /* parent/divider is a conservative upper bound for SCK even on GENI
     * revisions which oversample by2. No new source/divider is selected. */
    if (clock.source > 7 || !clock.divider || clock.divider > 4095 ||
        !clock.parent_hz || clock.parent_hz >
        (uint64_t)s->profile.max_hz * clock.divider ||
        clock.parent_hz < UINT64_C(128000) * clock.divider ||
        (!initial && !same_clock(&clock, &s->clock)))
        return fail(s, DRE_TN_PROFILE);
    if (!read_abs(s, s->profile.spi_base + 0x7c, &selected) ||
        !read_abs(s, s->profile.spi_base + 0x48, &master) ||
        !read_abs(s, s->profile.spi_base + 0x68, &revision) ||
        !read_abs(s, s->profile.spi_base + 0x64, &fifo)) return 0;
    if (selected != clock.source || !(master & 1) ||
        ((master >> 4) & 4095) != clock.divider ||
        ((revision >> 8) & 255) != 1 || !(fifo & 1))
        return fail(s, DRE_TN_PROFILE);
    s->clock = clock;
    return 1;
}
static int verify_blocks(struct dre_tn_spi *s)
{
    for (unsigned i = 0; i != s->blocks; ++i) {
        const struct dre_tn_memory *b = s->memory + i;
        size_t need = i < 2 ? CH_BYTES : (i == DRE_TN_EVENT_RING ? EV_BYTES : BOUNCE_BYTES);
        size_t align = i < 3 ? need : 16;
        if (!b->cpu || !b->allocation || b->bytes != need ||
            (uintptr_t)b->cpu > UINTPTR_MAX - b->bytes ||
            b->iova < s->profile.iova_base ||
            b->iova >= s->profile.iova_base + s->profile.iova_size ||
            b->bytes > s->profile.iova_base + s->profile.iova_size - b->iova ||
            (b->iova & (align - 1)) || ((uintptr_t)b->cpu & 15) ||
            s->ops.verify_nc(s->ops.cookie, s->lease, b, s->profile.stream_id))
            return fail(s, DRE_TN_DMA);
        for (unsigned j = 0; j != i; ++j) {
            const struct dre_tn_memory *a = s->memory + j;
            if ((a->iova < b->iova + b->bytes && b->iova < a->iova + a->bytes) ||
                ((uintptr_t)a->cpu < (uintptr_t)b->cpu + b->bytes &&
                 (uintptr_t)b->cpu < (uintptr_t)a->cpu + a->bytes))
                return fail(s, DRE_TN_DMA);
        }
    }
    return 1;
}
static int fatal_irq(struct dre_tn_spi *s)
{
    uint32_t type, global, general;
    if (!rd(s, IRQ_TYPE, &type) || !rd(s, GLOBAL_STATUS, &global) ||
        !rd(s, GENERAL_STATUS, &general)) return 1;
    s->last_irq_type = type;
    if ((type & ~0x0bu) || global || general) {
        /* Capture, do not clear an unqualified error then keep submitting. */
        (void)rd(s, ERROR_LOG, &s->last_error_log);
        fail(s, DRE_TN_PROTOCOL);
        return 1;
    }
    return 0;
}
static int command(struct dre_tn_spi *s, int event, unsigned ch,
                    unsigned opcode, unsigned expected_state)
{
    unsigned src = event ? EV_SRC : CH_SRC;
    unsigned clear = event ? EV_CLEAR : CH_CLEAR;
    unsigned context = event ? EV0 : CH0 + 0x80u * ch;
    uint32_t pending, state, bit = event ? 1u : 1u << ch;
    if (!rd(s, src, &pending)) return 0;
    if (pending) return fail(s, DRE_TN_PROTOCOL); /* Never accept an old ACK. */
    if (!wr(s, event ? EV_CMD : CH_CMD, opcode << 24 | (event ? 0 : ch))) return 0;
    uint64_t start = s->ops.ticks(s->ops.cookie);
    /* Finite poll count is a second bound if the counter/provider is broken. */
    for (unsigned poll = 0; poll != 40000; ++poll) {
        if (fatal_irq(s) || !rd(s, src, &pending)) return 0;
        if (pending & ~bit) return fail(s, DRE_TN_PROTOCOL);
        if (pending & bit) {
            if (!rd(s, context, &state) || !wr(s, clear, bit)) return 0;
            state = (state >> 20) & 15;
            /* STOP_IN_PROC can raise an intermediate control interrupt. */
            if (!event && opcode == 2 && state == 4) continue;
            /* Dealloc's observed control ACK is authoritative: this GPI
             * revision does not necessarily clear the context state bits. */
            if (opcode != 10 && state != expected_state)
                return fail(s, DRE_TN_PROTOCOL);
            return 1;
        }
        if (s->ops.ticks(s->ops.cookie) - start >= UINT64_C(19200000)) break;
        s->ops.delay_us(s->ops.cookie, 25);
    }
    return fail(s, DRE_TN_TIMEOUT);
}
static int context(struct dre_tn_spi *s, unsigned base, unsigned db,
                    const struct dre_tn_memory *m, uint32_t config)
{
    return wr(s, base, config) && wr(s, base + 4, (uint32_t)m->bytes) &&
        wr(s, base + 8, (uint32_t)m->iova) && wr(s, base + 12, m->iova >> 32) &&
        wr(s, db + 4, m->iova >> 32);
}
static int configure(struct dre_tn_spi *s)
{
    uint32_t tx, rx, ev, pending, type, master;
    if (!rd(s, CH0, &tx) || !rd(s, CH0 + 0x80, &rx) || !rd(s, EV0, &ev) ||
        !rd(s, CH_SRC, &pending) || !rd(s, IRQ_TYPE, &type) ||
        !read_abs(s, s->profile.spi_base + 0x40, &master)) return 0;
    if (((tx | rx | ev) & 0x00f00000) || pending || type || (master & 0x1001))
        return fail(s, DRE_TN_OWNERSHIP);
    if (fatal_irq(s)) return 0;
    /* Only this exclusively reserved GPII is configured; no top-level GPI,
     * firmware image, shared vote, SMMU or unrelated channel is changed. */
    if (!wr(s, TYPE_MASK, 0x4f) || !wr(s, IEOB_MASK, 1) ||
        !wr(s, CH_MASK, 3) || !wr(s, EV_MASK, 1) ||
        !wr(s, GLOBAL_ENABLE, 1) || !wr(s, GENERAL_ENABLE, 15) ||
        !wr(s, MSI_LOW, 0) || !wr(s, MSI_HIGH, 0) ||
        !wr(s, SCRATCH0, 0) || !wr(s, SCRATCH1, 0) || !wr(s, INTSET, 1)) return 0;
    if (!command(s, 1, 0, 0, 1)) return 0;
    s->event_allocated = 1;
    if (!context(s, EV0, EV_DB, s->memory + DRE_TN_EVENT_RING, 0x10010002)) return 0;
    const unsigned event_zero[] = {0x20,0x28,0x2c,0x30,0x34};
    for (unsigned i = 0; i != sizeof(event_zero)/sizeof(event_zero[0]); ++i)
        if (!wr(s, EV0 + event_zero[i], 0)) return 0;
    s->event_rp = 0;
    s->event_wp = EV_BYTES - 16;
    s->ops.barrier(s->ops.cookie, DRE_TN_PUBLISH);
    if (!wr(s, EV_DB, (uint32_t)(s->memory[DRE_TN_EVENT_RING].iova + s->event_wp))) return 0;
    for (unsigned ch = 0; ch != 2; ++ch) {
        if (!command(s, 0, ch, 0, 1)) return 0;
        s->allocated_channels |= 1u << ch;
        unsigned off = CH0 + 0x80u * ch;
        /* TX=OUT1, RX=IN0; paired flag is set only on TX. */
        if (!context(s, off, CH_DB + 8u * ch, s->memory + ch,
                      0x10000002u | (!ch ? 8u : 0u)) ||
            !wr(s, off + 0x60, (!ch ? 0x10000u : 0) | 0x10u) ||
            !wr(s, off + 0x64, 0) || !wr(s, off + 0x68, 0) ||
            !wr(s, off + 0x6c, 0) || !wr(s, off + 0x5c, 1)) return 0;
    }
    s->ops.barrier(s->ops.cookie, DRE_TN_PUBLISH);
    return command(s, 0, 0, 1, 2) && command(s, 0, 1, 1, 2);
}
static enum dre_tn_result release_unstarted(struct dre_tn_spi *s)
{
    /* Called only before the first MMIO write or after acknowledged teardown. */
    while (s->blocks) {
        struct dre_tn_memory *m = s->memory + s->blocks - 1;
        if (s->ops.free_nc(s->ops.cookie, s->lease, m)) {
            s->state = DRE_TN_QUARANTINED;
            return error(s, DRE_TN_DMA);
        }
        *m = (struct dre_tn_memory){0};
        --s->blocks;
    }
    if (s->lease && s->ops.release(s->ops.cookie, s->lease)) {
        s->state = DRE_TN_QUARANTINED;
        return error(s, DRE_TN_OWNERSHIP);
    }
    s->lease = 0;
    s->state = DRE_TN_CLOSED;
    return s->first_error;
}
enum dre_tn_result dre_tn_spi_open(struct dre_tn_spi *s,
    const struct dre_tn_profile *profile, const struct dre_tn_ops *ops)
{
    if (!s || s->state != DRE_TN_CLOSED || s->lease || s->blocks)
        return DRE_TN_STATE;
    if (!qualified_profile(profile)) return DRE_TN_PROFILE;
    if (!usable_ops(ops)) return DRE_TN_PROVIDER;
    *s = (struct dre_tn_spi){ .ops = *ops, .profile = *profile };
    if (s->ops.ticks_hz(s->ops.cookie) != 19200000) return DRE_TN_PROVIDER;
    if (s->ops.reserve(s->ops.cookie, profile, &s->lease, &s->gpii)) {
        s->first_error = DRE_TN_OWNERSHIP;
        return s->lease ? release_unstarted(s) : DRE_TN_OWNERSHIP;
    }
    s->state = DRE_TN_RESERVED;
    if (!s->lease || s->gpii > 4 || !(profile->gpi_mask & (1u << s->gpii)) ||
        !live(s) || s->ops.irq_mask_sync(s->ops.cookie, s->lease) || !clock_guard(s, 1)) {
        if (s->first_error == DRE_TN_OK) s->first_error = DRE_TN_OWNERSHIP;
        return release_unstarted(s);
    }
    for (unsigned i = 0; i != DRE_TN_BLOCKS; ++i) {
        size_t size = i < 2 ? CH_BYTES : (i == DRE_TN_EVENT_RING ? EV_BYTES : BOUNCE_BYTES);
        /* Alloc failure must return a zero block, or a releasable partial block.
         * Provider retains failed unmap allocations itself if retirement fails. */
        int r = s->ops.alloc_nc(s->ops.cookie, s->lease, size, i < 3 ? size : 16,
                               s->memory + i);
        if (s->memory[i].allocation) ++s->blocks;
        if (r || s->blocks != i + 1 || !verify_blocks(s)) {
            if (s->first_error == DRE_TN_OK) s->first_error = DRE_TN_DMA;
            return release_unstarted(s);
        }
        for (size_t j = 0; j != size; ++j) s->memory[i].cpu[j] = 0;
    }
    s->ops.barrier(s->ops.cookie, DRE_TN_PUBLISH);
    if (!configure(s)) {
        if (s->first_error == DRE_TN_OK) error(s, DRE_TN_PROTOCOL);
        if (!s->mutated) return release_unstarted(s);
        s->state = DRE_TN_QUARANTINED;
        return s->first_error;
    }
    s->state = DRE_TN_RUNNING;
    return DRE_TN_OK;
}
static unsigned next(unsigned offset, unsigned bytes)
{ return (offset + 16) & (bytes - 1); }
static void put_tre(struct dre_tn_memory *b, unsigned offset, const uint32_t words[4])
{
    for (unsigned i = 0; i != 4; ++i)
        for (unsigned j = 0; j != 4; ++j)
            b->cpu[offset + 4*i + j] = (uint8_t)(words[i] >> (8*j));
}
static uint32_t load_word(const volatile uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int event(struct dre_tn_spi *s)
{
    const volatile uint8_t *p = s->memory[DRE_TN_EVENT_RING].cpu + s->event_rp;
    for (unsigned i = 0; i != 4; ++i) s->last_event[i] = load_word(p + i*4);
    uint32_t *d = s->last_event;
    unsigned type = (d[3] >> 16) & 255, ch = d[3] >> 24;
    if (ch > 1) return fail(s, DRE_TN_PROTOCOL);
    /* A genuine notification is recorded and recycled, but never treated as
     * completion. Immediate/stale records are impossible in this fresh lease. */
    if (type == 0x31) return 1;
    if (type != 0x22 || s->state != DRE_TN_INFLIGHT ||
        (s->completion_mask & (1u << ch))) return fail(s, DRE_TN_PROTOCOL);
    uint64_t pointer = (uint64_t)d[0] | (uint64_t)d[1] << 32;
    if (pointer != s->expected_tre[ch] || (d[2] >> 24) != 2 ||
        (d[2] & 0xffffff) != s->transfer_bytes || (d[3] & 0xffff))
        return fail(s, DRE_TN_PROTOCOL);
    s->completion_mask |= 1u << ch;
    return 1;
}
static int drain(struct dre_tn_spi *s)
{
    uint32_t producer;
    const uint64_t base = s->memory[DRE_TN_EVENT_RING].iova;
    for (unsigned count = 0; count != 512; ++count) {
        if (!rd(s, EV0 + 0x10, &producer)) return 0;
        /* Rings lie entirely inside the qualified low32bit IOVA pool. */
        if (producer < base || producer >= base + EV_BYTES || ((producer-base) & 15))
            return fail(s, DRE_TN_PROTOCOL);
        if (producer == (uint32_t)(base + s->event_rp)) {
            if (!wr(s, IEOB_CLEAR, 1) || !rd(s, EV0 + 0x10, &producer)) return 0;
            if (producer == (uint32_t)(base + s->event_rp)) return 1;
            continue; /* Producer raced the interrupt clear. */
        }
        s->ops.barrier(s->ops.cookie, DRE_TN_CONSUME);
        if (!event(s)) return 0;
        s->event_rp = next(s->event_rp, EV_BYTES);
        s->event_wp = next(s->event_wp, EV_BYTES);
        s->ops.barrier(s->ops.cookie, DRE_TN_PUBLISH);
        if (!wr(s, EV_DB, (uint32_t)(base + s->event_wp))) return 0;
    }
    return fail(s, DRE_TN_PROTOCOL); /* IRQ/event flood does not hold worker forever. */
}
static int engine_idle(struct dre_tn_spi *s)
{
    uint32_t status, irq;
    if (!read_abs(s, s->profile.spi_base + 0x40, &status) ||
        !read_abs(s, s->profile.spi_base + 0x610, &irq)) return 0;
    if (irq & 0x3300003eu) return fail(s, DRE_TN_PROTOCOL);
    return !(status & 0x1001);
}
enum dre_tn_result dre_tn_spi_transfer(struct dre_tn_spi *s,
    const uint8_t *tx, uint8_t *rx, size_t bytes)
{
    if (!s || !tx || !rx || !bytes || bytes > BOUNCE_BYTES) return DRE_TN_ARGUMENT;
    if (s->state == DRE_TN_QUARANTINED) return DRE_TN_POISONED;
    if (s->state != DRE_TN_RUNNING) return DRE_TN_STATE;
    if ((uintptr_t)tx > UINTPTR_MAX-bytes || (uintptr_t)rx > UINTPTR_MAX-bytes)
        return DRE_TN_ARGUMENT;
    for (unsigned i = 0; i != s->blocks; ++i) {
        uintptr_t base = (uintptr_t)s->memory[i].cpu;
        size_t size = s->memory[i].bytes;
        if (((uintptr_t)tx < base+size && base < (uintptr_t)tx+bytes) ||
            ((uintptr_t)rx < base+size && base < (uintptr_t)rx+bytes))
            return DRE_TN_ARGUMENT;
    }
    if (!live(s) || !clock_guard(s, 0) || !verify_blocks(s) || fatal_irq(s) ||
        !drain(s) || !engine_idle(s)) return error(s, s->first_error ? s->first_error : DRE_TN_STATE);
    uint32_t state;
    for (unsigned ch = 0; ch != 2; ++ch)
        if (!rd(s, CH0 + ch*0x80, &state) || ((state >> 20) & 15) != 2)
            return error(s, s->first_error ? s->first_error : DRE_TN_STATE);
    struct dre_spi_plan plan;
    const char *planned = dre_spi_plan(&plan, s->memory[DRE_TN_TX_BYTES].iova,
        s->memory[DRE_TN_RX_BYTES].iova, (unsigned)bytes, 0,
        s->clock.source, s->clock.divider);
    if (strcmp(planned, "SPI_PLAN_OK")) return error(s, DRE_TN_ARGUMENT);
    for (size_t i = 0; i != bytes; ++i) {
        s->memory[DRE_TN_TX_BYTES].cpu[i] = tx[i];
        s->memory[DRE_TN_RX_BYTES].cpu[i] = 0;
    }
    for (unsigned i = 0; i != 3; ++i) {
        put_tre(s->memory + DRE_TN_TX_RING, s->tx_wp, plan.tx[i]);
        s->expected_tre[0] = s->memory[DRE_TN_TX_RING].iova + s->tx_wp;
        s->tx_wp = next(s->tx_wp, CH_BYTES);
    }
    put_tre(s->memory + DRE_TN_RX_RING, s->rx_wp, plan.rx[0]);
    s->expected_tre[1] = s->memory[DRE_TN_RX_RING].iova + s->rx_wp;
    s->rx_wp = next(s->rx_wp, CH_BYTES);
    s->completion_mask = 0;
    s->transfer_bytes = (unsigned)bytes;
    s->state = DRE_TN_INFLIGHT;
    s->ops.barrier(s->ops.cookie, DRE_TN_PUBLISH);
    /* Receive descriptors must be available before TX's GO can clock bytes. */
    if (!wr(s, CH_DB + 8, (uint32_t)(s->memory[DRE_TN_RX_RING].iova + s->rx_wp)) ||
        !wr(s, CH_DB, (uint32_t)(s->memory[DRE_TN_TX_RING].iova + s->tx_wp)))
        return s->first_error;
    uint64_t start = s->ops.ticks(s->ops.cookie);
    for (unsigned poll = 0; poll != 40000; ++poll) {
        if (fatal_irq(s) || !drain(s)) return s->first_error;
        if (s->completion_mask == 3 && engine_idle(s)) {
            /* Both exact DMA EOTs + GENI idle retire this request; no memcpy
             * occurs on a partially completed, stale or failed transfer. */
            s->ops.barrier(s->ops.cookie, DRE_TN_CONSUME);
            for (size_t i = 0; i != bytes; ++i) rx[i] = s->memory[DRE_TN_RX_BYTES].cpu[i];
            s->state = DRE_TN_RUNNING;
            return DRE_TN_OK;
        }
        if (s->state == DRE_TN_QUARANTINED) return s->first_error;
        if (s->ops.ticks(s->ops.cookie) - start >= UINT64_C(19200000)) break;
        s->ops.delay_us(s->ops.cookie, 25);
    }
    /* In-flight buffers remain mapped/pinned. No blind abort, reset, unmap,
     * firmware retry or clock-drop is used to conceal this unknown lifetime. */
    return error(s, DRE_TN_TIMEOUT);
}
enum dre_tn_result dre_tn_spi_close(struct dre_tn_spi *s)
{
    if (!s) return DRE_TN_ARGUMENT;
    if (s->state == DRE_TN_CLOSED && !s->lease && !s->blocks) return DRE_TN_OK;
    if (s->state == DRE_TN_QUARANTINED || s->state == DRE_TN_INFLIGHT)
        return DRE_TN_POISONED;
    if (s->state != DRE_TN_RUNNING || !live(s)) return DRE_TN_STATE;
    if (s->ops.irq_mask_sync(s->ops.cookie, s->lease)) return error(s, DRE_TN_OWNERSHIP);
    if (fatal_irq(s) || !drain(s) || !engine_idle(s))
        return error(s, s->first_error ? s->first_error : DRE_TN_STATE);
    /* STOP is used only after completed transfer retirement, never as an
     * assumed recovery from unknown DMA/cache/firmware references. */
    for (unsigned ch = 0; ch != 2; ++ch) {
        if (!command(s, 0, ch, 2, 3) || !command(s, 0, ch, 10, 0)) return s->first_error;
        s->allocated_channels &= ~(1u << ch);
    }
    if (!command(s, 1, 0, 10, 0)) return s->first_error;
    s->event_allocated = 0;
    if (!wr(s, TYPE_MASK, 0) || !wr(s, IEOB_MASK, 0) || !wr(s, CH_MASK, 0) ||
        !wr(s, EV_MASK, 0) || !wr(s, GLOBAL_ENABLE, 0) ||
        !wr(s, GENERAL_ENABLE, 0) || !wr(s, INTSET, 0)) return s->first_error;
    if (s->ops.irq_mask_sync(s->ops.cookie, s->lease)) return error(s, DRE_TN_OWNERSHIP);
    s->ops.barrier(s->ops.cookie, DRE_TN_CONSUME);
    return release_unstarted(s);
}
