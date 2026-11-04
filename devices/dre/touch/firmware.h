/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { DRE_TOUCH_FW_PARTS = 32 };
struct dre_touch_partition { uint32_t file_offset, address, length, crc; };
struct dre_touch_firmware {
    uint32_t write_length, version, count;
    struct dre_touch_partition parts[DRE_TOUCH_FW_PARTS];
};

/* Bounds-check the NT36672C ILM/DLM/info/overlay layout and version complement.
 * length in the result is a byte count; the vendor file stores count minus one.
 * This does not authenticate firmware or validate its hardware-specific CRC.
 * The caller owns the bytes. On failure *out is unchanged. No hardware access. */
const char *dre_touch_parse_firmware(const uint8_t *bytes, size_t length,
                                     struct dre_touch_firmware *out);
