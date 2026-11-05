# Native touch input lifetime — UNVALIDATED

After user-directed resumption, the production input/report host fixture
passes424 ASan/UBSan checks and the actual selected native adapter/SweetHID
source compiles locally. See README for scope. Physical IRQ/DMA/panel and iOS
acceptance remain unproven. The public `dre_tn_hid_frame` layout is unchanged.

The existing normal report decoder already indexes contacts by the Novatek
contact ID minus one rather than packet order. A reordered packet therefore
keeps the same ten contact slots. The complete valid active mask describes
the current controller frame: absence releases a preceding contact, and a
checksum-validated empty mask is a genuine all-up report. Corrupt reports,
checksum failures and early watchdog patterns do not become empty frames.

These facts come from the pinned
[Novatek normal-mode parser](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/input/touchscreen/oplus_touchscreen_v2/Novatek/NT36672C_noflash/nvt_drivers_nt36672c_noflash.c),
not a synthetic touch stream. `nvt_get_touch_points` assigns `points[pointid]`
from the controller ID, accepts the two active states, and builds an attention
mask. `nvt_trigger_reason` separately rejects recovery/checksum failures. The
local decoder performs the corresponding bounded payload interpretation.

The existing SweetHID fragment constructs one complete digitizer collection
using the retained real `SweetHIDTouch` service. Slots0..9 map to its existing
Index0..9 and Identity2..11 contract. It includes previously down contacts that
are absent from the new mask as releases at their preceding locations, uses
one actual timestamp for the batch, and commits contact state only after
every event/child append allocation succeeds. The physical-panel transform
is applied once there. `input.c` does not invent another identity numbering
scheme, re-sort contacts, scale coordinates or allocate a second HID ABI.

The submit callback must be transactional: `DRE_TN_OK` means that complete
collection was accepted and the service committed the whole contact state;
a failure must preserve its preceding state. The present fragment returns
success only after calling `dispatchEvent`. There is no backboardd consumption
or acknowledgement ABI, and no successful iOS input claim follows from this.

## Concrete lifecycle correction

Previously a worker retried its held frame before checking whether its
controller generation or transport was still the same stream. It also failed
to retry a rejected fault-cancellation batch, because retry admission required
the controller still be READY. A controller fault consequently could leave
previously submitted contacts active indefinitely while the real HID service
was temporarily unavailable.

The private input object now latches terminal stream loss on observed
controller/transport state loss, a generation mismatch, or a worker-observed
GPIO source-ownership failure. It checks stream identity before held replay,
before each report read, and after the actual read. Only a fresh, actually
READY/RUNNING controller with a nonzero real boot generation can initialize a
new input object. An existing lost input object is never reinitialized in
place, and no artificial generation increment performs recovery.

On loss, a held batch that was never accepted is discarded. The worker may
submit only an explicit `DRE_TN_DEVICE_LOST` cancellation of contacts already
accepted from this producer. Their last accepted positions and original
generation remain associated with those releases. This is a cancellation,
not a fake controller report or an assertion that physical contacts lifted.

If cancellation is rejected, the last accepted active mask remains retained as
the pending release. The existing retained-service scheduler may retry that
one batch once per worker invocation with its existing10ms delay. Retry is
allowed even while the controller is faulted; it never reads SPI, rearms an
IRQ, resets firmware, changes a panel rail or clears the loss latch. Successful
cancellation still returns recovery rather than claiming valid input or a
restored controller. A failed scheduler/submit remains an explicit refusal
with the actual contact state retained for an owner stop/retry.

There is no guessed age deadline. This protocol has no supplied authoritative
frame sequence/timestamp with which to invent a latency-based all-up rule.
An authentic held frame in the same still-owned stream is retried in order;
it is stale for this object when stream identity/state is lost. Malformed
frames continue to preserve the preceding contacts rather than cancelling
them without evidence.

Owner stop still joins IRQ/workers, discards never-delivered held frames, and
cancels only previously submitted contacts. Failed cancellation preserves
those contacts for an explicit stop retry. It grants no hardware teardown,
allocation free, automatic firmware recovery or resource ownership.

## Remaining prerequisites

Actual selected GPIO/GIC dispatch, panel/source/power ownership, SID0x16 DMA,
firmware handshake, real controller reports and retained HID event delivery
remain unproven on the native phone path. The SweetHID service and all producer
lifetimes must remain serialized under their real binding policy.

The kernel-private sleepable `dre_tn_input_source_lost(input)` entry point
connects an actual outer source-owner fault to the same cancellation logic.
The actual `DRENativeTouch` adapter now invokes it instead of bypassing input
after real atomic IRQ-owner failure, under the same serialized work-loop gate.
Its primary failure paths only latch the fault and post the existing retained
coalescing worker memory when dispatch is enabled and not closing. They neither
call this sleepable API from IRQ nor acknowledge an invalid/foreign source.
Startup also posts a fault observed before dispatch became enabled.

Pin sample/rearm callbacks refuse an observed fault. Normal HID submissions
also refuse an observed fault; explicit device-loss/owner-stop cancellations
remain deliverable. An already in-flight transfer may finish under its actual
GPI lease; this software notification neither aborts DMA nor proves retirement.
The atomic checks are observation boundaries, not an assertion that IRQ and
HID dispatch are one indivisible transaction. No fault flag is cleared and no
controller, source or panel is automatically recovered.

The entry point refuses stopping or concurrent worker execution, clears only
its coalesced software work mark, latches terminal loss and attempts at most
one previously accepted-contact cancellation. It shares the exact delivery
and10ms retry helpers used by `input_worker`, keeps failed releases retained,
and never calls GPIO, SPI, IRQ, quiesce, panel, reset or MMIO operations. It
does not fabricate a controller FAULT state or clear actual source-fault
evidence. Acceptance returns recovery rather than healthy input. No native
source ownership, IRQ restoration or service consumption is implied.

The host fixture covers ID reorder, partial/all-up masks, corrupt-frame
preservation, held generation loss, rejected cancellation then accepted retry,
scheduler refusal, no post-loss SPI/rearm and stop/source-loss retry lifetimes.
It also exposed a delivery timer incorrectly marking a new report pending:
delivery-only retries now preserve the independent real IRQ/report mark and
cannot force a stale SPI reread at HIGH GPIO. Actual phone event delivery and
iOS consumption remain unproven.
