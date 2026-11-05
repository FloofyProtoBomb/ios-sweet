# Native TLMM GPIO22 source — UNVALIDATED

This source was written under the overnight implementation-only instruction.
No compiler, test, lint, CI, staging recipe, emulator, ADB, fastboot or phone
operation was run for it. It does not establish working touch or IRQ delivery.

## Actual route and scope

Read-only inspection of the existing original Holi FDT confirms TLMM at
`0x00400000+0x00800000`, summary `<GIC_SPI 227 LEVEL_HIGH>` and the inherited
GICv3 parent at `0x0f200000`. The physical summary INTID is therefore **259**.
GPIO22's registers are at `0x00516000`; pin22 is not GIC INTID22. Its IOKit
nub has one explicitly mapped local source0/vector0 backed by this physical
pin and the actual leased parent, rather than equating the source with INTID.

The original wake parent is `/soc/wake-gic`, compatible with
`qcom,mpm-gic-holi` and `qcom,mpm`, not a generic PDC route. It has 96 wake
interrupts, vMPM at `0x045f01b8+0x1000`, IPC at `0x0f40000c+4`, and a separate
GIC INTID229 rising-edge notification. The pinned Holi pinctrl table maps
GPIO22 to MPM wake pin21. Neither wake pin21 nor the parent notification is
used as the awake GPIO22 GIC route. No MPM/PDC/wake register is mapped or written.

The controller is deliberately awake-only and manages only GPIO22. Because
the TLMM summary is shared by many pins, the real native broker must exclude
every foreign summary producer and hold that exclusion throughout this lease.
If a real global TLMM owner already handles other pins, this controller cannot
replace it or silently claim its summary. There is no scan-and-disable of
foreign GPIOs, global IRQ handler replacement, security change, PDC selection,
wake enable, parent EOI, GPIO21 reset or panel power operation.

## Concrete implementation

`tlmm.c` binds the original FDT resources and performs actual scoped register
operations under one native IOSimpleLock. Its initial read-only bind requires
an already masked, non-pending GPIO22 input owned by the actual broker. GPIO
mux, OE and EGPIO ownership must agree; an unknown inherited owner is not
masked to make the check pass. Only after the real parent GIC route has been
reserved, masked and joined can the controller program the pinned KPSS target,
falling-edge detection, polarity and RAW_STATUS_EN fields. It leaves the pin
masked and clears the setup edge before any client is armed.

Masking changes only the owned enable bit, preserving edge raw-status capture.
Acknowledgement reads status and writes bit0 clear, matching Holi's W0 ACK.
Arming masks/acks, checks the actual input, enables only when appropriate, then
rechecks both level and pending status. A low line queues the real report
worker; it does not create a fake interrupt or touch packet. The summary ISR
requires an enabled, actually pending pin, masks it and invokes the real child
handler, which performs its single ACK. No level-only fabricated IRQ is used.

`DRETouchTLMM.cpp` is an actual IOInterruptController with an exclusive child
slot, retained target, real callback dispatch and explicit parent joins. It
uses `dre_spi_reserve` for INTID259/level-high and the genuine native source
owner token. It registers its actual parent handler on the returned cached
IOKit nub, and uses real GIC mask/join/unmask and owner checks. The selected
SLEH path retains sole IAR/EOI handling. The controller and adapter share the
same `dre_tn_tlmm_pin`; they cannot race different GPIO register locks.

The namespace registration/removal uses the actual dictionary's atomic
register-if-absent/unregister-if-same hooks. Cache allocation calls the narrow
IOService friend bridge, which invokes the existing private non-resolving
allocator. Actual namespace owner and empty cache are then atomically bound.
No absent-name blocking lookup, guessed private arrays or private-to-public
macro is used. The adapter's capture rechecks the bounded real namespace and
cached controller/specifier before operations.

Normal unregister masks GPIO22, disables and joins the actual parent including
GIC ACTIVE retirement after SLEH EOI, and confirms no child callback remains.
Controller shutdown requires that the adapter has already unregistered its
child, then unregisters the real parent handler and restores the originally
masked pin configuration. It releases only its real parent/pin policy leases.
Any uncertain write, changed pin, ambiguous registration, failed join or lost
owner retains the relevant resources. It never blindly unregisters a possible
foreign handler or restores configuration over a changed owner.

Once published, the actual controller object remains boot-resident even after
orderly hardware retirement. General IOService client/cache unload has not
been proved, so late cached controller pointers cannot target freed memory.
An unpublished, fully retired failed acquisition may release its extra owner.
This object lifetime choice is not an assertion that hardware remains powered.

## Remaining real native policy and integration

The `dre_touch_tlmm_policy` has no default implementation. Its real reserve,
check, fast-check and release operations must own GPIO22, held TLMM access and
power, awake lifecycle, and exclusion of all foreign summary producers. The
same actual board ownership mutex used by `dre_spi` must be entered before
slow operations; lock order is board owner, controller lifecycle, pin spinlock.
Fast checks cannot sleep, acquire policy, recurse into the pin lock or touch
peripheral registers. A retained IOService supplies object lifetime only; a
framebuffer, GPIO level, DT property or success flag cannot grant these rights.

The separate GIC broker must qualify actual Group1NS authority, target CPU,
source ownership, trigger, affinity, global delivery and quiet-source retirement.
The parent controller, global ownership policy and its opt-in source hooks must
be present before this path can publish. No missing provider succeeds.
Panel/reset/rails, QUP clocks, GPI/SID0x16 DMA and actual Novatek firmware remain
additional touch dependencies; TLMM delivery does not establish any of them.

New translation units: `tlmm.c`, `DRETouchTLMM.cpp`. New headers: `tlmm.h`,
`xnu_tlmm.h`. Existing adapter and route capture depend on the separate `irq/`
headers, implementation, namespace/cache hooks and SLEH integration. Provider
ABI remains version3; `gpio_irq_nub` returns `dre_touch_tlmm_nub`'s real child.
`dre_touch_tlmm_pin_for_nub` supplies the actual shared engine after capture.
There is no extra HID device, user UAPI or change to the SweetHID batch hook.

Tomorrow still requires source/build validation, ownership/IRQ/error/teardown
fixtures, real native policy implementation and authorized hardware tests.
No current frozen image contains this source.

## Primary references

All Qualcomm references use kernel commit `98a38783f42f5588bf00cd933e299aedc589bc10`:

- [pinctrl-msm.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/pinctrl/qcom/pinctrl-msm.c): actual mask, raw-edge capture, W0 ACK, type/target, summary dispatch and wake-parent behavior.
- [pinctrl-holi.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/pinctrl/qcom/pinctrl-holi.c): exact GPIO register geometry/fields, EGPIO selection and GPIO22-to-MPM21 table.
- [holi-pinctrl.dtsi](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/holi-pinctrl.dtsi), [holi.dtsi](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/holi.dtsi): matching TLMM, GIC and MPM topology; actual captured hardware metadata was inspected separately without exporting its raw contents.
- [qcom-mpm.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/irqchip/qcom-mpm.c): separate MPM wake-domain programming, deliberately not performed here.
- Pinned XNU8796 `IOService`, `IOPlatformExpert` and `IOInterruptController`: actual namespace, source-cache, controller and service lifetime APIs; shared `irq/` source owns the narrow source-private hooks.
