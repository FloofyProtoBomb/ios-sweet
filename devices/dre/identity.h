#pragma once
#include <stdint.h>

/* Detect the real hardware serial without requiring a personalized image. */
const char *dre_identity_detect(const void *live_fdt);
const char *dre_identity_serial(void);
const char *dre_identity_fixup(void *adt, uint32_t size);
