# DE2118 native touch draft — UNVALIDATED

Written overnight, then validated locally after the user resumed work on
November5. **Native hardware and iOS input acceptance remain unvalidated.**
The explicit native-touch source selector exists; frozen installation images
are unchanged. This source does not mean the N200 accepts touch input in iOS.

## Local validation, November5

Run `python3 devices/dre/touch_native/test_native.py --cc cc --sanitize --out <directory>`
with a C11 compiler, Python and host OpenSSL libcrypto. Homebrew openssl@3 is
detected on macOS. It exercises the actual portable components through
controlled peripheral/IRQ/HID boundaries. It never accesses a phone or applies
a kernel recipe. Optional `--private-fdt <local-file>` reads an existing
capture; no FDT, firmware or identifier is copied or printed.

Linux GCC ASan/UBSan passes424 input/report checks,25672 GPI/SPI model checks,
3300 SMMU/TLMM/QUP checks and593 controller wire/firmware checks using actual
host SHA256. Optional original-FDT profile qualification passes29 checks. The
existing report and firmware-parser suites pass447 and640 checks, including
their freestanding ARM64 objects. These prove controlled source behavior,
not physical IRQ/DMA or iOS consumption.

Fourteen selected touch/common/SweetHID translation units compile as local
ARM64 Apple-target objects against actual selected XNU/generated headers,
using recovered CI flags and local Zig. This is not an Apple-toolchain build,
kernel link, boot or successful backboardd delivery. Native touch requires the
existing0071 edge patch and actual `fEdge`/`DRE_TOUCH_EDGE_PX` contract.

Validation fixed delivery timers setting a false pending-report mark, which
forced a stale SPI reread after held HID delivery at HIGH GPIO. The main
profile now requires a bounded full FDT and exact totalsize. Two native Device
mapping consumers explicitly include `vm/memory_types.h`.

The draft implements a concrete SPI/GPI transaction and Novatek input path.
Missing native providers return an error before hardware mutation. It has no
default DMA allocator, permissive SMMU switch, simulated touch packet,
readiness flag, GPIO reset fallback or successful placeholder callback.

## Implementation

* `profile.c` accepts the exact Holi SE0/GPI/Novatek/Tianma FDT shape: mode 0,
  CS0, 12 MHz maximum, physical 1080x2400 panel, GPIO21 reset/GPIO22 interrupt,
  AP GPII mask 0x1f, event factor 2, SID 0x16 and the original 1 MiB IOVA pool.
  The complete selected ABL panel token must identify `nt36672c_tm` once.
  Firmware bytes are not returned or written to disk.
* `gpi_spi.c` requests an exclusive native lease and actual wired Normal-NC
  allocations/IOVA mapping proofs, checks a qualified existing clock source,
  allocates event/TX/RX contexts through observed control acknowledgements,
  programs ring geometry and the paired SPI scratch fields, and starts both
  channels. It builds real mode 0, 8-bit full-duplex TREs using the existing
  `touch/spi_plan.c`. RX descriptors are published before TX GO. Both exact
  EOT pointers, lengths, codes and GENI idle/error status must agree before
  the caller receives bytes. Rings wrap; a transfer does not overwrite an
  outstanding request. Notifications never count as transfer completion.
* GPI completion polling is worker-only, bounded by the actual 19.2 MHz counter
  and an independent finite iteration count. The provider masks and joins
  native IRQ handlers while this poller owns the GPI registers. This uses
  actual GPI control/completion status, but does **not** prove GIC delivery of
  those IRQs. Normal teardown requires acknowledged STOP and DEALLOC commands
  before DMA buffers can be unmapped. A timeout, error, lost ownership or
  possibly effective failed write quarantines the object and retains its
  allocations, mapping, clocks and leases. There is no automatic recovery
  which might free memory still reachable by DMA.
* `nt36672c.c` computes SHA256 over the supplied immutable firmware, checks the
  pinned Tianma DT blob, and uses the existing bounded partition parser. It
  performs real page selection, command-bit masking and command/dummy/payload
  framing; splits transfers at 128-byte page boundaries; matches the controller
  trim; writes firmware SRAM; programs ILM/DLM destination/last-index lengths
  and golden CRCs; enables bootloader/firmware CRC; sets BOOT_READY; waits for
  reset transitions; compares actual hardware CRC results and firmware version.
  Every operation must complete through the SPI transport. There is no
  unknown-trim/default-memory-map or firmware-error success fallback.
* `input.c` leaves hard IRQ work at masking/coalesced scheduling. Its bounded
  serialized worker reads actual controller reports and uses the existing
  checksum/contact-ID/coordinate decoder. It retains an authentic frame while
  the real HID service is unavailable. Corrupt reports and watchdog markers
  never become an invented empty frame. A confirmed controller fault or owner
  stop can cancel only the contacts previously submitted, with an explicit
  cancellation reason. Teardown joins IRQs and workers before cancellation or
  object destruction. There is no automatic firmware-reset loop.
* `xnu_hid_frame.inc` is opt-in source for the existing open `SweetHIDTouch`
  class. It creates one real IOHID digitizer collection per complete controller
  frame, including releases, edge origin, pressure and explicit cancellation.
  Child allocation/append failures preserve the preceding state. The existing
  physical-panel to presented 1125x2436 offset is applied once. Submission
  requires a retained active service and existing clients; its return value
  means `dispatchEvent` was called, not that backboardd acknowledged the event.
* `DRENativeTouch.cpp` supplies the actual XNU side of these interfaces. It
  obtains the boot-held original FDT through `dre_native_board_snapshot`,
  privately copies and binds the exact hardware nodes, and retains the existing
  `SweetHIDTouch` IOService. It allocates/prepares real IOBuffer descriptors,
  checks their actual physical segments and EL1 read/write PAR attributes,
  requires native alias/SID mapping proofs, and retains uncertain mappings.
  A private IOWorkLoop/IOEventSource/timer executes the portable worker. Real
  `registerInterrupt`/`disableInterrupt`/`unregisterInterrupt` calls bind and
  join a qualified GPIO22 interrupt nub. Local TLMM mask/ack/rearm preserves
  edge raw-status detection and rechecks after both local and parent enable.
  The platform's atomic resource, IRQ-route and SMMU policies remain required
  callbacks in `xnu_provider.h`; their implementation is not invented here.
* `smmu.c` supplies the touch-specific SID0x16 domain component. It binds the
  actual shared apps QSMMU-v500 FDT node, TCU/anoc_1 TBU, original IOVA pool
  and SID-specific ACTLR policy. It builds owned 4 KiB LPAE tables, publishes
  only the exact SID0x16 SMR/S2CR and reserved context bank, creates five
  bounded RO/RW/NX mappings, checks every PTE and guard hole, invalidates the
  actual owned ASID and waits for context TLBSYNC completion. Mapping changes
  require actual producer/IRQ/TBU retirement. Failure retains pages and leases.
  Empty-domain teardown restores only that bank and its previously unused
  SMR/S2CR through a faulting route. There are no global reset/CR0/ACR/TLBI,
  peer-stream, UFS, secure-context, TBU debug or ECATS writes.
* `qup.c` independently binds the original FDT's Holi GCC and wrapper, exact
  SPI clock phandles, and existing pin/IRQ profile. Its read-only observer
  captures actual AP votes, branch states, static/DFS RCG configuration,
  selected Fabia PLL, GENI source/divider/protocol, wrapper version, pin muxes,
  and all five AP GPII pairs' control/completion/error status. The decoder
  derives the existing SE core and serial rates from those readbacks. It
  refuses unknown/test parents, unsettled sources, inactive branches, invalid
  M/N/D, protocol/FIFO mismatch and an out-of-range rate. A held native XO and
  real shared parent/clock/rail policy are mandatory additional evidence.
  No PLL, source, divider, pin configuration or shared vote is programmed.
* The adapter uses a real kernel mutex and bounded generation ledger to reserve
  SPI0, GPIO0..3/21/22, GENI IRQ359 and the broker-selected GPII pair/event/IRQ.
  Its snapshot must independently show that exact pair completely unallocated,
  SPI idle and no old completion/control/error status before the adapter's
  first peripheral write. It never substitutes a different unleased pair.
  Actual hardware allocation still uses GPI control commands and observed ACKs
  in `gpi_spi.c`; a local ledger entry alone is not a hardware allocation or
  proof against an unregistered firmware/native owner. Full native broker
  exclusion remains mandatory. STOP-state contexts cannot become a new free
  pair just because a local owner disappeared.
* `xnu_irq_route.cpp` captures the exact real IOKit nub, registered interrupt
  controller, controller symbol, vector specifier and resolved
  `IOService::_interruptSources` mapping supplied by native IRQ ownership.
  It retains those objects, bounds/copies the specifier, verifies the actual
  cached binding, and refuses a missing/unregistered controller, changed
  mapping, shared-vector conversion or an ambiguous registration result.
  Task-context register/enable/disable/unregister calls use the captured real
  controller. Hard IRQ compares the actual callback nub/source without an
  allocating property lookup and never acknowledges an unexpected source.
  A failed or ambiguous registration keeps all leases/references resident;
  it does not unregister a possibly foreign handler to recover automatically.
* `tlmm.c` implements actual bounded GPIO22 target/type setup, mask, W0 status
  acknowledgement, arm/recheck, real pending-edge detection and originally
  masked configuration restoration. `DRETouchTLMM.cpp` supplies the actual
  IOKit controller/private nub and binds it to the native leased GIC259 route.
  Controller and adapter share this one pin engine and native IOSimpleLock.
  This is awake-only summary delivery. MPM wake, PDC, foreign GPIOs, GPIO21
  reset, panel rails and GIC EOI are untouched. See [TLMM_IRQ.md](TLMM_IRQ.md).

The XNU adapter is also **UNVALIDATED**. None of these native allocation,
translation, IRQ registration or IOHID calls has been compiled or executed.

The SMMU component uses apps QSMMU-v500 at `0x0c600000`, TCU `0x0c702000`
and anoc_1 TBU `0x0c705000`. It never uses the GPU-private `0x05940000`
profile or that GPU's implementation register settings. Hardware ID1 derives
the context-bank span/stride; the 1 MiB DT reservation is not assumed to be
the actual hardware layout. The matching SID ACTLR is `1`, not the GPU's
`0x70000000`. The inherited apps node has `qcom,skip-init`, which is a reason
to require the real native global owner, not evidence initialization succeeded.

Before publication, the component checks actual observed ID0/ID1/ID2,
CR0, ACR and TCU version against the native reservation, unmatched-fault policy,
an unused disabled nonsecure bank, no pending fault/TLB activity, no matching
peer SID route or shared bank, and no observed enabled peer ASID collision.
Those readbacks supplement the real native CB/SMR/ASID/security/clock/IRQ/TBU
ownership ledger. They cannot establish an unused nonsecure reservation or
complete vendor initialization from a register snapshot. An unknown global
policy, cache-lock state, producer retirement or no-ASID-retention workaround
refuses before programming this domain. No implementation of that native
global-owner policy has been supplied or claimed successful here.

## Existing-clock evidence and reservation lifetime

The QUP observer reads GCC's AP vote at `0x0147900c` and requires held wrap0
master-AHB/slave-AHB/S0 bits6/7/10, with actual branch status at
`0x0141f004`, `0x0141f008` and `0x0141f144`. These are observations of the
existing tree, not newly acquired rail or bandwidth votes. The serial RCG is
at `0x0141f148`, with a 5-bit HID and 16-bit M/N divider. DFS mode uses the
actual current level and that level's hardware CFG/M/N, rather than assuming
the compiled static table is active. GENI `CLK_SEL` is that performance-level
index, not GCC's parent mux encoding.

Recognized existing parents are qualified XO, GPLL0 main/even and GPLL6 even.
Fabia rates use the actual L and 16-bit FRAC values; even output accepts only
the pinned `/2` post-divider encoding. No nominal 600/768MHz constant stands
in for PLL readback. The wrapper version determines the pinned SPI driver's
oversampling factor. The portable transport retains its stricter conservative
12MHz bound, and the native policy must keep that exact existing source and
rate held through all GPI commands and transfers. Unsupported or power-gated
inherited state refuses; no source is enabled or synthesized to pass a check.

`TouchClockReason`, `TouchQUPReadback` and numeric rate IORegistry properties
retain fixed hardware evidence even on refusal. A failed observation can
contain an incomplete snapshot; it is not proof that zero-valued fields were
read. The reason records access/stability/decoding refusal. These properties
contain no private FDT, firmware or phone identifier and no claim of readiness.

Normal release requires actual transfer retirement, acknowledged STOP/DEALLOC,
native GPI/GENI mask-and-join, SMMU retirement, GPIO IRQ join/unregistration,
and a fresh idle/error-free owned snapshot while power/access remain held.
Only after the real native broker releases its lease does the adapter remove
the local generation token, using that preceding retirement evidence without
any new hardware access. A failed broker release preserves the token and
hardware-lifetime owner. Unexpected local ledger loss preserves the diagnostic
owner instead of freeing it or accepting a different generation. Neither a
stale STOP state nor a read-only snapshot establishes foreign-owner exclusion.

## Native interrupt route boundary

The selected platform expert explicitly excludes `/arm-io/gic` when publishing
IOKit nubs. The selected DRE `sleh_irq` path handles the timer and SGIs itself,
then dispatches other physical INTIDs through the installed per-CPU interrupt
handler, if one exists. The default selected Sweet platform/CPU sources do not
provide an IOKit GIC/TLMM controller or install native GPIO22 summary delivery.
The new opt-in `DRETouchTLMM.cpp` and separately owned `irq/` source now
implement that actual route, subject to real native broker leases and source
integration. They have not been compiled, linked, staged or executed. No nub
is manufactured from a DT number or assumed to exist because platform startup
worked. The native `gpio_irq_nub` operation must refuse until those actual
controller owners and CPU/cascade paths exist and are exclusively leased.

In pinned XNU8796, generic `IOInterruptController::disableInterrupt` sets a
software-disabled state and joins active callbacks in task context; it does
not necessarily mask the hardware source. The adapter's actual TLMM22 mask
must happen first. Its controller unregister follows real handler join and
performs the owned controller's teardown. A captured controller/specifier is
additional object/mapping evidence, not proof of interrupt delivery, PDC/GIC
routing, vector exclusivity, held power or correct controller-specific bounds.
Those policies remain native-owner requirements. No GIC EOI, PDC selection,
global IRQ-controller replacement or synthetic interrupt is added here.

The native publisher must resolve the actual IOService source cache after
registering its real controller. Pinned `lookUpInterruptController` waits
indefinitely for a missing name, so this capture never uses it as an existence
probe and never starts IOService resolution for a missing cache. It inspects
the already resolved real cache and invokes that retained controller directly.
An unregistered/unresolved route refuses immediately instead of waiting for a
provider to appear. Native ownership must keep the actual platform namespace
registration and cache immutable through teardown.

Source capture happens before the adapter's first GPIO/GPI/SMMU write. A
capture-only failure releases its retained objects and native ownership through
the no-mutation unwind; it does not mask an unqualified parent or reset a
controller merely because metadata was missing. Once an actual write may have
occurred, that fact is recorded before the write and teardown requires the
real mask/join and retirement path. Changed/ambiguous registered mappings keep
the diagnostic owner, real resources and every retained reference resident.

## Exact native providers still required

1. **Atomic ownership and power:** implement `dre_tn_ops.reserve/check_lease/`
   `get_clock/release` against the shared native orchestrator. Reserve one
   genuinely unused GPII pair/event ring from 0..4, SE0 and all pins/IRQs, keep
   the actual QUP wrapper/master/slave/SE clocks and panel/reset dependencies
   alive, and exclude the legacy Sweet touch driver and every other QUP owner.
   The concrete decoder now derives the existing clock rate from actual
   readbacks. The real native `clock_reference` and `check_observed_clock`
   policy must qualify/hold the XO, selected parent, source, aggregate voltage
   and bandwidth requirements. DT, CNTFRQ, an active branch or a derived rate
   alone cannot supply that policy. The locked generation registry supplements
   that real cross-driver/firmware broker, rather than replacing it. No native
   source/rail/panel owner is installed by this source. It never writes a
   guessed PLL, shared regulator floor or pin configuration.
2. **Touch DMA:** finish actual shared-resource/global policy and physical
   alias ownership. The provider's allocation
   ledger must exclude physical aliasing between all five blocks. GPU SID 0,
   a CPU pointer, an idle channel or a static preflight arena is insufficient.
   Prior physical diagnostics reported the touch stream as unmatched/faulting;
   no later native mapping success is claimed here.
   The adapter now supplies real IOBuffer allocation/prepare/complete and
   explicit CPU barriers, descriptor/PAR inspection and failure retention.
   The concrete SMMU component now supplies SID0x16 table construction,
   mapping, walk, invalidation/sync, unmapping and restoration. The remaining
   policy is an actual unused nonsecure CB/SMR/ASID/fault-IRQ reservation and
   initialized shared QSMMU-v500 TCU/anoc_1 TBU owner, with all SID0x16
   producers quiescent when required. Each of the five logical
   1024/4096-byte blocks gets a separate wired 16 KiB descriptor; the mapping
   proof covers all four 4 KiB physical pages. TX/RX TREs and TX data request
   device RO, event/RX destinations RW, and all device mappings must be NX.
   SID0x16 can serve other GPII clients: reserving one GPII does not authorize
   replacing their SMMU domain or claiming their IOVA mappings.
   A separate wired 16 KiB IOBuffer holds the tables; its first three verified
   4 KiB pages contain L1/L2/L3 and none of its pages can be a data mapping.
   A software walk plus actual control-register readback and completed TLBSYNC
   does not prove physical DMA or a QSMMUv500 ECATS translation worked.
3. **Native interrupts/work loop:** bind GPI/GENI mask-and-join and GPIO22
   falling-edge one-shot behavior, native 32-bit atomics, source-specific TLMM
   acknowledge/GIC EOI, coalesced sleepable scheduling, clear/arm/recheck, and
   external teardown which joins the IRQ before cancelling/joining workers.
   GPII 0..4 IRQs are GIC INTID 367..371 and SE0 is 359 in this exact profile.
   GPIO22's native IRQ routing is a separate TLMM provider, not INTID 22.
   The adapter now supplies real work-loop scheduling, atomic coalescing,
   IOKit interrupt registration/join, and GPIO22-local mask/ack/rearm. Its
   `gpio_irq_nub` policy still has to prove the actual exclusive native route
   and coordinate local TLMM ownership; a second interrupt controller must not
   race the adapter's pin lock or modify pin22 config/status behind it. An
   unresolved shared summary/PDC route is not converted into a guessed GIC nub.
   The concrete route capture additionally retains and verifies actual IOKit
   controller/cache objects. The provider must keep that controller, its nub
   and immutable source metadata resident, qualify its real vector bounds,
   and own the CPU-handler/cascade route for the whole lease. The concrete
   TLMM/GIC source drafts provide that implementation surface; the default
   kernel still supplies no publisher or real native broker. Native global
   ownership must exclude every foreign TLMM summary producer before this
   one-pin controller can lease INTID259. It never disables another pin or
   substitutes an unrelated PDC wake route to make ownership appear valid.
4. **Panel/controller lifecycle:** qualify and hold incell-panel power/reset
   prerequisites before any Novatek software reset or SRAM download. The
   draft does not pulse resetGPIO21 or run display-off/engineering-reset
   commands. Current panel mode, shared ownership and firmware loss during
   suspend still need a real native lifecycle policy.
5. **Private firmware qualification:** accept only the selected DT Tianma blob
   (139264 bytes, SHA256 `e44e4ea6d7d1dd23b10ee26a31b50a39cf111f618a75ea5b2335f6e2ca489ada`).
   The separately captured ODM file is 139472 bytes and is a different input.
   It is deliberately not accepted by this path. The conservative SRAM
   ceiling excludes the 0x3f000 control bank and must be checked against the
   actual private partition layout tomorrow. No claim is made that the full
   hardware-CRC sequence or 65-byte report size has run on this transport yet.
6. **IOHID lifecycle:** retain the actual service, serialize stop and all touch
   producers, and supply the real `mach_absolute_time` clock. The existing
   void `sweet_hid_touch` ABI cannot report a missing service or partial
   collection allocation. The opt-in batch fragment supplies an explicit
   failure path; successful consumption by iOS remains unproven.

## Opt-in source recipe, not applied or built

Do these changes only after tomorrow's validation is authorized. They are not
part of any automatic patch or current kernel image:

1. Stage this directory's C/header files plus existing `touch/firmware`,
   `touch/report`, and `touch/spi_plan` into the SWEET native source tree. Keep
   directories under `iokit/Kernel/arm/dre/{touch_native,touch,libfdt}` and
   preserve relative includes. Link `profile.c`, `gpi_spi.c`, `nt36672c.c`,
   `input.c`, `smmu.c`, `qup.c`, `tlmm.c`, `xnu_irq_route.cpp`, `DRETouchTLMM.cpp`,
   `DRENativeTouch.cpp` and the three existing portable C helpers
   only in an explicitly enabled DE2118 build. Link the existing shared
   `dre_dma_ram_allowed` and libfdt implementation once, plus the separately
   owned `native_board.c`/header beside these directories. No duplicate native
   storage or libfdt implementation should be linked from the touch directory.
   TLMM additionally requires the separately owned `irq/` source and its real
   opt-in SLEH, IOPlatformExpert namespace, IOService source-cache friend hooks.
   This is a hard source/link dependency, not a default success fallback. Keep
   `touch_native/../irq` relative includes intact.
2. In `SweetHID.cpp` after existing patches 0058, 0061 and 0071, add a global
   `struct dre_tn_hid_frame;` declaration before `class SweetHIDTouch` and add
   this public method declaration inside that class:

   ```cpp
   bool dreNativeFrame(const struct dre_tn_hid_frame *);
   ```

   Place `#include "../dre/touch_native/xnu_hid_frame.inc"` after the existing touch
   method implementations. The fragment uses their real fields and types.
   It does not introduce a proprietary Apple HID ABI.
3. Provide the actual resident version-3 `dre_touch_xnu_provider` and call the kernel-only
   `dre_touch_xnu_publish(provider, actual_sweet_hid_service, &owner)` after the
   real HID service has registered and scheduling is available. The adapter
   retains that service, binds the existing batch fragment, and obtains the
   authoritative FDT from the checked shim snapshot helper. Optional
   `provider.fdt` is only an exact expected copy; a caller-provided tree cannot
   replace the boot-held hardware authority. The old Sweet touch hardware
   driver must be excluded by the real platform acquisition policy.
   `reserve_domain`, `check_domain` and `release_domain` are real native
   resource/global-owner operations. Version 2 removed the former generic
   map/walk/unmap callback gap; the concrete component now performs those
   operations. The global-owner evidence/epoch must originate in actual held
   native initialization and reservation, never a DT flag or claimed ready bit.
   Version 3 removes the former opaque `qualified_clock` Hertz callback: the
   adapter decodes its own GCC/GENI/Fabia readbacks and passes the exact result
   to `check_observed_clock`, with XO supplied by a real `clock_reference`
   ownership operation. Missing shared clock/rail/panel policy still refuses
   before the adapter's first pin/GPI/SMMU write. There is no successful
   placeholder for those remaining native dependencies.
4. The adapter fills actual XNU operations, opens SPI, obtains the private
   Tianma firmware bytes from its FDT copy, computes SHA256 with the already
   linked kernel SHA256 implementation, performs the controller handshake,
   then starts input. It never publishes another digitizer/HID protocol.
   Input starts only after the observed boot/CRC/version sequence succeeds.
   The `UNVALIDATED` and `TouchPhase` service properties describe source/phase
   only; they are not provider readiness or successful iOS-consumption flags.
5. Preserve a returned `owner` even when publish returns an error. A partial
   lease creates a separate hardware-lifetime retain; error return is not
   permission to destroy it. From external task context outside the touch work
   loop, call `dre_touch_xnu_shutdown(&owner)` for explicit retirement. It
   quiesces/cancels input, closes the transport, retires actual SMMU mappings,
   completes/releases IOBuffer descriptors, joins/unregisters the IRQ and drops
   only this owner's policy refs. A successful call consumes the handle.
   Failure leaves the owner/uncertain resources retained. Ordinary IOService
   stop does not invent recovery or free a quarantined DMA owner. Controller
   recovery still requires a separately qualified sequence and a fresh
   controller/input generation after a real cancellation.

## Primary protocol/source facts

All vendor references are pinned to LineageOS's OnePlus SM4350 kernel
`98a38783f42f5588bf00cd933e299aedc589bc10`:

* [GPI driver](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/dma/qcom/gpi.c):
  dynamic `gpi_of_dma_xlate` allocation, channel direction, allocation/control
  acknowledgement, ring alignment/geometry, event pointer/recycling and
  transfer-completion layouts. DT DMA argument 1 is SEID 0, not GPII 0.
* [GPI registers](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/dma/qcom/msm_gpi_mmio.h):
  EE-adjusted context/doorbell/command/interrupt/scratch fields.
* [GENI SPI](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/spi/spi-msm-geni.c):
  CONFIG0/GO/DMA TRE fields, paired TX/RX completion and source/divider handling.
* [Holi QUP topology](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/holi-qupv3.dtsi):
  GPI event factor 2, interrupts 335..344, pool, SID 0x16 and SE0 resources.
* [Novatek controller](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/input/touchscreen/oplus_touchscreen_v2/Novatek/NT36672C_noflash/nvt_drivers_nt36672c_noflash.c):
  masked trim, NT36675 map, software reset, SRAM/CRC/boot sequence and reports.
* [Novatek SPI framing](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/input/touchscreen/oplus_touchscreen_v2/util_interface/touch_interfaces.c):
  writebit7, readbit7 clearing, one dummy byte and payload offset2.
* Matching `pinctrl-holi.c` and `pinctrl-msm.c`: GPIO22 config/IO/IRQ registers,
  target/detection fields, preserve-RAW edge masking and write-zero status ack.
* Matching `drivers/clk/qcom/gcc-holi.c`, `clk-rcg2.c`, `clk-alpha-pll.c`,
  `clk-branch.c` and `include/dt-bindings/clock/qcom,gcc-holi.h`: exact wrap0S0
  RCG/MND/HID/DFS, AP vote/branch registers, parent maps, Fabia fractional and
  post-divider decoding and clock IDs117/143/144. The actual vendor GENI helper
  is `drivers/platform/msm/msm-geni-se.c`; the SPI driver derives source-index
  and oversampling semantics from that implementation and wrapper hardware.
* Existing pinned XNU `IOBufferMemoryDescriptor`, `IOWorkLoop`, `IOEventSource`,
  `IOTimerEventSource`, `IOService` and `IOLocks` source/header APIs, plus the
  repository's existing SweetHID patches: native allocation, scheduling,
  interrupt join and actual HID service interfaces. No Apple private input
  method or substitute Metal/GPU mapping API is introduced by this adapter.
* Pinned XNU8796 [IOService header](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/iokit/IOKit/IOService.h)
  and [IOInterruptController implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.141.3/iokit/Kernel/IOInterruptController.cpp):
  registered platform-controller lookup, real source-cache/vector lifetime,
  soft-disable/join semantics and actual unregister. The selected repository
  `SweetPlatformExpert.cpp` and DRE `sleh_irq` changes establish the present
  missing-controller boundary; an original-FDT interrupt property is not a
  replacement controller implementation.
* Matching `drivers/iommu/arm-smmu.c`, `arm-smmu.h`, `arm-smmu-qcom.c` and
  `io-pgtable-arm.c`, plus `msm-arm-smmu-holi.dtsi`: QSMMU-v500 resource and
  ACTLR selection, architecture geometry/stream/context/permission fields,
  table construction, context-ASID invalidation and completed TLB sync.
  QSMMUv500 hard-translation debug uses a separate TBU halt/ECATS flow in that
  vendor driver; this component deliberately does not perform those operations
  or present a software walk as their hardware result.

FDT profile constants agree with the existing private live metadata and
tracked preflight implementation; this is source evidence, not a successful
native-driver run. Raw FDT, firmware and per-phone identifiers remain private.

Tomorrow's work starts with source/build validation, then realistic descriptor,
failure/IRQ/teardown fixtures and the native provider implementations. A phone
test comes only after those dependencies are real and separately authorized.
