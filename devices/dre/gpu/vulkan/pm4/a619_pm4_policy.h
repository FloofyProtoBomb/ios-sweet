/* SPDX-License-Identifier: MIT
 * UNVALIDATED structural-intake subset. Never a complete raw PM4 validator.
 * Constants transcribed from the exact Mesa85fda A6XX XML/PM4 helpers; see
 * sources.json. No code is generated from a different A740 register table.
 */
#pragma once
#include <stdint.h>

#define DRE_PM4_CHIP_ID 0x06010900u
#define DRE_PM4_TYPE4 0x40000000u
#define DRE_PM4_TYPE7 0x70000000u
enum dre_pm4_opcode {
    DRE_PM4_NOP = 0x10,
    DRE_PM4_WAIT_MEM_WRITES = 0x12,
    DRE_PM4_WAIT_FOR_IDLE = 0x26,
    DRE_PM4_MEM_WRITE = 0x3d,
    DRE_PM4_INDIRECT_BUFFER = 0x3f,
    DRE_PM4_MEM_TO_MEM = 0x73
};

/* Register numbers are DWORD offsets, not byte MMIO addresses. Every TYPE4
 * packet is denied. These source-derived privileged windows only improve the
 * refusal reason; holes outside them do NOT become an allowed register list. */
struct dre_pm4_denied_register_range { uint32_t first, last; const char *name; };
static const struct dre_pm4_denied_register_range dre_pm4_denied_registers[] = {
    {0x0000, 0x07ff, "RBBM/clock/reset/debug global state"},
    {0x0800, 0x091f, "CP ring/SQE/protection/context/debug"},
    {0x0e00, 0x0eff, "UCHE global cache/address programming"},
    {0x3000, 0x30ff, "VBIF bus/halt programming"},
    {0xf400, 0xf8ff, "SECVID trust/security programming"},
};
static inline int dre_pm4_register_is_privileged(uint32_t first, uint32_t words)
{
    if (!words || first > UINT32_MAX - (words - 1)) return 1;
    uint32_t last = first + words - 1;
    for (unsigned i = 0; i < sizeof(dre_pm4_denied_registers) / sizeof(dre_pm4_denied_registers[0]); ++i)
        if (first <= dre_pm4_denied_registers[i].last && last >= dre_pm4_denied_registers[i].first) return 1;
    return 0;
}
static inline int dre_pm4_opcode_is_privileged(uint32_t opcode)
{
    switch (opcode) {
    case 0x04: /* internal PKT4 jump table */
    case 0x0a: /* internal IB end */
    case 0x0b: /* internal GMU interrupt */
    case 0x0f: /* internal preemption */
    case 0x21: /* CP_REG_RMW */
    case 0x3e: /* CP_REG_TO_MEM */
    case 0x42: /* CP_MEM_TO_REG */
    case 0x48: /* CP_ME_INIT */
    case 0x4a: /* CP_REG_TO_SCRATCH */
    case 0x4d: /* CP_SCRATCH_TO_REG */
    case 0x53: /* A5XX+ CP_SMMU_TABLE_UPDATE, NOT old A2XX WAIT_REG_GTE */
    case 0x54: /* A6XX CP_CONTEXT_SWITCH */
    case 0x56: /* A6XX CP_SET_PSEUDO_REG */
    case 0x5c: /* A5XX+ CP_CONTEXT_REG_BUNCH */
    case 0x5e: /* CP_CONTEXT_UPDATE */
    case 0x5f: /* CP_SET_PROTECTED_MODE */
    case 0x66: /* CP_SET_SECURE_MODE */
    case 0x69: /* preemption control (not an admitted A619 packet) */
    case 0x6a:
    case 0x6b: /* CP_CONTEXT_SWITCH_YIELD */
    case 0x6f: /* CP_BOOTSTRAP_UCODE */
    case 0x72: /* A6XX CP_REG_TO_MEM_OFFSET_REG */
    case 0x74: /* A6XX CP_REG_TO_MEM_OFFSET_MEM */
    case 0x78: /* CP_REG_WR_NO_CTXT */
        return 1;
    default: return 0;
    }
}
