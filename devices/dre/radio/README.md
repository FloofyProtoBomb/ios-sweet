# DE2118 Wi-Fi and Bluetooth source inventory

Wi-Fi and Bluetooth are **not working under this XNU port**. This inventory
records the handset's previously captured device tree and the matching
LineageOS sources. It contains no driver, radio firmware, calibration data,
MAC addresses, serial number or IMEI. Reading it does not require a phone.
The completed iOS filesystem image does not change these radio limitations.

`resource-inventory.json` contains selected hardware properties only.
`source-references.json` records 22 primary-source files with repository,
exact commit, source SHA-256 and permanent URL. The pinned kernel is
`98a38783f42f5588bf00cd933e299aedc589bc10`; the device configuration is
`e8476d2dcb058c24fbf657fd1f70d3052829bfa6`.

## Hardware established by the captured tree

| Resource | Observed configuration |
| --- | --- |
| WLAN binding | `/soc/qcom,icnss@C800000`, `qcom,icnss`; integrated SNOC/Adrastea host path |
| WLAN MMIO | `0x0c800000`, 8 MiB; a separate `smmu_iova_ipa` window at `0xb0000000`, 64 KiB |
| WLAN interrupts | Twelve copy engines, GIC SPI numbers 358–369, level high; these are DT SPI numbers, not Linux virtual IRQ numbers |
| WLAN DMA | Apps SMMU at `0x0c600000`, stream ID `0x80`, specifier flag `1`; IOVA pool starts at `0xa0000000`, size `0x10000000` |
| WLAN shared memory | Fixed 2 MiB MSA region at `0x86500000` |
| Bluetooth power binding | `/soc/bt_wcn3990`, `qcom,wcn3990`; TLMM software-control GPIO 69, active high |
| Enabled high-speed UART candidate | `/soc/qcom,qup_uart@4a84000`, `qcom,msm-geni-serial-hs`, 16 KiB register window; GIC SPI 328 and TLMM GPIO 64 wake input |
| UART pin configuration | Active function `qup01`, GPIOs 61–64; clock inputs `se-clk`, `m-ahb`, `s-ahb`; wake byte `0xfd` |

The UART is a **candidate**: the Bluetooth power node has no UART-child or
transport phandle establishing that association. Its exact Android tty
assignment, controller ROM/patch revision and operating baud rate remain
unverified. The `qcom,wcn3990` power binding identifies a WCN399x family
configuration; it does not establish the exact silicon revision.
The WLAN binding selects the downstream Adrastea path rather than the
different `qcom,wcn6750` path. [Holi hardware tree](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/holi.dtsi),
[ICNSS binding dispatch](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/soc/qcom/icnss2/main.c#L3923)

Both radios share RPM-SMD controlled rails: WLAN references PM6350 L4/L7 and
PM6150A L2/L10; Bluetooth references PM6350 L11/L7 and PM6150A L2/L10.
The board removes WLAN's second-chain `vdd-3.3-ch1` supply. Generic regulator
limits and per-consumer voltage requests are separate fields in the JSON;
neither is proof of the current powered state. Copying settings from another
Qualcomm platform could enable the wrong rail. [DE2118 board overrides](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/dre/holi-oem-common.dtsi#L44),
[ICNSS power implementation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/soc/qcom/icnss2/power.c),
[Bluetooth power implementation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/bluetooth/btpower.c#L124)

## Native driver requirements

WLAN needs more than an MMIO register driver. The pinned ICNSS path connects
to a QRTR/QMI firmware service, registers indications, exchanges MSA memory
information, reports MSA readiness, requests capabilities and manages power.
The service is WLFW `0x45`, version `1`, Adrastea instance `0`. Firmware
capabilities report the actual chip/board identifiers used to select board
data. No such response has been observed under XNU. Its remote GLINK/SMEM
transport, notifications and subsystem lifetime also need native support;
the presence of bootloader-reserved memory does not establish a live service.
[ICNSS startup sequence](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/soc/qcom/icnss2/main.c#L611),
[QMI transport and board-data selection](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/soc/qcom/icnss2/qmi.c),
[WLFW protocol definitions](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/net/wireless/cnss_utils/wlan_firmware_service_v01.h)

After that control path works, WLAN still needs owned SMMU mappings, coherent
copy-engine rings, interrupts, the WLAN firmware command/data protocols,
association/security/key handling, packet delivery and the interface expected
by iOS networking. Linux's SNOC HIF configures copy engines after enabling
WLAN; identifying its base address alone does not provide networking.
[SNOC HIF](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/staging/qca-wifi-host-cmn/hif/src/snoc/if_snoc.c#L157)

Bluetooth needs correct shared-rail sequencing, TLMM, GENI UART, flow control,
sleep/wake handling, HCI transport and controller patch/NVM loading before
an iOS Bluetooth adapter can expose it. The Linux WCN399x HCI source is a
protocol reference, **not evidence that its serdev binding is used on this
handset**: its `qcom,wcn3990-bt` UART-child binding differs from the captured
`qcom,wcn3990` power node. Firmware names depend on controller ROM version;
selecting a random `crbtfw`/`crnv` file is insufficient.
[GENI serial driver](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/tty/serial/msm_geni_serial.c),
[HCI UART reference](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/bluetooth/hci_qca.c#L1578),
[patch/NVM selection reference](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/bluetooth/btqca.c#L421)

The LineageOS device configuration loads `qca_cld3_wlan` and uses Android
Wi-Fi HAL/supplicant services and a Qualcomm Bluetooth HAL. These Linux ELF
modules and Android libraries cannot be loaded as XNU drivers or iOS
frameworks. Device firmware may eventually remain an opaque input for its
own peripheral, but it still needs a compatible native loader and protocol.
Per-device calibration/addresses must remain private; preserve the existing
modem/persist partitions and never replace them with another handset's data.
[WLAN module loading](https://github.com/LineageOS/android_device_oneplus_dre/blob/e8476d2dcb058c24fbf657fd1f70d3052829bfa6/init/init.target.rc#L102),
[Android Wi-Fi integration](https://github.com/LineageOS/android_device_oneplus_dre/blob/e8476d2dcb058c24fbf657fd1f70d3052829bfa6/device.mk#L398),
[Bluetooth HAL inventory](https://github.com/LineageOS/android_device_oneplus_dre/blob/e8476d2dcb058c24fbf657fd1f70d3052829bfa6/proprietary-files.txt#L133)

## First useful driver milestones

1. Establish a native shared power/clock/GPIO layer with reference-counted
   rails and reversible cleanup. Confirm the UART-to-Bluetooth assignment
   and controller version from existing Android evidence before selecting
   firmware or transmitting HCI commands. The first Bluetooth milestone is
   a bounded HCI command/response exchange with captured controller version,
   followed by clean shutdown; pairing/audio come later.
2. Implement the SMEM/GLINK/IPCRTR control boundary required for WLFW, with
   bounded message parsing/timeouts and no data DMA. First WLAN milestone:
   receive the actual service/capability response and complete the documented
   MSA handshake using the correct reserved region. Report this as control
   bring-up, without claiming association or packet transfer.
3. Add owned DMA, interrupts and firmware protocol operations, then measure
   WLAN scan/association/IP traffic and Bluetooth discovery/pairing through
   the iOS adapters. Successful hardware binding, firmware upload or an iOS
   toggle alone cannot satisfy those runtime milestones.

These steps are separate work after storage/UI bring-up. No radio activity,
firmware upload or phone partition operation was performed for this inventory.
