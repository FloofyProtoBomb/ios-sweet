/* Register definitions and gating semantics from gcc-holi.c / clk-branch.c
 * at LineageOS kernel commit 98a38783f42f5588bf00cd933e299aedc589bc10. */
#include "ufs_clock.h"

extern int dre_read32(uintptr_t addr, uint32_t *out);
extern int dre_write32(uintptr_t addr, uint32_t value);

#define GCC UINT64_C(0x01400000)
#define ENABLE (1u << 0)
#define HWCG (1u << 1)
#define CLK_OFF (1u << 31)
#define FSM_MASK (7u << 28)
#define FSM_ON (2u << 28)

/* Fabric first, then the register interface, host bus, and UniPro core. */
static const uint32_t cbcr[DRE_UFS_CLOCKS] = {0x45098, 0x45014, 0x45010, 0x45040};

static uint64_t ticks(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static int ready(uint32_t value, unsigned index)
{
    if (!(value & ENABLE)) return 0;
    /* The fabric branch has no HWCG field. Linux skips halt checks for
     * the three UFS branches while their hardware gating mode is enabled. */
    return !(value & CLK_OFF) || (value & (CLK_OFF | FSM_MASK)) == FSM_ON ||
           (index != 0 && (value & HWCG));
}

static int rollback(unsigned changed)
{
    int ok = 1;
    for (int i = DRE_UFS_CLOCKS - 1; i >= 0; i--) {
        if (!(changed & (1u << i))) continue;
        uint32_t value;
        if (!dre_read32(GCC + cbcr[i], &value) ||
            !dre_write32(GCC + cbcr[i], value & ~ENABLE)) ok = 0;
    }
    return ok;
}

const char *dre_ufs_clocks(struct dre_ufs_clock_state *s, int enable)
{
    for (unsigned i = 0; i < DRE_UFS_CLOCKS; i++) {
        if (!dre_read32(GCC + cbcr[i], &s->before[i])) return "READ_FAULT";
        s->after[i] = s->before[i];
    }
    if (!dre_read32(GCC + 0x45020, &s->axi_cmd) ||
        !dre_read32(GCC + 0x45024, &s->axi_cfg) ||
        !dre_read32(GCC + 0x45060, &s->unipro_cmd) ||
        !dre_read32(GCC + 0x45064, &s->unipro_cfg)) return "ROOT_READ_FAULT";
    unsigned changed = 0;
    const char *failure = "TIMEOUT";
    for (unsigned i = 0; i < DRE_UFS_CLOCKS; i++) {
        uint32_t value = s->before[i];
        if (!enable && !(value & ENABLE)) return "DISABLED";
        if (enable && !(value & ENABLE)) {
            /* Include this branch in rollback even if the store reports
             * an abort, since a failed access can have reached hardware. */
            changed |= 1u << i;
            if (!dre_write32(GCC + cbcr[i], value | ENABLE)) {
                failure = "WRITE_FAULT";
                goto failed;
            }
        }
        uint64_t start = ticks();
        do {
            if (!dre_read32(GCC + cbcr[i], &value)) {
                failure = "READ_FAULT";
                goto failed;
            }
            s->after[i] = value;
            if (ready(value, i)) break;
        } while (enable && ticks() - start < 38400); /* 2 ms at 19.2 MHz */
        if (!ready(value, i)) {
            if (!enable) return "HALTED";
            goto failed;
        }
    }
    return changed ? "ENABLED" : "READY";
failed:
    if (!rollback(changed)) return "ROLLBACK_FAULT";
    /* Report the post-rollback state, rather than the last poll value. */
    for (unsigned i = 0; i < DRE_UFS_CLOCKS; i++)
        if (!dre_read32(GCC + cbcr[i], &s->after[i])) return "READ_FAULT";
    return failure;
}
