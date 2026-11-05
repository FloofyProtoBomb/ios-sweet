# Existing-source Holi GDSC and GPUCC branch acquisition

`holi_power.c` is a real, restricted acquisition component for the full live
Holi power-domain variant. It consumes acknowledged RPM holds, reads the
actual clock sources and parent branches, performs the published domain/reset
sequence, and checks branch/domain readbacks. Host tests pass. The code has
**not been staged or run on the phone**; native voltage/source/domain state
remains unproven.

## Required existing source profile

Before a store, the helper validates the caller's exact live-FDT map, the
19.2-MHz counter/reference, the same exclusive lease as the RPM transport,
and distinct matching ACK records for CX, MX and GX. A pending, changed,
foreign-resource or poisoned hold is refused. The qualified aggregate floors
must be explicit and no lower than the live boot/proxy CX/MX level384 and
GX init64. These source entries bound this component's accepted profile;
they do not independently qualify all current CPU/memory consumers. The
owner still needs a platform-wide policy. No frequency-to-shared-floor guess
or automatic RPM request is made by this helper.

It then captures the 37 registers enumerated in `holi_power.h`, with a validity
mask, initial/latest values, phase, write count and explicit last/failed-register
indices. The GCC GPU config AHB gate
is checked before any GPUCC access and again before every later GPUCC read.
Already-held GCC GPU AXI/MEMNOC branches and the GPLL0-div parent vote are
required. Missing parents produce evidence and an error, not a blind vote.

Only this deliberately narrow existing configuration is accepted:

- GCC fixed Fabia GPLL0 is locked/active in its voted run mode, with L31,
  fractional word `0x4000` and the published divide-by-two output. Its decoded
  main/parent rates are 600/300MHz.
- GPU Fabia PLL0 is running/locked with its published 506MHz configuration,
  calibration and analog-control words. L26 and fractional `0x5aaa` (initial
  configuration) or `0x5aab` (rounded-up rate request) are accepted. The exact
  integer-decoded GPU rates are 252,999,902 or 253,000,048Hz.
- GFX RCG selects PLL0 even output with HID3 (`CFG=0x103`); GMU RCG selects
  the GCC-div parent with HID2 (`CFG=0x602`) for 200MHz. Pending update/dirty,
  unknown modes and other source/rate configurations are refused. An initially
  gated root can become active through branch enable; both root statuses
  must be observed active at final validation.

No PLL, divider, RCG configuration, source-vote register or interconnect policy
is programmed. A bootloader that leaves PLL0 disabled, an XO safe mux, or an
unrecognized configuration will therefore be refused **before mutation**.
Its captured values identify the missing source-provider work. An Android
253MHz rate readback by itself cannot satisfy this native check.

## Actual full GDSC variant

The selected live DT contains the references that a filtered graph initially
missed. The helper requires these exact addresses/options:

| Control | Address / option |
| --- | --- |
| CX GDSCR | `0x0599106c`, clock-disable wait8 |
| CX hardware status | `hw-ctrl-addr` → `0x05991540`, timeout500us with the published additional retry window |
| GX GDSCR | `0x0599100c`, default timeout100us |
| GX reset controls | `sw-reset` → `0x05991008`; `acd-reset` → `0x05991160`; `acd-misc-reset` → `0x05998004` |
| GX domain control | `domain-addr` → `0x05991508`, with `qcom,reset-aon-logic` |

GDSC hardware-control/SW-override ownership, an inconsistent transition, an
inherited asserted reset, or an already-powered clamped GX is refused before
the first store. A fully on domain is borrowed and never reset or collapsed.
Already-enabled branch bits are likewise borrowed. There is no release API
that could disable someone else's clock or rail.

For cold domains the acquisition does the following:

1. Configure CX's wait field while collapsed, clear software collapse, order
   the writes, wait at least1us, poll the actual CX hardware-status register,
   and allow the published two1us memory-settle intervals.
2. For GX, assert all three reset bit0 controls in order, hold at least100us,
   and deassert them in order. Each RMW preserves that register's unrelated
   bits rather than replaying another register's stale image.
3. Assert domain AON reset bit4 for at least1us, deassert it, clear clamp bit0,
   then clear GX software collapse. Wait/poll/settle according to the pinned
   regulator implementation. Retention and unrelated fields are preserved.
4. Acquire only the named/critical GPUCC AHB, GX_CXO, CXO, CX_GMU and GX_GFX
   branch enable bits. Published delay branches receive their10us minimum;
   HALT branches use a bounded200us check. This restricted component also
   requires the common branch2 hardware ON/FSM-ON observation for delay
   branches; it conservatively refuses a gate whose activity cannot be seen.

Every attempted store has an ordered masked readback. All source facts, ACK
holds and lease are rechecked through the operation and again at completion.
`dre_holi_power_validate` performs only reads and can supply one part of a
later backend `platform_ready` check; it does not repeat a cold CP condition
after the GPU starts running.

After a failed/uncertain store or lost hold/source, the state is poisoned and
all resources must remain held for real recovery. There is no blind register
rollback, rail release or borrowed-domain disable. The owner must ensure no
live GPU/DMA work exists before calling acquisition: the cold GX path really
does assert reset and is not a diagnostic no-op.

## Checks and remaining native hooks

The existing `test_platform.py` entrypoint runs both RPM and power fixtures;
`--sanitize` adds GCC ASan/UBSan, and `--zig` cross-compiles all production
files for ARM64 ELF and Mach-O. The current fixtures pass 6,806 RPM plus 1,558
power cases (8,364 total). The power fault sweep covers467 read,17 possibly
effective write/readback, and894 lease-check positions. It verifies the literal17-store
ordering,100us/1us reset timing, preservation of unrelated fields, every
read/write/readback/lease failure position, source/ACK/gate loss, status and
counter timeouts, and all128 inherited domain/branch combinations. It uses
synthetic ACK records and MMIO, not physical voltage evidence.

Still needed for native operation:

- Bind the verified live graph and Device mappings, exclusive quiescent owner,
  IRQ/remote-reset handling, and real RPM-acknowledged platform aggregate.
- Qualify actual inherited CPU/memory/bus/source state. If the accepted PLL/
  RCG profile is absent, implement its source provider under a qualified
  voltage/rate policy before retrying; this helper will not synthesize it.
- Acquire/hold the GCC parent and interconnect bandwidth providers; this
  slice only verifies already-held parent evidence.
- Follow acquisition with SPTPRAC, static RBBM, authenticated firmware/secure
  transition, owned SMMU mappings and CP/shader submissions. These five
  branches and two GDSCs alone do not establish complete GPU or Metal support.

Primary register/order/rate facts are pinned in [sources.json](sources.json):
`gpucc-holi.c`, `vdd-level-holi.h`, the actual `gdsc-regulator.c`, `holi-gdsc.dtsi`,
`clk-alpha-pll.c`, `clk-rcg2.c`, `clk-regmap.c`, `clk-branch.c`, and `gcc-holi.c`.
