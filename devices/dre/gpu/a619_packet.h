#pragma once
#include <stdint.h>

/* Adreno CP_TYPE7 / CP_MEM_WRITE. The destination must be a GPU virtual
 * address in the submitting context. The caller manages coherency and fences. */
static inline void a619_write_marker(uint32_t ib[4], uint64_t gpu_addr, uint32_t value)
{
    /* count=3 has even popcount; opcode 0x3d has odd popcount. */
    ib[0] = (7u << 28) | (0x3du << 16) | (1u << 15) | 3u;
    ib[1] = (uint32_t)gpu_addr;
    ib[2] = (uint32_t)(gpu_addr >> 32);
    ib[3] = value;
}
