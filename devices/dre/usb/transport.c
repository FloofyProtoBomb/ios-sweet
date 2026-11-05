/* SPDX-License-Identifier: MIT
 * DWC3 hardware format facts: matching drivers/usb/dwc3/core.h. */
#include "transport.h"
const char *dre_usb_trb(struct dre_usb_trb *s, unsigned type, uint64_t buffer,
                       unsigned bytes, unsigned address_bits)
{
    *s = (struct dre_usb_trb){0};
    if (address_bits < 32 || address_bits > 48) return "USB_ADDRESS_WIDTH";
    if (type != 1 && type != 2 && type != 3 && type != 4 && type != 5) return "USB_TRB_TYPE";
    if ((type == 2 && bytes != 8) || ((type == 1 || type == 5) && (!bytes || bytes > 65535)) ||
        ((type == 3 || type == 4) && (bytes || buffer))) return "USB_TRB_LENGTH";
    uint64_t limit = UINT64_C(1) << address_bits;
    if ((bytes && !buffer) || buffer >= limit || bytes > limit - buffer) return "USB_BUFFER";
    s->low = (uint32_t)buffer; s->high = buffer >> 32; s->bytes = bytes;
    s->control = 0x802 | type << 4; /* IOC + LST, never HWO. */
    return "USB_TRB_OK";
}
const char *dre_usb_event(const uint8_t *ring, unsigned capacity, unsigned offset,
                         unsigned available, struct dre_usb_event *s)
{
    *s = (struct dre_usb_event){0};
    if (!ring || capacity < 16 || capacity > 4096 || (capacity & (capacity - 1)) ||
        offset >= capacity || (offset & 3) || (available & 3) || available > capacity)
        return "USB_EVENT_BOUNDS";
    s->next = offset;
    if (!available) return "USB_EVENT_EMPTY";
    const uint8_t *p = ring + offset;
    uint32_t value = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    s->raw = value; s->next = (offset + 4) & (capacity - 1);
    if (value & 1) {
        s->device = 1; s->type = (value >> 8) & 15; s->parameters = (value >> 16) & 511;
        if (value & 0xfe || value & 0xfe00f000) return "USB_EVENT_UNKNOWN";
    } else {
        s->endpoint = (value >> 1) & 31; s->type = (value >> 6) & 15;
        s->status = (value >> 12) & 15; s->parameters = value >> 16;
        if (value & 0xc00) return "USB_EVENT_UNKNOWN";
    }
    return "USB_EVENT_OK";
}
