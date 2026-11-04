# OnePlus Nord N200 DE2118

This target boots XNU 8796 with one CPU and a diagnostic Mach-O as PID 1
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
| Identifiers | The private image carries the phone's real serial and checksum-validated slot-0 IMEI. XNU publishes the root serial using its platform expert and accepts a deterministic per-phone UUID from the ADT. Diagnostic PID 1 passed reads of hw.machine, hw.model, hw.product, hw.target and gethostuuid in QEMU and on the handset. The handset also reports SERIAL=ABL_MATCH. The IMEI is an OSData string at IODeviceTree:/oneplus-hardware/imei. A native CoreTelephony/Settings IMEI lookup still requires modem integration. Serial display in Settings is untested. |
| Adreno | The N200 reports Adreno619v1. On the handset a KGSL userspace test submitted an A6xx CP_MEM_WRITE packet and read its result back after the GPU fence. The XNU ADT supplies the GPU register ranges and interrupt. XNU GPU power, firmware, SMMU, submission, buffer sharing and iOS graphics user-client support remain unimplemented. |
| Input and display | The phone reported a Tianma NT36672C panel and Novatek no-flash touch on SPI. The console writes to continuous-splash memory. Touch firmware loading, touch events, display power and accelerated composition remain outstanding. |
| Storage and radios | The kernel image has no UFS driver. A verified UFS read path and a root volume are required before it can run a full iOS filesystem. Cellular, Wi-Fi, Bluetooth, camera and audio drivers remain outstanding. |

## Rebuild

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

Add `--identity /private/path/phone.private.json` for a private handset
image. Supply two JSON strings named `serial` and `imei`. Omitting the
option builds without those personal identifiers. A personalized boot image
and its intermediate files contain the original values. Rebuilds use fresh
random seed bytes in the ADT.

The source-build alternative is the fork's upstream macOS/Xcode kernel
workflow with `machine_config=DRE`, `macos_version=13.5`. It stages changes
through 0027 plus the port's identity backport. All kernel patches were
checked against the source release. The supplied binary was built using
the pinned upstream artifact path. Pass a separately built kernel with
`--xnu /path/to/kernel.development.dre`.

The Linux QEMU workflow uses the upstream screen decoder and requires
`qemu-system-aarch64` and `llvm-nm` on PATH.

```sh
OUT=devices/dre/out
python devices/dre/virt_payload.py "$OUT" "$OUT/kernel.development.dre"
python tools/qemu-sweet.py "$OUT/virt-payload.bin" "$OUT/qemu" \
    --fb-base 0x85200000 --cpu cortex-a76 --kernel "$OUT/kernel.development.dre" \
    --secs 150 --fail-on-panic
```

The expected console includes `DRE userspace: init running as PID 1`,
`DRE identity checks PASS` and `DRE init: still alive`.

### Combined hardware test

Pass `--board-tests` along with the kernel and private identity arguments.
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
bootloader's own serial to the private build. No personal value is printed.
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
branches. The expanded probe above has passed emulation and awaits a new
approved handset test.

## Hardware reference

The verified splash reservation is 0x85200000 + 0xc00000. GICD is at
0x0f200000, GICR at 0x0f240000, and the virtual timer uses INTID 19 at
19.2 MHz. The shim checks that the entire 512 MiB XNU window starting at
0x100000000 fits in a live RAM bank and conflicts with no fixed
reservation, dynamic allocation range, shim, DTB or initrd. The top 1 MiB
is reserved for the panic log.

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
