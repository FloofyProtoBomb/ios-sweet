/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { DRE_TOUCH_CONTACTS = 10, DRE_TOUCH_REPORT_BYTES = 65 };

struct dre_touch_contact {
    uint16_t x, y, pressure, width;
};

struct dre_touch_frame {
    uint16_t active_mask, count;
    /* Indexed by the controller's contact ID minus one, not wire order. */
    struct dre_touch_contact contacts[DRE_TOUCH_CONTACTS];
};

enum dre_touch_result {
    DRE_TOUCH_OK,
    DRE_TOUCH_INVALID_ARGUMENT,
    DRE_TOUCH_SHORT_REPORT,
    DRE_TOUCH_BAD_CHECKSUM,
    DRE_TOUCH_INVALID_CONTACT,
    DRE_TOUCH_OUT_OF_RANGE,
    DRE_TOUCH_FIRMWARE_RECOVERY,
    DRE_TOUCH_WATCHDOG_PATTERN
};

/* Normal-mode NT36672C report payload, after stripping SPI command/dummy bytes.
 * The first 65 bytes include the checksum. Optional trailing debug data is
 * ignored. max_x/max_y are inclusive raw coordinate maxima from firmware.
 * On any error or recovery marker, *out remains unchanged. The caller handles
 * recovery and mode changes; this routine performs no I/O or firmware reset. */
enum dre_touch_result dre_touch_decode(const uint8_t *payload, size_t length,
    uint16_t max_x, uint16_t max_y, struct dre_touch_frame *out);
