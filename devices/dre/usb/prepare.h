/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
struct dre_usb_prepare { uint32_t address_bits, endpoints, in_endpoints, events; };
/* Read core capabilities and validate unowned setup/data/status descriptors.
 * No controller write, HWO bit, PHY reset, pull-up or enumeration. */
const char *dre_usb_prepare(const void *dt, uintptr_t arena, struct dre_usb_prepare *out);
