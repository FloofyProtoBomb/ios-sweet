/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
struct dre_usb_trb { uint32_t low, high, bytes, control; };
struct dre_usb_event { uint32_t raw, device, endpoint, type, status, parameters, next; };
/* Build one isolated setup/data/status/bulk TRB. HWO deliberately remains
 * clear: this does not hand memory to hardware or start an endpoint. */
const char *dre_usb_trb(struct dre_usb_trb *out, unsigned type, uint64_t buffer,
                       unsigned bytes, unsigned address_bits);
/* Decode one bounded little-endian event from a caller-owned snapshot.
 * Does not read or acknowledge the controller's event-count register. */
const char *dre_usb_event(const uint8_t *ring, unsigned capacity, unsigned offset,
                         unsigned available, struct dre_usb_event *out);
