/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
const char *dre_storage_prepare(const void *dt);
const char *dre_storage_fixup(void *adt, uint32_t size);
