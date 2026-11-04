#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/* An embedded payload can use the same loader after its board has checked
 * the destination RAM against the live physical memory map. */
const char *loader_boot_range(uintptr_t start, uintptr_t end, const char *extra);

/* Run a board callback on the validated, copied ADT before entering XNU. */
typedef const char *(*loader_adt_fixup_fn)(void *adt, uint32_t size);
const char *loader_boot_range_with_adt(uintptr_t start, uintptr_t end,
    const char *extra, loader_adt_fixup_fn fixup);
