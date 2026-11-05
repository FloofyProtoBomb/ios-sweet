/* SPDX-License-Identifier: MIT */
#pragma once
#include "a619_zap.h"
#include "a619_backend.h"
#define DRE_A619_PAS13_SMC64 (1u<<0)
#define DRE_A619_PAS13_SCM_CLOCKS (1u<<1)
#define DRE_A619_PAS13_EXCLUSIVE (1u<<2)
#define DRE_A619_PAS13_RESERVED (1u<<3)
#define DRE_A619_PAS13_FIRMWARE_HASH (1u<<4)
#define DRE_A619_PAS13_NC_MEMORY (1u<<5)
#define DRE_A619_PAS13_PREREQUISITES 63u
struct dre_a619_smc_regs { uint64_t x[8]; };
struct dre_a619_pas13_ops {
    int (*guard)(void *cookie);
    int (*smc64)(void *cookie,struct dre_a619_smc_regs *regs);
    void (*barrier)(void *cookie);
    uint64_t (*now)(void *cookie);
    void *cookie;
};
struct dre_a619_pas13 {
    uint32_t chip_id, prerequisites, tick_hz;
    const void *firmware; size_t firmware_bytes;
    void *metadata_cpu; uint64_t metadata_pa; size_t metadata_capacity;
    void *reserved_cpu; uint64_t reserved_pa; size_t reserved_bytes;
    size_t metadata_bytes;
    uint32_t last_command;
    int64_t transport_status, service_status;
    int initialized, authenticated, unsafe;
};
/* Uses only PAS peripheral 13: supported, init-image, memory-setup,
 * auth-and-reset and shutdown. It never bypasses authentication. The platform
 * must prove SMC64 discovery, SCM clocks, exclusive PAS/GPU ownership, the exact
 * FDT reserved microcode range, hash-verified private firmware and contiguous
 * non-cacheable metadata/load memory. Retain both allocations until successful
 * shutdown; an unresolved SMC/ownership failure requires reset. Serialize SCM.
 * Retry loops are bounded, but secure firmware can itself hang inside SMC.
 * The successful authenticated ZAP state alone does not initialize CP/SQE. */
const char *dre_a619_pas13_authenticate(struct dre_a619_pas13 *state,
    const struct dre_a619_pas13_ops *ops);
const char *dre_a619_pas13_shutdown(struct dre_a619_pas13 *state,
    const struct dre_a619_pas13_ops *ops);
/* Optional raw transport for a real EL1 ARM64 kernel; not called by unit tests.
 * This is a privileged SMC instruction, not a userspace syscall. */
int dre_a619_smc64_arm64(void *cookie,struct dre_a619_smc_regs *regs);
