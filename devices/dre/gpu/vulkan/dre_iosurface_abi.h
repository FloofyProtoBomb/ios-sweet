/* SPDX-License-Identifier: MIT
 * Narrow, documented IOSurface C ABI; not a replacement framework.
 * Some iPhoneOS SDKs expose IOSurfaceRef.h rather than the macOS umbrella.
 * The fallback is exactly the two public declarations documented by Apple:
 * https://developer.apple.com/documentation/iosurface/iosurfaceref
 * https://developer.apple.com/documentation/iosurface/iosurfacecreatemachport(_:)
 * No private header is copied. The actual IOSurface framework must resolve
 * IOSurfaceCreateMachPort at strict link time; no weak/dynamic-lookup waiver.
 */
#ifndef DRE_IOSURFACE_ABI_H
#define DRE_IOSURFACE_ABI_H
#if defined(__APPLE__)
#if __has_include(<IOSurface/IOSurfaceRef.h>)
#include <IOSurface/IOSurfaceRef.h>
#elif __has_include(<IOSurface/IOSurface.h>)
#include <IOSurface/IOSurface.h>
#else
#include <mach/mach.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct __IOSurface *IOSurfaceRef;
mach_port_t IOSurfaceCreateMachPort(IOSurfaceRef buffer);
#ifdef __cplusplus
}
#endif
#endif
#endif
#endif
