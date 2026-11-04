# NT36672C report decoding

This portable C component decodes normal-mode touch reports for the N200's
Novatek controller. It is not linked into the current XNU image. SPI access,
controller firmware loading, GPIO interrupts and IOHID event delivery remain
to be implemented. It performs no hardware access or controller resets.

`dre_touch_decode` takes the payload after the SPI layer removes its command
and dummy bytes. It checks the 65-byte core checksum, decodes up to ten
contacts by ID, validates coordinates against firmware-provided inclusive
maxima, and returns a new active-contact bitmap. Pressure extensions belong
to the first two wire slots, regardless of their contact IDs. Coordinates
remain in the controller's raw units.

Malformed packets, duplicate active IDs and recovery/watchdog markers leave
the caller's previous frame untouched. A valid frame with no active contacts
allows the future HID layer to emit releases. The caller must handle gesture
mode separately and decide when repeated watchdog markers require recovery.

The wire-format reference is the matching published
[OnePlus/LineageOS NT36672C driver](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/input/touchscreen/oplus_touchscreen_v2/Novatek/NT36672C_noflash/nvt_drivers_nt36672c_noflash.c).
This module is an independent implementation of the protocol facts. It
contains no Linux driver code or firmware binaries.

Run the synthetic fixture suite from the repository root with Python and
Zig 0.14.1.

```sh
python devices/dre/touch/test_report.py --zig /path/to/zig
```

The test builds the host library and an ARM64 freestanding object. It covers
literal reports, checksum corruption, truncation, coordinate bounds, contact
ID errors, recovery markers, shuffled IDs, pressure extensions, release
frames and randomized multi-contact reports. The current suite passes 447
checks. No test reads SPI or reports real touchscreen input.
