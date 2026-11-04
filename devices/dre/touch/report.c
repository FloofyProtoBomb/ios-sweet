/* SPDX-License-Identifier: MIT
 * Original payload decoder using the NT36672C wire-format facts in OnePlus's
 * published driver. Reference: LineageOS android_kernel_oneplus_sm4350 commit
 * 98a38783f42f5588bf00cd933e299aedc589bc10, NT36672C_noflash driver.
 * This file contains no Linux driver implementation or hardware-access code. */
#include "report.h"

enum dre_touch_result dre_touch_decode(const uint8_t *payload, size_t length,
    uint16_t max_x, uint16_t max_y, struct dre_touch_frame *out)
{
    if (!payload || !out || !max_x || !max_y || max_x > 4095 || max_y > 4095)
        return DRE_TOUCH_INVALID_ARGUMENT;
    if (length < DRE_TOUCH_REPORT_BYTES) return DRE_TOUCH_SHORT_REPORT;
    unsigned recovery = 0, watchdog = 0;
    for (unsigned i = 0; i < 6; i++) {
        recovery += payload[i] == 0x77;
        watchdog += payload[i] == 0xfd || payload[i] == 0xfe;
    }
    /* These controller markers take precedence over the normal checksum. */
    if (recovery == 6) return DRE_TOUCH_FIRMWARE_RECOVERY;
    if (watchdog == 6) return DRE_TOUCH_WATCHDOG_PATTERN;
    unsigned sum = 0;
    for (unsigned i = 0; i < DRE_TOUCH_REPORT_BYTES; i++) sum += payload[i];
    if (sum & 0xff) return DRE_TOUCH_BAD_CHECKSUM;

    struct dre_touch_frame next = {0};
    for (unsigned slot = 0; slot < DRE_TOUCH_CONTACTS; slot++) {
        const uint8_t *record = payload + slot * 6;
        unsigned state = record[0] & 7;
        if (state != 1 && state != 2) continue;
        unsigned id = record[0] >> 3;
        if (!id || id > DRE_TOUCH_CONTACTS) return DRE_TOUCH_INVALID_CONTACT;
        uint16_t bit = (uint16_t)(1u << (id - 1));
        if (next.active_mask & bit) return DRE_TOUCH_INVALID_CONTACT;
        unsigned x = (unsigned)record[1] * 16 + (record[3] >> 4);
        unsigned y = (unsigned)record[2] * 16 + (record[3] & 15);
        if (x > max_x || y > max_y) return DRE_TOUCH_OUT_OF_RANGE;
        unsigned pressure = record[5];
        /* Extended pressure is attached to the first two wire slots. */
        if (slot < 2) pressure += (unsigned)payload[62 + slot] * 256;
        if (pressure > 1000) pressure = 1000;
        struct dre_touch_contact *contact = &next.contacts[id - 1];
        contact->x = (uint16_t)x;
        contact->y = (uint16_t)y;
        contact->pressure = pressure ? (uint16_t)pressure : 1;
        contact->width = record[4] ? record[4] : 1;
        next.active_mask |= bit;
        next.count++;
    }
    *out = next;
    return DRE_TOUCH_OK;
}
