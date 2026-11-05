# Restricted Holi RPM-SMD voltage vote transport

This slice implements actual RPM request, GLINK FIFO and IPCC publication
code. A successful vote requires a matching firmware ACK. It has passed host
tests; it has **not run on the N200**, observed a physical RPM ACK, qualified a
voltage policy, or established GPU power readiness.

`rpm_smd.c` uses the vendor `drivers/rpmsg/rpm-smd.c` protocol, rather than the
different generic `drivers/soc/qcom/smd-rpm.c` implementation. Source revisions,
hashes and selected live-FDT facts are pinned in [sources.json](sources.json).
The original implementation here is MIT; no GPL driver implementation is
vendored or relabeled.

## Selected hardware and wire identities

The actual live supply properties override the names suggested by node paths:

| GPU supply | Child regulator | RPM wire type | Resource ID | DT level range |
| --- | --- | --- | --- | --- |
| CX | `pm6150a_s1_level` | `rwcx` | 0 | 16–512 |
| MX | `pm6350_s5_level` | `rwmx` | 0 | 16–512 |
| GX | `pm6350_s3_level` | `rwgx` | 0 | 0–512 |

All three children have `qcom,use-voltage-level`, `qcom,set=3`, and no voltage
offset. Values such as 16 and 512 are RPM **levels**, despite the DT property
names containing “microvolt.” They are not direct PMIC voltages. The provider
refuses path-derived `smpe1` / `smpa5` / `smpa3` resource identities, raw `uv`,
legacy `corn`, floor-level and sleep-set changes. Its only request is an active
`swen=1` plus `vlvl=<qualified level>` hold/raise.

The live RPM node names `rpm_requests`, channel type 15, and has no `reg`
version register. The pinned vendor default therefore selects RPM wire v0.
This is an inferred expected protocol, not an observed ACK. The explicit
version selector also recognizes mapped value `0x31726576` as v1; unknown or
failed version reads are refused. The v1 codec has only synthetic verification.
GLINK's negotiated version 1 is separate from the RPM payload version.

The fixed transport map is RPM message RAM `0x045f0000` / `0x7000` bytes and
IPCC MPROC `0x00208000` / `0x1000` bytes. AOP client 0 / GLINK_QMP signal 0
packs to zero: the actual doorbell is a zero store to SEND_ID at `0x0020800c`.
The MMIO adapter accepts only this map/endpoint and never enables, clears or
changes another client's receive interrupt.

## Implemented transaction

1. The caller holds exclusive ownership, Device mappings and a stable lease.
   The helper reads and validates the `grt0` TOC, `ap2r` and `r2ap` FIFO ranges,
   non-overlap, lengths and cursor alignment. Both FIFOs must be empty. Their
   positions may be nonzero: the helper never resets or rewinds inherited
   indices and never discards inherited messages.
2. It sends GLINK VERSION 1 / features 0, receives a valid VERSION_ACK, and
   completes both OPEN directions for exactly `rpm_requests`. The two channel
   IDs are tracked separately. Unknown versions, channels and commands fail
   closed. This restricted transport is not a general GLINK multiplexer.
3. Intentless TX_DATA contains one bounded RPM request. FIFO stores wrap at
   the advertised data length, reserve one alignment slot, and are ordered
   before publishing the owned head. The IPCC SEND_ID store then notifies RPM.
4. A complete incoming frame is copied before advancing the owned RX tail.
   Partial frames are preserved. Fragmented or oversized payloads, wrong
   channel IDs, unsupported intents and unexpected control traffic are refused.
5. The active vote is recorded only after a well-formed, matching message-ID
   ACK, the deadline check and a final ownership check. IDs start at 2 and
   never wrap/reuse. The normal v0 `req` / `msg#` ACK and bounded `err` NACK are
   checked; ambiguous legacy v0 service-3 ACK encodings are deliberately refused.
   The v1 ACK/NACK formats are likewise bounded. A foreign ACK cannot complete
   a vote.

The API is synchronous and must be serialized by its kernel owner; its structs
are private kernel state, not user-controlled shared memory. Each call is
bounded by a checked 19.2-MHz monotonic counter, a caller budget of at most one
second, and a finite poll limit if the counter stops. Callbacks must themselves
be bounded. Ownership must be invalidated on reset, suspend, mapping loss or
remote restart. No outstanding request or uncertain store may be retried by
zeroing this state.

After any uncertain publication, NACK, timeout or ownership loss, the session
is poisoned. Existing acknowledged holds are retained. The helper sends no
rollback, off or lower-level request: firmware may already have accepted the
request whose ACK was lost. Recovery requires the platform owner to establish
an actual safe reset/recovery state while preserving dependent resources.
`RPM_VOTE_ACKED` means firmware accepted one request; it does not assert a
powered GDSC, stable clock, active GPU, or completed Metal operation.

## Integration still required

The kernel platform owner must supply the real mappings, synchronization,
remote-reset detection, GIC_SPI334 (GIC INTID366) ownership/service or masking, and resource
lifetimes before calling this stack. Polling here does not register a GIC/IPCC
handler or clear receive IRQs. It cannot share FIFO ownership with another
GLINK driver; other remote channels require a proper multiplexer before use.
No kernel staging or native provider registration is included in this change.

Most importantly, **DT min/max values are not a qualified voltage plan**.
CX and MX serve CPU/memory and other consumers. The `allowed_levels` mask in
each resource row must come from an independently qualified, platform-wide
aggregate floor and frequency/corner policy; zero masks are refused. Use
`dre_rpm_level_mask(level)` to construct that explicit mask. The helper also
checks the live resource identities and ranges, refuses off/decreases, and
does not infer any mask from Android's 253-MHz GPU readback. Synthetic tests
supply their own clearly artificial policy.

The source's CX proxy starts at TURBO, whereas the GPU parent's retention
dependency is only one consumer minimum. Replacing that shared vote with
retention solely because the GPU runs slowly could undervolt other consumers.
Actual startup aggregates, rate-to-level tables and inherited clock bounds
remain unqualified. This stack must therefore not yet be used to claim a
complete GPU power provider.

## Verification

```sh
python3 devices/dre/gpu/platform/test_platform.py --out /private/rpm-host-tests
python3 devices/dre/gpu/platform/test_platform.py --cc gcc --sanitize --out /private/rpm-asan-tests
python3 devices/dre/gpu/platform/test_platform.py \
  --zig /path/to/zig --out /private/rpm-cross-tests
```

The current production-code fixture passes **6,806 synthetic cases** on GCC
and Zig. Literal v0/v1 packet vectors are independent of the encoder. A
synthetic remote firmware consumes published circular FIFO frames, performs
the negotiation/open dance, and returns ACKs or faults. Tests cover wraparound
from nonzero positions, no inherited cursor reset, partial RX preservation,
TOC/index/range corruption, wrong resource names, unsupported version/map/
endpoint, foreign/malformed/NACK/late ACK, stopped timer, forbidden policy
changes, monotonic IDs and retained prior holds.

Faults are injected at all 110 read, 40 write, 5 mailbox and 330 lease-check
positions in a successful open-plus-vote path. Write/mailbox faults occur
*after* the simulated effect, ensuring uncertain stores cannot look successful.
The concrete MMIO adapter is also exercised through the complete transaction:
only message RAM and SEND_ID are accessed. GCC AddressSanitizer/UBSan passes;
all three production files compile freestanding for ARM64 ELF and Mach-O with
`-Wall -Wextra -Werror`, general registers only and strict alignment.

These checks establish host protocol/state-machine behavior, not physical RPM
acceptance, voltage correctness, GPU operation or Metal support.
