/* SPDX-License-Identifier: MIT */
#include "storage_abi.h"
uint32_t dre_storage_crc(const void *bytes, unsigned size)
{
    const uint8_t *p = bytes;
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *p++;
        for (unsigned b = 0; b < 8; b++) crc = (crc >> 1) ^ ((0u - (crc & 1)) & 0xedb88320u);
    }
    return ~crc;
}
int dre_storage_abi_valid(const uint64_t w[8])
{
    if (w[0] != DRE_STORAGE_MAGIC || w[7] != dre_storage_crc(w, 56) ||
        w[2] != DRE_STORAGE_ARENA || (w[1] & (DRE_STORAGE_ARENA - 1)) ||
        w[5] < UINT64_C(0x80000000) || !w[6] || w[6] > 96u * 1024 * 1024 ||
        w[6] > UINT64_C(0x100000000) - w[5] || w[5] >= UINT64_C(0x100000000)) return 0;
    uint64_t end = w[5] + w[6];
    if (w[1] < w[5] || w[1] > end || w[2] > end - w[1] ||
        w[3] < UINT64_C(0x80000000) || (w[3] & 7) || w[4] < 40 ||
        w[4] > 2u * 1024 * 1024 || w[3] >= UINT64_C(0x100000000) ||
        w[4] > UINT64_C(0x100000000) - w[3]) return 0;
    return w[3] >= end || w[3] + w[4] <= w[5];
}
