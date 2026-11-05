/* SPDX-License-Identifier: MIT -- UNVALIDATED, kernel-only. */
#ifndef DRE_IRQ_PLATFORM_NAMESPACE_H
#define DRE_IRQ_PLATFORM_NAMESPACE_H
#include <IOKit/IOInterruptController.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Atomic with the selected IOPlatformExpert's ACTUAL dictionary lock. No
 * blocking absent-name lookup, no replacement/removal of foreign entries.
 * register requires absence, including rejecting an already equal object.
 * copy_nowait returns a retained real controller or NULL; caller releases. */
IOReturn dre_irq_namespace_register_absent(const OSSymbol *,IOInterruptController *);
IOReturn dre_irq_namespace_unregister_same(const OSSymbol *,IOInterruptController *);
IOInterruptController *dre_irq_namespace_copy_nowait(const OSSymbol *);
/* Real IOService.cpp friend bridge allocates its source/private arrays using
 * the existing PRIVATE lookupInterrupt(false). New private nub/source0 only.
 * Success leaves an EMPTY cache; it never resolves a controller name. */
IOReturn dre_irq_source_cache_allocate(IOService *,unsigned);
/* New private, unpublished nub only. First use the allocation bridge above
 * to allocate IOService's real source/private arrays WITHOUT resolving a name.
 * This atomically verifies the registered controller and binds that empty
 * public controller-implementor cache. Never overwrites another cached source.
 * Caller holds its actual owner mutex and verified immutable property arrays. */
IOReturn dre_irq_namespace_bind_cached_source(const OSSymbol *,IOInterruptController *,
    IOService *,unsigned,OSData *);
#ifdef __cplusplus
}
#endif
#endif
