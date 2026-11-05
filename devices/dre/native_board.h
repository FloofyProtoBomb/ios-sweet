/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight source. */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Obtain a private boot-lifetime snapshot of the original Holi FDT using the
 * existing shim's checked ownership handoff. This is kernel-only metadata;
 * never return it through a user client, sysctl, log or published OSData.
 * It contains private boot/device properties. The const view must not change.
 * No UFS command, GPU/touch access, DMA or native provider is initialized. */
int dre_native_board_snapshot(const void **fdt,size_t *bytes);
#ifdef __cplusplus
}
#endif
