# Native RPM Device mappings

**UNVALIDATED source draft.** No source recipe, compilation, test, mapping,
interrupt registration, RPM message or phone operation has been executed.

`rpm_native.cpp` supplies actual XNU Device mappings and guarded32-bit accesses
for the existing GLINK/RPM codec. It independently follows the complete
boot-held original FDT's RPM RAM, GLINK mailbox, RPM channel, IPCC parent and
MPM resource identities. It maps the actual RAM/IPCC apertures, checks actual
PA and Device read/write translations using AT/PAR plus `kvtophys`, and uses
the checked19.2MHz physical counter and real memory barriers.

The advertised RPM RAM is0x045f0000+0x7000. The captured MPM vMPM resource
is0x045f01b8+0x1000, **inside that range**. The adapter hard-refuses every read
or write overlapping the MPM window, even if an invalid GLINK TOC points a
FIFO there. It never accesses MPM IPC, wake, PDC or GIC registers. Only the
qualified IPCC SEND_ID0x0c store of0 is admitted outside RPM RAM. The16KiB CPU
mapping granules cover extra neighboring bytes; those bytes grant no access.

All actual per-word TOC/FIFO ownership, full-granule Device alias compatibility,
exclusive GLINK/channel/remote-reset ownership, powered sources and the real
IPCC receive route remain a mandatory external platform broker. The broker
must preserve MPM and every foreign consumer and must reject competing live
channels. An empty FIFO or a header-declared range is not this authority.
The receive route is SPI334/INTID366; no IOKit source index is inferred from it.
No default broker, receive handler, IRQ mask/ACK/EOI or takeover is supplied.

The same actual native owner mutex covers every callback, protocol operation
and close. Partial acquisition returns a held object, and uncertain publication
retains mappings and leases. Release requires real remote/FIFO/CPU/device and
rail-consumer retirement. No lower/off/rollback message is sent automatically.

Creation performs no GLINK negotiation or hardware store. `binding` returns
real callbacks/configuration for a later explicit `dre_rpm_bind_mmio` and
`dre_rpm_votes_open`; neither is called here. The source-derived v0 selection
for an absent version register is a configuration fact, not an ACK. Actual
firmware negotiation and matched RPM ACKs are still required. This adapter
does not choose aggregate levels, qualify inherited APSS policy, prove rail
power, initialize a GPU or enable Vulkan/Metal.

After actual map/lease/alias qualification, creation now takes a guarded
read-only256-byte TOC snapshot and describes the two known cursor-plus-FIFO
ranges before any protocol publication. A malformed/duplicate/missing pipe,
TOC/peer conflict or overlap with ANY declared MPM byte refuses while retaining
the actual owner. The pinned Linux MPM access footprint is smaller than the
declared resource, but that unknown tail remains excluded. Unknown TOC IDs
remain unowned; no foreign header format or free gap is invented. Structural
description does not supply an ownership lease, and all original GLINK limits
and actual per-word broker checks remain in force. Raw snapshot bytes are not
exported or logged. This preflight source has not been executed.
Private owner-locked diagnostics retain a constant failure reason, described
conflict bits and attempted-write count after a failure. They perform no MMIO
and expose no raw TOC or public selector. The native provider can preserve that
specific reason instead of collapsing a shared-resource refusal into a generic
mapping failure. Copying diagnostics proves no lease, ACK or ready state.
