#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/* An embedded payload can use the same loader after its board has checked
 * the destination RAM against the live physical memory map. */
const char *loader_boot_range(uintptr_t start, uintptr_t end, const char *extra);
