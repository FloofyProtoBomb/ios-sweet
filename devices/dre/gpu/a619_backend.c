/* SPDX-License-Identifier: MIT
 * A619-Holi restricted queue backend. Register and packet definitions checked
 * against LineageOS android_kernel_oneplus_sm4350 commit
 * 98a38783f42f5588bf00cd933e299aedc589bc10, adreno_pm4types.h/a6xx_reg.h.
 * This file does not initialize clocks, firmware, trust state or the SMMU. */
#include "a619_backend.h"

static unsigned odd_parity(unsigned value)
{
    value ^= value >> 16; value ^= value >> 8; value ^= value >> 4;
    return (~(0x6996u >> (value & 15))) & 1;
}
uint32_t dre_a619_type7(unsigned opcode, unsigned count)
{
    if (opcode > 127 || count > 0x3fff) return 0;
    return 0x70000000u | (opcode << 16) | (odd_parity(opcode) << 23) |
        count | (odd_parity(count) << 15);
}
static int contained(struct dre_a619_region region, uint64_t address, uint64_t bytes)
{
    /* This first backend restricts GPU VAs to the low 48 bits. */
    return region.bytes && region.gpu < (UINT64_C(1) << 48) &&
        region.bytes <= (UINT64_C(1) << 48) - region.gpu &&
        address >= region.gpu && address - region.gpu <= region.bytes &&
        bytes <= region.bytes - (address - region.gpu);
}
static int overlaps(struct dre_a619_region a, struct dre_a619_region b)
{
    return a.gpu < b.gpu + b.bytes && b.gpu < a.gpu + a.bytes;
}
static const char *poison(struct dre_a619_queue *q, const char *reason)
{ q->poisoned = 1; return reason; }

const char *dre_a619_submit_marker(struct dre_a619_queue *q,
    const struct dre_a619_ops *ops, uint64_t address, uint32_t value, uint64_t budget)
{
    if (!q || !ops || !ops->guard || !ops->read32 || !ops->write32 ||
        !ops->barrier || !ops->now || !budget || budget > UINT64_MAX / 2)
        return "INVALID_ARGUMENT";
    if (q->poisoned) return "RESET_REQUIRED";
    if (q->chip_id != DRE_A619_CHIP_ID) return "WRONG_GPU";
    if (q->ready_mask != DRE_A619_READY_MASK) return "INITIALIZATION_INCOMPLETE";
    if (q->completed > q->submitted || q->submitted == UINT64_MAX || q->completed == UINT64_MAX)
        return "INVALID_COUNTER_STATE";
    if (!q->ring || !q->result || ((uintptr_t)q->ring & 3) || ((uintptr_t)q->result & 3) || q->ring_dwords < 64 ||
        q->ring_dwords > 16384 || (q->ring_dwords & (q->ring_dwords - 1)) ||
        q->write_pointer >= q->ring_dwords ||
        !contained(q->ring_region, q->ring_region.gpu, (uint64_t)q->ring_dwords * 4) ||
        !contained(q->result_region, address, 4) || (address & 3) ||
        (q->ring_region.gpu & 4095) || (q->result_region.gpu & 3) ||
        q->result_region.bytes > 1048576 ||
        !contained(q->result_region, q->result_region.gpu, q->result_region.bytes) ||
        overlaps(q->ring_region, q->result_region)) return "INVALID_MAPPING";
    volatile uint32_t *result = q->result + (address - q->result_region.gpu) / 4;
    if (q->write_pointer + 6 > q->ring_dwords) return "RING_NEEDS_REWIND";
    if (!ops->guard(ops->cookie)) return "OWNERSHIP_REJECTED";
    uint32_t low, high, control, read, write;
    if (!ops->read32(ops->cookie, DRE_A619_RB_BASE, &low) ||
        !ops->read32(ops->cookie, DRE_A619_RB_BASE_HI, &high) ||
        !ops->read32(ops->cookie, DRE_A619_RB_CNTL, &control) ||
        !ops->read32(ops->cookie, DRE_A619_RB_RPTR, &read) ||
        !ops->read32(ops->cookie, DRE_A619_RB_WPTR, &write)) return "REGISTER_READ_FAILED";
    if (((uint64_t)high << 32 | low) != q->ring_region.gpu)
        return "RING_MAPPING_MISMATCH";
    if ((control & 0x3f) > 13 || (2u << (control & 0x3f)) != q->ring_dwords)
        return "RING_SIZE_MISMATCH";
    if (read != write || write != q->write_pointer) return "RING_BUSY";
    if (!ops->guard(ops->cookie)) return "OWNERSHIP_CHANGED";
    unsigned p = q->write_pointer;
    *result = value ^ 1u; /* A stale result cannot satisfy this submission. */
    q->ring[p++] = dre_a619_type7(0x3d, 3); /* CP_MEM_WRITE */
    q->ring[p++] = (uint32_t)address;
    q->ring[p++] = (uint32_t)(address >> 32);
    q->ring[p++] = value;
    q->ring[p++] = dre_a619_type7(0x12, 0); /* CP_WAIT_MEM_WRITES */
    q->ring[p++] = dre_a619_type7(0x26, 0); /* CP_WAIT_FOR_IDLE */
    ops->barrier(ops->cookie);
    if (!ops->guard(ops->cookie)) return poison(q, "OWNERSHIP_CHANGED");
    p &= q->ring_dwords - 1;
    /* A failed MMIO store can still have reached hardware. Keep all pages. */
    if (!ops->write32(ops->cookie, DRE_A619_RB_WPTR, p))
        return poison(q, "PUBLISH_FAILED");
    q->write_pointer = p;
    q->submitted++;
    uint64_t start = ops->now(ops->cookie);
    unsigned polls = 0;
    do {
        if (!ops->guard(ops->cookie)) return poison(q, "OWNERSHIP_LOST");
        if (!ops->read32(ops->cookie, DRE_A619_RB_RPTR, &read))
            return poison(q, "COMPLETION_READ_FAILED");
        ops->barrier(ops->cookie);
        if (read == p && *result == value) {
            if (!ops->guard(ops->cookie)) return poison(q, "OWNERSHIP_LOST");
            q->completed++;
            return "MARKER_COMPLETED";
        }
        if (++polls == 1000000) return poison(q, "CLOCK_STALLED");
    } while (ops->now(ops->cookie) - start < budget);
    return poison(q, "COMPLETION_TIMEOUT");
}
static uint32_t little(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
const char *dre_a619_sqe_view(uint32_t chip, const void *file, size_t bytes,
    struct dre_a619_sqe *view)
{
    if (!view) return "INVALID_ARGUMENT";
    *view = (struct dre_a619_sqe){0};
    if (chip != DRE_A619_CHIP_ID) return "WRONG_GPU";
    if (!file || bytes < 16 || bytes > 1048576 || (bytes & 3)) return "INVALID_FIRMWARE";
    const uint8_t *p = file;
    uint32_t version = little(p + 4);
    if ((version & 15) == 10)
        version = (version & ~0xfffu) | ((little(p + 12) & 0xfff000) >> 12);
    view->code = p + 4; view->bytes = bytes - 4; view->version = version;
    return "SQE_VIEW_READY";
}
