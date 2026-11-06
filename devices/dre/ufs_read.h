/* SPDX-License-Identifier: MIT */
#pragma once
#include "dma_route.h"
typedef int (*dre_ufs_dma_guard)(void *cookie);
#define DRE_UFS_FAULT_CAPTURED (1u << 31)
#define DRE_UFS_MAX_READ_BLOCKS 3u

struct dre_ufs_read_state {
    uint32_t capabilities, block_size, ocs, response, status;
    uint64_t last_lba, userdata_first, userdata_blocks;
    unsigned commands;
    int safe_to_handoff;
    dre_ufs_dma_guard guard;
    void *guard_cookie;
    int guard_lost;
    /* Snapshot before abort/restoration destroys the timed-out descriptor. */
    uint32_t last_opcode, command_lba, fault_doorbell, fault_interrupts, fault_hcs;
    uint32_t fault_ocs, fault_uec[5];
    /* Bits 0..2: DB/IS/HCS; 3..7: layer errors; 8: power; 9..10: UTRL.
     * Bit31 means capture attempted, including when register reads failed. */
    uint32_t fault_valid, fault_power, fault_utrl_lo, fault_utrl_hi;
    uint64_t command_ticks;
    unsigned command_blocks;
};

/* Polled, LUN-0-only read test with a private, below-4G-capable DMA arena.
 * Requires a physical DMA route or the explicit revalidated bypass guard, and
 * the caller's
 * existing Holi UFS power/clock/resource checks. Restores the original UTRL.
 * A false safe_to_handoff MUST stop booting XNU until a hardware reset. */
const char *dre_ufs_read_test(const struct dre_dma_route *route,
                            struct dre_ufs_read_state *out);
uintptr_t dre_ufs_probe_arena(void);
unsigned dre_ufs_probe_size(void);
/* Optional opt-in handoff guard; rechecked before/after every command. */
const char *dre_ufs_read_guarded(const struct dre_dma_route *route,
                               struct dre_ufs_read_state *out,
                               dre_ufs_dma_guard guard, void *cookie);
/* Caller serializes access and supplies geometry from a successful probe.
 * Copies one native block to normal CPU memory only after safe restoration. */
const char *dre_ufs_read_block_guarded(const struct dre_dma_route *route,
                                     struct dre_ufs_read_state *state,
                                     dre_ufs_dma_guard guard, void *cookie,
                                     uint32_t lba, uint8_t out[4096]);
/* out must hold count native blocks. The 8 KiB shim arena fits one block. */
const char *dre_ufs_read_blocks_guarded(const struct dre_dma_route *route,
                                      struct dre_ufs_read_state *state,
                                      dre_ufs_dma_guard guard, void *cookie,
                                      uint32_t lba, unsigned count, uint8_t *out);
/* Installer/runtime opt-in only. Relative block is confined to validated
 * userdata; primary and backup GPT are excluded. WRITE(10) is followed by
 * SYNCHRONIZE CACHE before success. Caller must serialize and authorize RW. */
const char *dre_ufs_write_userdata_guarded(const struct dre_dma_route *route,
    struct dre_ufs_read_state *state, dre_ufs_dma_guard guard, void *cookie,
    uint64_t relative_block, const uint8_t data[4096]);
const char *dre_ufs_flush_guarded(const struct dre_dma_route *route,
    struct dre_ufs_read_state *state, dre_ufs_dma_guard guard, void *cookie);
