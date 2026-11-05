# Native GPU SPI connection — UNVALIDATED

`gic_route.cpp/.h` composes the actual `native_owner` broker's IRQ operations
with the real `dre_spi` controller. Source only; no tests, compilation, recipes,
CI, emulator or phone operations were run. No Metal/Vulkan capability changes.

The caller supplies the genuine existing controller and **actual** external
controller lifetime/source-owner policy. Every non-IRQ broker callback is
required and forwarded with its original policy cookie. The wrapper does not
supply GPU rails/reset/MMIO/RPM/SCM policy or a controller/security broker. Its
owner callbacks preserve the original common board mutex; the controller and
its policy must use that same actual lock, including provider-v2 binding.

Creation reads the existing boot-held original FDT and checks Holi, A619 chip,
GPU IRQ tuple `<0,177,4>`, unique GIC phandle and GICD/GICR resource identity.
The acquire wrapper forwards the actual GPU acquisition, preserving its real
lease/reset epoch even on error. It then resolves the actual acquired GPU
source-owner record. No token is derived from a counter, address or lease value.
GPU resource descriptions must match the six original native-owner apertures
and exact SPI177/INTID209 level-high GIC route before reservation.

Reservation calls actual `dre_spi_reserve`, obtains its actual retained nub,
controller, source0 and genuine route-generation token, then requires an
unregistered exclusive handler. `check_irq_route` compares the complete route,
source-owner/lease generation and real cached/registered target via `dre_spi`.
Fast callbacks qualify real GPU/reset/source/controller ownership and the real
SPI token before masking only that owned vector. They perform no sleeping
lock, namespace lookup, GPU/rail MMIO or IRQ ACK/EOI. SLEH retains sole EOI.

The bridge's CAS reader gate joins its own fast users before freeing a route;
actual `dre_spi_route_mask_join/release` additionally joins hardware ACTIVE and
the real SLEH readers. Foreign/stale route arguments never authorize a mask or
release. Failed callbacks retain real metadata and resources for recovery.

An important partial-acquisition case is handled explicitly: `dre_spi_reserve`
can retain a genuine route before installation, whose real generation token is
still zero. The bridge preserves that actual pointer and returns only truthful
available metadata. Its composed outer `release` ALWAYS retires such retained
SPI ownership first, even when `native_owner.route_lease==0` would otherwise
skip `release_irq_route`. A failed retirement prevents releasing the GPU lease.
After an escaped broker callback cookie, bridge and controller reference stay
boot-held; no general callback-join/unload proof is invented.

Future integration order:

1. Bind the actual provider-v2 owner mutex to the external policy and actual
   SPI controller. Supply all real non-IRQ `dre_gpu_native_broker` operations.
2. Supply genuine controller hold/check/drop and acquired-source lookup/check
   callbacks in `dre_gpu_gic_policy`; create this bridge with owner mutex held.
3. Obtain its full typed broker with `dre_gpu_gic_route_broker`; pass that to
   actual `dre_gpu_native_owner_create`. Keep escaped bridge memory resident.
4. Native owner must mask/join the controller under the board mutex before
   leaving it to join deferred work; then unregister/release after actual GPU
   retirement. No worker join is hidden in this bridge's primary IRQ join.
5. Select `gpu/platform/gic_route.cpp` alongside actual `irq/DRENativeGIC.cpp`,
   shared IRQ source hooks and native-board/libfdt source. Apply the existing
   platform libfdt/string-header rewrites in root's opt-in source recipe.

Missing actual external controller/source/Group1NS/producer/CPU-route policy
still refuses. The adapter removes hand-written IRQ glue; it does not turn
those unresolved hardware ownership requirements into ready flags.
