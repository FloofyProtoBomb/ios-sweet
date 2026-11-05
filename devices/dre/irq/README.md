# Native Holi SPI route draft — UNVALIDATED

Source implementation only. No recipe, test, compile, emulator, CI or phone
operation has run for this slice. It supplies neither a default native broker
nor interrupt/graphics/input readiness.

`DRENativeGIC.cpp` is an actual `IOInterruptController` implementation connected
to the selected DRE `sleh_irq` branch through `sleh_spi.inc`. It initially admits
only original-FDT GPU SPI177/INTID209 and TLMM SPI227/INTID259, both level-high.
These are physical vectors, distinct from each nub's local source0. Its real
private nub has the standard IOKit controller/specifier properties and an
actually allocated/resolved source cache. It is attached to its controller and
returned to the retained native owner; it is not exposed to arbitrary driver
matching. The controller enters the actual IOKit namespace only after a real
external per-SPI lease and a registered live SLEH route exist.

`pe_spi.inc` observes the **existing** `pe_init_fiq` GICD0x0f200000+0x10000 and
GICR0x0f240000+0x100000 mappings. It never creates a second mapping, resets the
distributor/redistributors or rewrites group, trigger, affinity, priority, PMR,
SGI/PPI, pending or active state. Its only writes are the owned SPI's one W1S
ISENABLER bit or W1C ICENABLER bit. Acquisition requires an already disabled,
inactive and nonpending source. Trigger/route/priority must remain unchanged.
The actual broker must hold the selected CPU online and qualify its interrupt
security/priority policy. A RAZ IGROUPR read in NS mode cannot establish group
ownership: `group1ns_authority` must acquire actual platform/EL3 authority.

The SLEH hook changes one condition in the OTHER branch. An unowned vector
follows the original `cdp->interrupt_handler` path. Timer, SGI, PPI, spurious
handling, original IAR read, existing single EOIR write and interrupt
prologue/epilogue are preserved. Claimed SPI callbacks never ACK/EOI/DIR. A
CAS-protected reader gate prevents cookie access after close; release also
waits for the real GIC ACTIVE bit to clear **after SLEH's EOI**, not merely the
return of the child callback. Faults/timeouts retain actual lifetime resources.
No shared-controller conversion or synthetic interrupt injection is supported.

`dre_spi.h` is the shared kernel-only API. All slow calls require the same actual
native board ownership mutex, and the broker's fast lease proof cannot sleep or
take it. A genuine broker must acquire each SPI, its producer/reset/power/pin
policy, CPU route and security authority; a local ledger or FDT node supplies
none of that. Invalidation must first mask/join before access authority is lost.
The IRQ wrapper touches only GIC registers, never a powered-off GPU peripheral.

TLMM259 is a **shared summary source**. GPIO22 ownership alone does not authorize
claiming it. A genuine TLMM broker must exclude/quiesce every foreign summary
producer or supply a real common dispatcher. This initial exclusive driver
implements the former only. The captured wake parent is Qualcomm MPM, with
vMPM0x045f01b8+0x1000 and parent INTID229; GPIO22's wake mapping21 is not INTID21.
This driver never takes that wake route or changes MPM/PDC/wake state.

`platform_namespace.inc` uses the actual `IOPlatformExpert.cpp` dictionary lock
to register only if absent, remove only if identical, and copy a retained real
controller without waiting. This avoids the original register API's silent
replacement and the original lookup API's unbounded absent-name wait. The
`IOService.cpp` friend bridge allocates its real private source arrays through
the existing private `lookupInterrupt(false)`; atomic namespace binding fills
only an empty source cache. No layout/vtable change or guessed private array is
used. These helpers are also available to the native TLMM controller.

An actually published GIC controller is currently boot-resident. This slice has
no genuine general IOService client-termination/join provider; `controller_close`
refuses disposal after publication. A route may release only after its handler
is unregistered, hardware is masked/inactive/nonpending, all SLEH readers are
joined, its producer is quiet and the actual external broker releases its lease.
Unknown or partially retained acquisition/release stays visible to the caller.

Integration, explicitly deferred:

- Copy the eight source/header/include files selected by `source_hooks.py` to
  `iokit/Kernel/arm/dre/irq/`. Compile only `DRENativeGIC.cpp`, with the selected
  board configuration and existing libfdt/native-board snapshot implementation.
  The copied TU needs `<libfdt.h>` rewritten to `"../libfdt/libfdt.h"` and the
  normal kernel `<string.h>` rewrite, matching the root-owned source selector.
- Add its TU to the actual selected `iokit/conf/files.arm64` DRE-only list.
- Invoke the **unrun**, strict `source_hooks.py:edit_selected_tree(root)` only on
  a fresh isolated selected XNU8796 tree. `source_hooks_program()` returns the
  same edit logic as standalone text for the root's late source edit recipe.
- It adds the real PE map observation, actual SLEH dispatch registry/callsite,
  DRE-only IOService friend declaration and two EOF bridges. Missing/changed
  exact anchors, duplicate hooks or absent source prerequisites refuse.
- Supply actual board/CPU/group/producer authority; absent providers refuse
  before controller/nub publication. Perform source/API compilation, concurrency
  and lifecycle checks tomorrow before any hardware attempt.

Primary source basis: selected XNU8796 `pe_fiq.c`, `sleh.c`,
`IOInterruptController.cpp`, `IOService.cpp` and `IOPlatformExpert.cpp`; original
boot-owned Holi FDT; pinned Lineage98a38783f42f5588bf00cd933e299aedc589bc10 Holi
interrupt graph; Linux v6.12 `include/linux/irqchip/arm-gic-v3.h` and
`drivers/irqchip/irq-gic-v3.c` for GIC register and NS priority presentation
semantics. No Linux global initialization sequence is reused here.
