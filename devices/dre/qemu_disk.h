/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* Explicit virtual test provider; never a physical storage transport.
 * Caller must pass enabled=1 only after checking all QEMU boot arguments
 * and synthetic ownership. Every semihost trap independently requires the
 * QEMU virt 62.5 MHz counter frequency. The caller serializes all access. */
int dre_qemu_disk_open(int enabled, int writable, uint64_t *bytes);
int dre_qemu_disk_read(uint64_t relative_block, uint8_t out[4096]);
int dre_qemu_disk_write(uint64_t relative_block, const uint8_t in[4096]);
/* Close/reopen flushes QEMU stdio only, NOT host/hardware durability proof. */
int dre_qemu_disk_flush(void);
int dre_qemu_disk_close(void);
const char *dre_qemu_disk_status(void);
