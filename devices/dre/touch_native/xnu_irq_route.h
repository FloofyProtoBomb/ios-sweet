/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only native binding. */
#ifndef DRE_TOUCH_XNU_IRQ_ROUTE_H
#define DRE_TOUCH_XNU_IRQ_ROUTE_H
#if defined(DRE) && defined(__cplusplus)
#include <IOKit/IOService.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include "native.h"

/* Captures an ALREADY qualified, exclusive logical GPIO22 nub supplied by the
 * actual native IRQ owner. This does not construct a GIC/TLMM controller,
 * install a CPU interrupt handler, interpret a Linux DT vector, route a PDC,
 * seize an existing vector or prove physical IRQ delivery.
 *
 * Calls are on one sleepable owner work loop, except fastMatches in hard IRQ.
 * The mandatory native guard freezes the real parent route, controller and
 * metadata while any source/reference is held, including its registration in
 * the platform's actual controller namespace. Publication must have resolved
 * the real IOService source cache AFTER that registration. Missing/unresolved
 * caches refuse without the unbounded lookUpInterruptController wait.
 * A retained object alone does
 * not freeze registration, clocks, panel power or a foreign driver.
 */
struct DRETouchIRQRoute {
    IOService *nub;
    IOInterruptController *controller;
    IOPlatformExpert *platform;
    OSArray *controllerList,*specifierList;
    OSSymbol *controllerName;
    OSData *specifier;
    void *cookie;
    int (*guard)(void *);
    unsigned source,sources,bytes;
    uint32_t words[8];
    bool captured,attempted,registered,uncertain;

    int capture(IOService *,unsigned,IOPlatformExpert *,void *,int (*)(void *));
    int check();
    int registerOwned(OSObject *,IOInterruptAction,void *);
    /* Task context only. This is real controller soft-disable + handler join,
     * NOT a substitute for the adapter's actual TLMM GPIO22 hardware mask. */
    int disableJoin();
    int enableOwned();
    int unregisterOwned();
    int releaseCapture(); /* No hardware calls, only after proven unregister. */
    bool fastMatches(IOService *,int) const;
};
#endif
#endif
