# RPM RAM layout description — UNVALIDATED

`rpm_layout.c/.h` are a bounded, pure original-FDT and captured-TOC metadata
interpreter. No tests, compilation, CI, recipe, emulator or hardware operation
has validated this overnight source. The module performs no MMIO, mapping,
allocation, mailbox notification, cursor mutation, IRQ operation or lease
acquisition. It supplies no native broker and changes no access exclusion.

The current `rpm_native` and `io_router` exclusion of the **entire** declared
MPM range remains unchanged. Every returned layout is explicitly **unowned**,
including layouts with no observed conflict. The API cannot export a transport
configuration, an access allowlist or a success/readiness predicate.

## Exact source and captured board facts

All implementation facts below are from LineageOS kernel commit
`98a38783f42f5588bf00cd933e299aedc589bc10`, using independent original code:

- [qcom-mpm.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/irqchip/qcom-mpm.c)
  defines the five MPM banks, word-width formula and register formula;
  `msm_mpm_timer_write` uses the first two words, the IRQ handler reads enable
  and status banks, and sleep clears status then sends its separate IPC.
- [qcom-mpm-holi.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/irqchip/qcom-mpm-holi.c)
  is the selected Holi pin table. Its few GIC entries do not reduce the shared
  bank width or grant ownership of unused wake bits.
- [holi.dtsi](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/holi.dtsi)
  declares the selected vMPM, IPC, interrupt and RPM message-RAM resources.
- [qcom_glink_rpm.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/rpmsg/qcom_glink_rpm.c)
  defines TOC placement, magic and12-byte LE entries, ap2r/r2ap IDs, cursor
  locations, FIFO payload length and wrap semantics. The existing native
  `rpm_glink.c` retains stronger private transport admission limits.

Source inspection of the already captured private original FDT selected:

| Resource | Actual declared value |
|---|---|
| Message RAM | `0x045f0000 + 0x7000` |
| vMPM | `0x045f01b8 + 0x1000` |
| MPM separate IPC | `0x0f40000c + 4` |
| MPM IRQ count | `96` |
| MPM GIC interrupt | SPI197, INTID229, edge-rising |
| GLINK | `qcom,glink-rpm`, reference to actual message-RAM node |
| GLINK mailbox | actual IPCC reference, client0/signal0, `rpm_smem` |

Only selected resource metadata was decoded. No phone read, RPM RAM snapshot,
identifier or private captured binary is included in this commit. FDT presence
describes address identity and routing; it establishes no active owner.

## Linux register footprint versus declared ownership

The pinned MPM driver uses `ceil(96/32)=3` words per bank. It places two timer
words first, then computes each bank word at
`((bank * 3) + index + 2) * 4`. Banks are ENABLE0, FALLING1, RISING2,
POLARITY3 and STATUS4. Thus the exact Linux-accessed footprint is17 words,
`0x44` bytes, within the larger declared resource:

| RAM-relative range, half-open | Source-defined interpretation |
|---|---|
| `0x01b8..0x01c0` |64-bit timer, low/high words |
| `0x01c0..0x01cc` | three ENABLE words |
| `0x01cc..0x01d8` | three FALLING words |
| `0x01d8..0x01e4` | three RISING words |
| `0x01e4..0x01f0` | three POLARITY words |
| `0x01f0..0x01fc` | three STATUS words |
| `0x01fc..0x11b8` | remaining declared MPM resource: **unknown, unowned** |

The smaller driver access footprint is not a firmware ownership map. It does
not prove the remainder is free, harmless to read, available for GLINK, or safe
to alias. RPM firmware, other execution environments and undocumented consumers
may use it. A range conflict with the exact Linux footprint is distinguished
from a conflict with the unknown declared remainder; both are reported, and
neither changes the existing full exclusion.

## Real GLINK TOC interpretation

The TOC is the final256 bytes of the declared message RAM: relative
`0x6f00..0x7000`. Its LE header contains magic `0x67727430` and count, then up
to20 entries of ID/offset/size. No FIFO offset is present in FDT or guessed by
this module; all candidate offsets must come from an explicitly supplied
already captured exact256-byte snapshot.

For the two known IDs, ap2r=`0x61703272` and r2ap=`0x72326170`, the pinned
parser sets tail=`offset`, head=`offset+4`, FIFO=`offset+8` and payload length
equal to entry.size. The full cursor-plus-payload extent is therefore
`offset + 8 + size`, not just `offset + size`. The descriptor checks that entire
extent for bounds, TOC overlap, peer overlap and MPM conflict using64-bit
arithmetic. It separately reports whether a candidate meets the current native
transport's8-byte alignment and128..0x4000 payload limits.

For an unknown ID, the pinned parser ignores the entry. It gives no authority
to assume that this foreign object is another FIFO with an8-byte header. The
descriptor records its declared offset/size span only, always marks unknown
semantics and keeps any extra/header/trailer/dynamic use unknown. A lack of
intersection with that declared span cannot prove a candidate or a gap free.
Duplicate known pipe IDs, missing pipes and malformed fields remain conflicts.

No cursor is read or reset. An apparently empty captured FIFO would still not
prove current ownership, producer retirement, a stable remote reset epoch or
exclusive use of GLINK's shared endpoint/mailbox/receive route.

## Primitives required before a policy change

Do not replace the full MPM exclusion with the smaller Linux footprint based
on this module. A future change needs real platform-wide authority for the
exact per-word/FIFO ranges, including all firmware and foreign consumers of
the declared MPM remainder. It must also provide actual GLINK remote-epoch and
producer/channel exclusion, source/receive/mailbox ownership, preserved MPM
wake/timer/sleep state, and coherent aliases for every rounded16KiB Device-map
granule. A real read-only snapshot provider must prove permission before
reading any bytes and establish stable metadata under its actual owner policy;
the snapshot parser itself creates no such provider.

Only after those actual native obligations are met could a separate reviewed
change consider a narrower scoped admission. The existing full exclusion,
missing native policy refusal, no cursor reset and no speculative vote/rollback
behavior remain in force.

Tomorrow's work must validate malformed FDT/TOC bounds, full8+payload extent,
unknown-ID behavior, overlaps, both MPM conflict classes and target compilation
before this description is used even in an opted-in diagnostic path.
