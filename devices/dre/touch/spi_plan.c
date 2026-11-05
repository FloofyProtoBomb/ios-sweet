/* SPDX-License-Identifier: MIT
 * GPI descriptor layout facts: matching msm_gpi.h / spi-msm-geni.c. */
#include "spi_plan.h"
const char *dre_spi_plan(struct dre_spi_plan *s, uint64_t tx, uint64_t rx,
                         unsigned bytes, unsigned mode, unsigned source, unsigned divider)
{
    *s = (struct dre_spi_plan){0};
    if (!bytes || bytes > 4096 || mode > 3 || source > 7 || !divider || divider > 4095)
        return "SPI_ARGUMENT";
    const uint64_t limit = UINT64_C(1) << 48;
    if (!tx || !rx || tx >= limit || rx >= limit || bytes > limit - tx || bytes > limit - rx ||
        (tx < rx + bytes && rx < tx + bytes)) return "SPI_BUFFER";
    /* Pack 8-bit words in both directions; CPHA/CPOL follow Linux SPI modes.
     * A single GO deasserts CS at completion (no fragmentation flag). */
    unsigned flags = ((mode & 1) ? 16 : 0) | ((mode & 2) ? 32 : 0);
    s->tx[0][0] = 0x03000004 | flags << 8;
    s->tx[0][2] = source << 16 | divider; s->tx[0][3] = 0x00220001;
    s->tx[1][0] = 3; s->tx[1][2] = bytes; s->tx[1][3] = 0x00200801;
    s->tx[2][0] = (uint32_t)tx; s->tx[2][1] = tx >> 32;
    s->tx[2][2] = bytes; s->tx[2][3] = 0x00100200;
    s->rx[0][0] = (uint32_t)rx; s->rx[0][1] = rx >> 32;
    s->rx[0][2] = bytes; s->rx[0][3] = 0x00100000;
    return "SPI_PLAN_OK";
}
