/* SPDX-License-Identifier: MIT; UNVALIDATED source-only draft. */
#pragma once
#include "dre_darwin.h"
#if defined(__APPLE__)
#include "dre_iosurface_abi.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The wrapper creates its own real IOSurface Mach send right, retains the
 * kernel backing through ABI1, then deallocates just that temporary right.
 * It never calls IOSurfaceGetBaseAddress or imports client/host pointers.
 * A typed error can still return a retained *out which must be closed.
 * There is no GPU import/readiness promise associated with this operation.
 */
enum dre_darwin_result dre_darwin_surface_retain_iosurface(struct dre_darwin *,
    IOSurfaceRef, uint32_t access, struct dre_darwin_surface **out);
#ifdef __cplusplus
}
#endif
#endif
