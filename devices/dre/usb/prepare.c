/* SPDX-License-Identifier: MIT */
#include <libfdt.h>
#include "prepare.h"
#include "transport.h"
#include "../peripheral_state.h"
extern int dre_read32(uintptr_t address, uint32_t *out);
const char *dre_usb_prepare(const void *dt, uintptr_t arena, struct dre_usb_prepare *s)
{
    *s = (struct dre_usb_prepare){0};
    struct dre_usb_detail detail;
    const char *state = dre_usb_detail(dt, &detail);
    if (strcmp(state, "HALTED")) return state;
    if (detail.endpoints || (detail.event_size & 0xffff)) return "RESOURCES_IN_USE";
    uint32_t p0, p1, p3;
    if (!dre_read32(0x04e0c140, &p0) || !dre_read32(0x04e0c144, &p1) ||
        !dre_read32(0x04e0c14c, &p3)) return "CAPS_READ_FAULT";
    s->address_bits = p0 >> 24; s->events = (p1 >> 15) & 63;
    s->endpoints = (p3 >> 12) & 63; s->in_endpoints = (p3 >> 18) & 31;
    if ((p0 & 3) == 1 || (p0 & 3) == 3 || s->address_bits < 32 || s->address_bits > 64 ||
        !s->events || s->endpoints < 2 || s->endpoints > 32 ||
        !s->in_endpoints || s->in_endpoints >= s->endpoints) return "CAPS_UNSUPPORTED";
    struct dre_usb_trb setup, data, status;
    if (strcmp(dre_usb_trb(&setup, 2, arena + 0x1100, 8, s->address_bits), "USB_TRB_OK") ||
        strcmp(dre_usb_trb(&data, 5, arena + 0x1200, 64, s->address_bits), "USB_TRB_OK") ||
        strcmp(dre_usb_trb(&status, 4, 0, 0, s->address_bits), "USB_TRB_OK")) return "TRB_PLAN_FAILED";
    return "TRB_PLAN_OK";
}
