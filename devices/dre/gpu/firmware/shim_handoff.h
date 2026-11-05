/* SPDX-License-Identifier: MIT -- UNVALIDATED optional shim handoff. */
#pragma once
#include <stdint.h>
const char *dre_gpu_fw_prepare(const void *dt);
const char *dre_gpu_fw_fixup(void *adt,uint32_t size);
