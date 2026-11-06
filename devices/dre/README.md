# OnePlus Nord N200 DE2118

**Measured status, 2026-11-05.** The target is iPhone X (`iPhone10,3`/D22AP)
iOS 16.7.16, build 20H392. The phone still runs its restored LineageOS image.
The port's full iOS image is prepared locally; it has not been installed on
the N200. The original Redmi instructions and results do not establish N200
hardware support.

| Area | Physical DE2118 evidence | Local validation |
| --- | --- | --- |
| CPU and RAM | Eight CPUs, diagnostic PID 1 and a 2,047 MiB usable window passed | Eight-core XNU and real HFS/iOS userspace boot in QEMU; all 4 GiB are not exposed |
| Storage | Early shim reads the real GPT; latest native adapter test timed out before disk registration | New Normal-NC/power/timeout candidate, guarded engine, BSD interface, real HFS writes/flushes and independent file readback pass; physical correction remains untested |
| iOS filesystem | No iOS userdata install performed | Exact 54,431,395,840-byte journaled HFSX image, 41,844,289,536 bytes free, complete sparse transport and clean filesystem audit |
| IOSurface/display | Diagnostic console works | Actual backboardd creates surfaces and produces CPU-rendered Apple-logo frames; retained-reference/category fixes pass full-kernel checks |
| Setup/activation | Not demonstrated | SpringBoard starts; latest five-minute run ends at the Apple logo, with LaunchServices database initialization still waiting |
| GPU/Metal | GPU observed powered off | Guarded A619 primitives and a real compiled Metal acceptance workload exist; native execution, graphics synchronization/presentation and a compatible Metal runtime/compiler remain unfinished |
| Touch/USB | Hardware and firmware/layout probes only | No working native touch input, USB gadget or iTunes restore compatibility |
| Wi-Fi/Bluetooth | No native radio tests | [Pinned resource/dependency inventory](radio/README.md); drivers are absent |

The combined kernel [CI 41976101399](https://github.com/FloofyProtoBomb/ios-sweet/actions/runs/41976101399)
passes compilation, linking, all 67 patches and Metal test-program compilation.
Its kernel SHA-256 is `2d388d12a28ed31dcffb316099604950c747127c5f6edd182985be72a8223f79`.
These host and simulator results do not establish native UFS, Setup or Metal
operation on the phone. The actual Metal acceptance workload has not passed.

`--native-ufs` retains the diagnostic RAM root and read-only userdata checks.
The separate `--install-root` profile omits that RAM disk and enables the
userdata-only write capability for the real HFS root; see
[installation prerequisites and evidence gates](INSTALL_READINESS.md).
Every phone partition write still needs its specific approval. Existing
tested images and original backups remain unchanged.

The next diagnostic also selects `--ram-mib 2048`: a 2 GiB upper-memory
window validated against the live RAM and reservation map. QEMU boots with
all eight cores and reports 2,047 MiB after the 1 MiB panic-log reservation.
The corrected handset test reported eight active CPUs, 2,047 MiB and a
running PID 1. This validates the reported window, not every RAM cell. Native
UFS initialization reaches its probe, then reports `IO:COMMAND_TIMEOUT`.
BSD disk devices are not registered, so the first open returns ENOENT. This does not expose all
4 GiB of installed RAM. The default remains 512 MiB.

The first combined handset test reached a kernel panic in temporary FDT
mapping cleanup, before PID 1. LineageOS was restored and hash-verified.
The driver now uses XNU's removable mapping API for that temporary copy.
`qemu_ui.py --native-map-test` exercises this positive ownership path across
a 16 KiB page boundary, then returns before Qualcomm MMIO. Both this test
and the empty-ownership refusal test must pass before another handset test.
The corrected kernel passed both QEMU cases and then booted the handset
without the mapping panic. Its early shim read the real GPT successfully;
the native BSD check reported failure. These are separate storage paths.
The subsequent image `1104ecab` confirmed the native failure is a command
timeout during driver initialization, before device registration. It retained
eight CPUs, 2,047 MiB, PID 1 and the early GPT pass. Its command number and
opcode are not yet known. LineageOS was restored and its boot/logo hashes
verified afterward. Successful simulated BSD tests do not establish physical
native UFS operation.

The next diagnostic exposes read-only `kern.dre_ufs_state` as
`NATIVE_DRIVER=phase:reason` and caches the first failing PID 1 stage and
syscall errno. Errno zero means a returned value or GPT check failed.

| Stage | Operation |
| --- | --- |
| 001 | Open the raw userdata device read-only |
| 002 | Query its logical block size |
| 003 | Query its block count |
| 004 | Verify the device reports read-only |
| 005 | Seek to byte 512 of userdata |
| 006 | Read 512 bytes of userdata |
| 007 | Verify a write-only open rejects with EROFS; no write is issued |
| 008 | Open the whole-LUN raw device read-only |
| 009 | Query the whole-LUN block count |
| 010 | Seek to the GPT header at byte 4096 |
| 011 | Read the GPT header |
| 012 | Validate GPT geometry and header CRC |
| 013 | Open the block userdata device read-only |
| 014 | Seek to byte 512 on the block device |
| 015 | Read 512 bytes through the buffer cache; match the raw read's first 16 bytes |

`qemu_ui.py --native-bsd-test` adds a third, explicitly simulated backend.
It requires both QEMU test arguments, the additional BSD-test argument and
valid synthetic FDT ownership. It exercises the real devfs registration,
open, ioctl, seek, physio/strategy read and rejected write-open paths with an
in-memory disk, without accessing Qualcomm MMIO. Its driver status begins
`VIRTUAL_TEST`; success does not establish physical UFS operation. Handset
payloads must omit every QEMU-only argument, including file-provider and
simulator timing options. The empty-ownership,
mapping-lifetime and simulated BSD cases must pass before another handset
test.

The validated diagnostic boots XNU 8796 with all eight CPUs and a small Mach-O as PID 1
on a mockfs RAM disk. The eventual userland target is iPhone X
(`iPhone10,3`, `D22AP`) iOS 16.7.16, build 20H392. The diagnostic image
contains the kernel and the project's small init program. SpringBoard,
Setup.app and the iOS system volume are separate.

## Results

| Path | Result |
| --- | --- |
| DE2118 board | Read the live FDT, boot headers and panel ID on project 133180, running LineageOS 22.2, slot B. Its product configuration reports DE2117 in Android. |
| Phone-specific boot image | Android v3, 4096-byte pages, Android 15.0.0 / July 2025 header, as on the installed boot image. The active vendor_boot and dtbo supply the bootloader device tree. The shim embeds its XNU payload inside the kernel Image to avoid vendor/generic ramdisk concatenation. |
| CPU, memory and display | In ARM64 emulation, the exact phone image accepted the handset's FDT, passed the verified boot arguments to XNU and executed XNU's first 64 × 32 framebuffer marker. A separate probe rendered its complete result text at two load addresses and refused five incompatible or malformed device trees. |
| Full-system virtual boot | On QEMU virt, the same DRE kernel and RAM-disk init mounted md0, started PID 1 and printed a heartbeat. The virtual test substitutes virt's interrupt controller and architected timer in the ADT. |
| Physical XNU boot | The DE2118 photos show XNU booting via dre-shim, VM bootstrap, CPU 0 registration and SGI1 verification, HFS registration, Skywalk self-tests and BSD network-interface threads being scheduled. In the subsequent combined handset test the user confirmed the repeating DE2118 TEST card, which is emitted by diagnostic PID 1 after md0 mounts. |
| Identifiers | The shim detects the real serial from ABL's androidboot.serialno, with a root FDT serial-number fallback. It fills the copied XNU ADT and derives the same per-phone UUIDv5 at startup. The native generic handset image reports SERIAL=ABL_DETECTED and identity checks PASS. Exact UUID values were independently checked in emulation and QEMU. An optional checksum-validated IMEI can be staged as OSData at IODeviceTree:/oneplus-hardware/imei. Automatic IMEI discovery and a native CoreTelephony/Settings lookup require modem integration. Serial display in Settings is untested. |
| Adreno | The N200 reports Adreno619v1. On the handset a KGSL userspace test submitted an A6xx CP_MEM_WRITE packet and read its result back after the GPU fence. The XNU ADT supplies the GPU register ranges and interrupt. XNU GPU power, firmware, SMMU, submission, buffer sharing and iOS graphics user-client support remain unimplemented. |
| Input and display | The phone reported a Tianma NT36672C panel and Novatek no-flash touch on SPI. The console writes to continuous-splash memory. The native test found the expected touch clock votes absent. The portable [report decoder](touch/README.md) passes 447 synthetic checks but is not linked into XNU. SPI, firmware loading, touch events, display power and accelerated composition remain outstanding. |
| Storage and radios | The early shim has read the real GPT. The native read-only BSD UFS adapter is implemented but has not passed the handset check. Native disk access and a root volume are required before it can run a full iOS filesystem. Cellular, Wi-Fi, Bluetooth, camera and audio drivers remain outstanding. |
| Native hardware survey | The handset's native generic image reached PID 1 with identity checks PASS. UFS reports LINK_READY with its existing clocks; USB identifies DWC3 v3.30a in device mode. Touch lacks the expected APSS clock votes, and both GPU domains report powered off. These results do not establish sector reads, USB enumeration, touch input or rendering. The original LineageOS boot image and installed artwork were verified by device readback after the approved restore. |

## Rebuild

The DRE Actions workflow prepares a content-checked compiler cache, separated
by Xcode/SDK, build-script revision and profile. It caches compiler results,
keeps full patch verification and linking, and prints hit/miss statistics.
The original hook was bypassed by DXB's exported compiler variables; the
corrected hook now wraps the selected compiler after those guards. Ten real
GNU make tests pass, and the workflow requires per-build cacheable-call
statistics. Actual macOS cache population, hits and time savings remain to be
measured on the next necessary kernel build.
Documentation and standalone diagnostic-test changes do not trigger a new
kernel build. A missing cache still performs the normal compilation.

Use Python 3.10 or later and Zig 0.14.1 on the host. The probe has no Apple
firmware dependency. From the repository root run

```sh
python devices/dre/build.py --zig /path/to/zig --out devices/dre/out
python devices/dre/test_probe.py devices/dre/out/dre-probe-boot.img
# Optional: add --dtb /private/path/live.dtb to test with your actual device tree.
# test_probe.py requires unicorn==2.1.4 on the host.
```

For the exact tested kernel, download the `xnu-kernel` artifact from
[upstream build 38779441895](https://github.com/dipa-studio/ios-sweet/actions/runs/38779441895).
The builder checks the underlying Mach-O's SHA-256 before applying its
fixed-build changes. This build precedes the Redmi UFS driver. The early
framebuffer constants and both mappings must be retargeted even though the
normal serial framebuffer driver reads the ADT.

```sh
python devices/dre/build.py --zig /path/to/zig --out devices/dre/out \
    --upstream-0027 /path/to/kernel.development.sweet
```

Serial and UUID detection require no private build input. The shim reads
the bootloader's live serial and fills reserved ADT slots after payload
validation. Missing, invalid or duplicate serials stop the handoff. The
UUID is `uuid5(NAMESPACE_URL, "urn:oneplus:dre:" + serial)`, preserving the
identity used by earlier builds. Only serial and UUID slots are changed.

Optionally add `--identity /private/path/phone.private.json`. Its optional
`serial` string adds an expected-value diagnostic check. Its optional `imei`
string must be the phone's checksum-valid 15-digit IMEI. The IMEI is still
explicit provisioning. It is not discovered from modem NV by this shim.
Such a private image and its intermediate files contain the supplied values.
Rebuilds use fresh random seed bytes in the ADT. Synthetic identity fixtures
test serial parsing, UUID compatibility and malformed ADT rejection without
reading a phone.

```sh
python devices/dre/test_identity.py --zig /path/to/zig
```

The source-build path is the fork's upstream macOS/Xcode kernel
workflow with `machine_config=DRE`, `macos_version=13.5`. It stages changes
through 0027 plus the port's identity backport. All kernel patches were
checked against the source release. The first successful native DRE kernel
is [build 41904068757](https://github.com/FloofyProtoBomb/ios-sweet/actions/runs/41904068757),
commit `21e955e7c39fbf6665377768bbf00de71e95945d`. It passed QEMU PID 1,
model/board and exact UUID checks with a synthetic test identity. The native
phone image passed all fourteen constrained-MMIO scenarios with the real
FDT. The same image subsequently reached PID 1 on the handset, reported
automatic serial detection and passed identity checks. Its original boot
partition was restored and verified afterward. Pass the native kernel
with `--xnu /path/to/kernel.development.dre`.

The Linux QEMU workflow uses the upstream screen decoder and requires
`qemu-system-aarch64` and `llvm-nm` on PATH.

```sh
OUT=devices/dre/out
python devices/dre/virt_payload.py "$OUT" "$OUT/kernel.development.dre" --test-serial QEMU-DRE-TEST
python tools/qemu-sweet.py "$OUT/virt-payload.bin" "$OUT/qemu" \
    --fb-base 0x85200000 --cpu cortex-a76 --kernel "$OUT/kernel.development.dre" \
    --secs 150 --fail-on-panic
```

The expected console includes `DRE userspace: init running as PID 1`,
`DRE identity checks PASS` and `DRE init: still alive`.

### Combined hardware test

Pass `--board-tests` along with the kernel argument. Private identity input
is optional.
The default survey reads registers only. It checks Holi's UFS power domain
and branch enables before reading the UFSHCI version, enable, host status
and outstanding-transfer bits. The read helper catches a synchronous EL1
data abort. It never submits a storage command. The N200 host is at 0x04804000, has a
QMP v3 PHY at 0x04807000, and uses SMMU stream 0x60. A read-only check
under LineageOS verified a 4096-byte logical block size and valid GPT
header and entry-array CRCs on LUN 0. Userdata is entry 16, with 13,288,915
logical blocks. These are comparison values for future native reads.
The Redmi driver's direct DMA address assumptions have
not been established on this phone.

After md0 mounts, the diagnostic repeats a results card about every five
seconds. `LINK_READY` means the device-present and transfer-list-ready
bits are set on an enabled, idle host. It does not establish sector I/O.
`GPU_DT` and `TOUCH_DT` identify FDT nodes. `PANEL_SEL` identifies the
panel selection in the actual boot arguments and `SERIAL` compares the
bootloader's own serial to an optional private expectation. A generic build
reports `SERIAL=ABL_DETECTED` after successful discovery. No personal value is printed.
Record the card after PID 1 has started. If boot stops earlier, capture the
last changing line instead.

Add `--ufs-clock-enable` to opt into enabling the four UFS GCC branches.
This changes only their enable bits, with a 2 ms bounded poll per branch
and rollback of changed enables on failure. Root clock configuration,
resets and DMA remain untouched. `UCLK_PRE` and `UCLK_NOW` report fabric,
AHB, AXI and UniPro in that order. `ROOT_AXI` reports AXI and UniPro
root command/configuration values. `UCLK` describes the clock operation.
Linux's HWCG semantics are preserved, so an enabled HWCG branch can be
ready while its idle halt bit is set.

The same survey checks DWC3 device mode, GENI SPI firmware readiness and
GPU register access. Each aperture is accessed only after DT, clock and
power checks. `USB`, `USB_CLK`, `TOUCH_SPI` and `GPU_HW` include the
result and raw register values. These are boot-handoff probes. USB
enumeration, touch transactions and accelerated rendering require drivers.
In particular, iTunes pairing and lockdown are not implemented.

Fourteen simulated power, clock, host, peripheral and invalid-DT scenarios
pass for both the read-only and enable variants. Tests restrict MMIO writes
to the four enable bits, verify rollback, reject peripheral accesses when
power or clocks are off, and verify that all fields fit the boot arguments.
Run the tests with the private image and live DTB outside the repository.

```sh
python devices/dre/test_board_tests.py /private/platform.img /private/live.dtb
# Add --ufs-clock-enable when the image was built with that option.
```

The combined DE2118 handset test produced `UFS=CLOCK_GATED` alongside
the successful PID 1 and identity checks. It identified A619v1,
NT36672C_SPI0 and the selected TIANMA_NT36672C panel. At least one of
the inspected AHB, AXI or UniPro clock branches had its halt bit set.
Consequently the original survey skipped all UFSHCI reads. That result
does not distinguish disabled clocks from enabled, automatically gated
branches. The expanded native handset test reports `UFS=LINK_READY` and
`UCLK=READY` with identical before/after values. Its enabled AHB branch is
automatically gated while idle. The next storage step is a validated DMA
mapping and read-only command path. The handset-tested image has no storage
commands; the additional opt-in bundle below implements that next test.

### Next storage, USB and touch bundle

These components are shim diagnostics, not XNU device drivers. The approved
handset test reached PID 1 with identity checks passing. Both UFS and USB
streams are translated (context banks 2 and 1), so storage commands were
correctly skipped. Touch clock voting/restoration and firmware layout checks
passed; GENI reports a GPI-only interface, requiring DMA rather than PIO.
USB is halted without endpoints/event buffers and GPU remains powered off.
LineageOS and the artwork were restored and hash-verified afterward.
Use the same native kernel with these additional flags:

```sh
python devices/dre/build.py --zig /path/to/zig --out /private/bundle \
    --xnu /path/to/kernel.development.dre --board-tests --ufs-clock-enable \
    --dma-probe --ufs-read-test --touch-preflight
python devices/dre/test_bundle.py /private/bundle/dre-xnu-boot.img /private/live.dtb \
    --elf /private/bundle/dre.elf
python devices/dre/touch/test_firmware.py --zig /path/to/zig
```

`--dma-probe` reads the existing ARM SMMUv2 stream routes for UFS SID 0x60
and USB SID 0xe0 at APPS SMMU 0x0c600000. It handles masked and extended
stream matches. Translated, faulting, ambiguous, inaccessible and unsupported
routes are never treated as physical DMA. No SMMU register is written.

`--ufs-read-test` requires the DMA probe. Only an observed physical bypass
route permits the private DMA ring to run. Commands are restricted to NOP,
TEST UNIT READY, READ CAPACITY(16), and one-block READ(10) on LUN 0.
The test checks the 4096-byte block size, primary GPT header and array CRCs,
and a unique, bounded userdata partition. It does not read userdata contents
or send media writes. Host interrupt enable, list base and run/stop state
are saved and restored. Polls are bounded. An unquiesced request or failed
restoration stops the XNU handoff and requires a reset. No host reset,
RPMB command, crypto-engine update or GPT modification is attempted.

`--touch-preflight` temporarily votes only GCC bits 6, 7 and 10 for the
known SPI engine. It reads GENI protocol, busy state and FIFO availability,
inspects the SPI pin mux and IRQ input, and restores its clock votes.
It also selects firmware-data-0 or firmware-data-1 from the live FDT using
the bootloader's `oplus_bsp_tp_custom.dsi_display0` panel suffix. The bounded
parser checks the Novatek end marker, version complement and ILM/DLM/info/
overlay ranges. `LAYOUT_OK` is a file-layout check, not authentication or
a hardware CRC result. No proprietary blob is embedded in the image or
repository. No pin is changed, touch reset asserted, SPI command sent or
firmware uploaded. This does not produce touch events or HID devices.

The USB addition reads DCTL/DSTS and the endpoint/event-buffer configuration,
alongside the USB DMA route. It does not consume events, configure endpoints,
enumerate a gadget, or provide iTunes/lockdown pairing. GPU power readiness
and runtime serial/UUID checks remain in the same image.

The shorter results card uses `DMA_UFS`, `IOMMU`, `UFS_IO`, `USB_STATE`,
`DMA_USB`, `TOUCH`, `TVOTE` and `TOUCH_FW`. Legacy raw clock fields are
omitted to keep the report within the kernel's 608-byte boot arguments.
`UFS_IO=GPT_PASS` means the shim read the synthetic/physical GPT on that
particular run; it does not expose a disk to XNU. `DMA_BLOCKED` is an
intentional skip, not evidence of a broken storage controller. `SPI_PIO_READY`
means register preflight succeeded, not that the touch controller responded.

The exact-image model supplies a synthetic disk with an independently
constructed GPT, inspects the actual UTRD/UPIU/PRDT bytes, rejects non-read
commands, verifies clock/list restoration, and checks unsafe handoff blocking.
It covers SMMU routes, storage protocol/CRC/timeouts, touch clock/firmware
selection and USB states. Guarded-access failures are injected at the helper
boundary; Qualcomm bus timing, SError and real exception delivery are not
emulated. A physical test still requires a separate approved boot-partition
write and subsequent restoration.

## Hardware reference

### UI and SMP kernel profile under development

`devices/dre/stage_xnu.py PATCH_DIRECTORY --profile ui` stages the generic
iOS userland, IOSurface/AppleCLCD, CPU-rendering and eight-core PSCI paths.
The workflow selects it automatically for a push to `dre-ui`; a manual
workflow run needs `machine_config=DRE` and `dre_profile=ui`. The ordinary
`bringup` profile remains the handset-tested early kernel configuration.

The first 64-patch UI profile passed [native build 41923540367](https://github.com/FloofyProtoBomb/ios-sweet/actions/runs/41923540367).
The 65-patch profile including the explicit QEMU-affinity adapter passed
[native build 41928302512](https://github.com/FloofyProtoBomb/ios-sweet/actions/runs/41928302512).
The DRE display/CPU tree and metadata checks are implemented; a full iOS root
volume and a working DMA/storage driver are still needed. Image `8cd8bf6f`
confirmed eight active CPU cores and IOSurface/framebuffer startup on the
physical DE2118. It used diagnostic PID 1, with no rendered iOS GUI.
The profile excludes the Redmi UFS, touch,
PMIC, USB and display-power drivers. It cannot use that phone's DMA assumptions
on the N200. A build-log gate rejects any skipped DRE patch.

The builder accepts `--ui-services --cpus 8` with a native UI/SMP kernel.
It validates the live FDT's eight Kryo CPU affinities and SMC PSCI method
before handing off. `--product-adt /private/d22ap.adt` optionally imports the
real iPhone X product metadata; Apple artifacts stay outside git. This still
uses diagnostic PID 1, which reports actual selected/active CPU counts.
It does not launch Setup.app or SpringBoard by itself.

For QEMU, `virt_payload.py ... --qemu-smp` changes only the virtual CPU reg
cells to Aff0 and adds `dre_qemu_affinity=1`. QEMU needs
`virt,virtualization=on` to provide SMC PSCI, with `qemu_start.S` entering
EL1 from EL2. The phone's image does not enable the virtual-affinity option.

`qemu_ui.py BUILD_DIRECTORY KERNEL --out QEMU_DIRECTORY --zig ZIG --qemu QEMU`
boots the generic `--ui-services --cpus 8` diagnostic with the unchanged
native kernel. It verifies actual `hw.ncpu` / `hw.activecpu`, interprocessor
interrupt startup, PID 1 / identity checks, and successful IOSurface and
framebuffer driver-start records. The diagnostic uses the kernel message
buffer API, not the ASL stream, and never prints raw kernel messages.
This check passes with eight active virtual CPUs. Peripheral report fields
are explicitly simulated; neither an iOS GUI nor N200 SMP is proven by it.

`--dma-context` together with `--dma-probe` snapshots validated UFS/USB
SMMUv2 context-bank metadata through guarded reads. It does not change
SMMU registers, walk page tables, grant DMA access or issue translation
commands. Storage I/O remains blocked on a translated route. The
exact-image MMIO suite accepts `--dma-context` to exercise these states.
The hardware's ID1-derived register space can be smaller than the DT
reservation: restored LineageOS reports 512 KiB inside a 1 MiB resource.
Context offsets therefore come from ID1; every bank must still fit the
hardware span and the complete layout must fit the validated reservation.

`--driver-prep` requires `--dma-context --ufs-read-test --touch-preflight`.
It adds three development paths to the existing eight-core/display test:

* `UMAP` / `XMAP` inspect inherited AArch64 stage-1 4K page tables for the
  private UFS and USB arenas. Tables must lie in live RAM, outside every
  reservation, dynamic allocation range, shim, FDT and initrd. Descriptor
  rereads and route/context snapshots reject changes. `IDENTITY_RW`
  describes the tables only: ownership, translation caches and XPU access
  are not proven. It never grants DMA permission. Translated UFS I/O
  remains blocked.
* `USB_PLAN` reads DWC3 capability registers and validates setup/data/status
  transfer descriptors only while endpoints/event buffers are unused.
  `usb/transport.c` also decodes bounded event-ring snapshots. HWO stays
  clear, and no endpoint, pull-up or enumeration is started.
* `GPI`, `GDMA`, `GMAP` and `SPI_PLAN` survey the AP's five GPI instances
  under the checked temporary touch clock votes. Register offsets account
  for `qcom,gpi-ee-offset`, as in the matching Android driver. The SPI
  descriptor builder prepares one bounded CS0/8-bit full-duplex transfer.
  No channel allocation, pin change, reset, transfer or firmware upload
  occurs. Existing clock votes are restored before XNU starts.

The portable table walker and RAM/SPI/USB wire-format checks can be run with:

```sh
python devices/dre/test_dma_walk.py --zig /path/to/zig --out /tmp/dre-walk
python devices/dre/test_driver_prep.py --zig /path/to/zig --out /tmp/dre-prep
```

Pass `--driver-prep --dma-context` to `test_bundle.py` for exact-image MMIO
cases covering the new paths. Hardware facts come from the pinned Holi
[page-table implementation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/iommu/io-pgtable-arm.c),
[GPI driver](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/dma/qcom/gpi.c),
and [DWC3 definitions](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/usb/dwc3/core.h).

`--ufs-handoff-read` is a separate opt-in on top of `--driver-prep`.
It recognizes the phone's observed stage-1-disabled / stage-2-bypass
configuration, without modifying SMMU registers. This is different from
an enabled translation context: the latter stays blocked even if its
page tables contain identity mappings. A private arena below 4 GiB,
known attribute behavior, stable route/context snapshots and no active
context faults are required. The guard runs before/after every storage
command and before allowing kernel handoff. A lost guard or failed host
restore stops handoff until reset. Only the existing NOP, TEST UNIT READY,
READ CAPACITY(16) and single-block READ(10) allowlist is used; LUN is fixed
to 0 and no media write is implemented. See
[Arm SMMUv2 sections 2.3 and 16.5](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb).

The physical `ce4ffafd` survey found UFS/USB context MMUs disabled, GPI
idle mask `1f`, SPI descriptor preparation OK and GPI's SID in unmatched
fault mode. It also found a 64-bit DWC3 bus address width, which the
initial checker rejected. The corrected USB builder accepts widths up
to 64 with checked end addresses and no shift-by-64. No USB function or
GPI DMA mapping is configured by these follow-up changes.

Physical image `16dd2e95` passed the guarded UFS capacity/GPT read: five
read-only commands, `GPT_PASS`, and `userdata` start/count matching the
private Android baseline (4096-byte blocks, about 50.69 GiB). The corrected
64-bit USB capability/TRB-plan check also passed; CPU/display/PID 1 checks
remained successful. The original LineageOS boot image was restored and
readback-hash verified afterward. This proves the early handoff read path;
a native XNU block device and persistent iOS filesystem are still needed.

The final filesystem will be sized against its verified target partition;
the root-image script's default 16 GiB is not intended to limit usable space.
Any later partition/layout write needs separate confirmation. No Android
partition is being resized during this build work.

The verified splash reservation is 0x85200000 + 0xc00000. GICD is at
0x0f200000, GICR at 0x0f240000, and the virtual timer uses INTID 19 at
19.2 MHz. The shim checks that the entire 512 MiB XNU window starting at
0x100000000 fits in a live RAM bank and conflicts with no fixed
reservation, dynamic allocation range, shim, DTB or initrd. The top 1 MiB
is reserved for the panic log.

### Direct XNU boot image

Add `--direct-xnu` to the native `--install-root` build to embed a flat XNU
memory image in the ABL kernel image. The builder resolves segment placement,
zero-fill, ADT placement and XNU boot arguments ahead of time. The linked ABL
entry checks the live board and owned memory, copies that image, applies
per-boot ADT fixups and transfers control to XNU. The boot artifact omits
the legacy segment loader. The host-only `payload.bin` is retained for
inspection and the existing virtual XNU tests. The default diagnostic
and install build paths retain their existing boot format.

For GPU development, `gpu/a619_packet.h` contains the packet used in the
physical test. The Android-only test uses the matching kernel's
`include/uapi/linux/msm_kgsl.h`. Build with
`zig cc -target aarch64-linux-musl -static -O2 -I/path/to/kgsl/header devices/dre/gpu/kgsl_test.c -o a619-test`.
It writes a marker into its own GPU allocation and waits for its timestamp.
Hardware acceleration under iOS would additionally need a working graphics
service and userspace interface. [Holi GPU device tree](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/holi-gpu.dtsi)
and [Mesa Freedreno/Turnip](https://docs.mesa3d.org/drivers/freedreno.html).

## Setup Assistant and local activation

On a Linux host with a mounted, writable root image built from the matching
IPSW, run `sudo python3 tools/sweet-root-tweaks.py MOUNTED_ROOT --first-run-setup`.
This checks Setup.app's bundle identifier, clears the upstream completion
flags, retains mobileactivationd and prepares the existing local activation
data ark. The DRE ADT retains the upstream development security-status
properties used by mobileactivationd's offline path. If iOS hasn't created
its mobileactivationd data container yet, run the tool with
`--activation-only` against the mounted image after the container exists.
That mode preserves Setup progress. The preference and activation paths
were tested with filesystem fixtures. The on-device wizard, Apple ID and
cellular services have not been demonstrated.

## Boot artwork

The custom logo image is based on the original 16 MiB slot-B DDPH partition.
It uses the Apple logo, recovery image, low-battery layers and plug glyph
from iPhone X build 20H392. The phone remains in fastboot when it displays
the recovery artwork. The bootloader controls any menu or warning it draws
over these bitmap resources.

Extract the corresponding `applelogo@`, `recoverymode@`, `batterylow0@`,
`batterylow1@` and `glyphplugin@` members with `tools/remotezip.py` from
the [target IPSW](https://ipsw.me/download/iPhone10,3/20H392). With Pillow
installed on the host:

```sh
python devices/dre/boot_art.py /path/to/Firmware/all_flash /tmp/dre-art
python devices/dre/logo.py original-logo_b.img /tmp/dre-art/power-on.bmp logo.img \
    --fastboot /tmp/dre-art/fastboot.bmp --low-battery /tmp/dre-art/low-battery.bmp
```

The packer checks the original partition's SHA-256, retains its headers and
copies every other compressed resource unchanged. Six of the eighteen
entries change. The original verified slot-B backup restores the artwork
using LineageOS fastbootd. This handset's bootloader fastboot rejected writes
to logo_b. Recovery-side fastbootd accepted the write without a critical unlock.

The read-only libfdt sources under `libfdt/` are from dtc commit
54e547173bf0f2a4cc35eba8b5738caf38b9230f, under the included BSD license.
