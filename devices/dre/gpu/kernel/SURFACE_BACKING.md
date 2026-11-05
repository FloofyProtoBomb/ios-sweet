# Retained IOSurface backing — UNVALIDATED source draft

This is overnight source implementation only. No test, compiler, source recipe,
kernel link, CI, QEMU, image operation or phone operation has run for it. The
earlier tested IOSurface/CPU-rendering reports remain evidence for their original
artifacts; they do not qualify this new bridge or prove GPU/Metal acceleration.

## Actual selected backing and share right

The selected `SweetSurface.cpp` comes from project patch0046, lookup correction
0068 and DRE lifetime/category correction0104. It contains the real
`SweetSurface` object, `IOMemoryDescriptor`, table lock and counted table-user
references. It already exports a Mach send right with
`IOUserClient::copyPortNameForObjectInTask`; its selector34 resolves that right
with `IOUserClient::copyObjectForPortNameInTask` and checks the actual class.

`dre_sweet_surface_bridge.inc` uses that same task-IPC resolution and exact
private class in the same translation unit. It never guesses class offsets or
imports a global surface ID, a client pixel pointer, a host-memory extension,
cached frame or CPU pixel snapshot. The numeric ID in returned metadata is not
an authorization mechanism. The owning task must have the real send right.
The current right identifies the whole object; no independently verified
read-only GPU share-right subtype is claimed. Requested READ/WRITE is a bound
on a future GPU use, not an existing GPU grant or active CPU-access epoch.

The bridge retains the actual `SweetSurface`, its actual memory descriptor and
a balanced `sweet_surface_get/put` user count. The table hold prevents ID reuse
while an export pin exists. The bridge installer retains the actual root
service and cannot replace callbacks while pins exist.

## Geometry and real descriptor lifecycle

Before a pin is created, the source checks nonzero width/height up to16384,
up to20 planes, at most64MiB backing, exact descriptor length, and16KiB backing
alignment. It copies the selected object's real plane metadata under its lock.
64-bit arithmetic checks each plane's element dimensions, row size, extent and
allocation bounds. Overlapping planes or rows outside the64-byte proposed
import alignment receive explicit layout refusal. Only single-plane BGRA/RGBA
with4-byte elements is presently recognized for a possible future importer;
all other formats retain metadata but have a format refusal. Recognition is
not GPU format support or a successful import.

For actual `IOBufferMemoryDescriptor` backing, the bridge calls its real
`prepare(kIODirectionInOut)`, obtains each4KiB physical segment, compares it
with `kvtophys` of the real kernel alias, verifies complete16KiB physical
granules, and rejects duplicate pages using a bounded private inventory.
Neither physical addresses nor kernel pointers are returned to userspace.
Inventory does not prove FDT/GPU allocation ownership or coherent aliases.

A successful prepare has exactly the matching real `complete` on close.
Prepare failure creates no successful prepare to balance. A typed error after
pin creation can still return the pin's token for cleanup. A failed complete
keeps the descriptor, surface, page inventory, task and connection owner;
the orphan reaper retries rather than recycling the backing. Physical panel
descriptors are retained as physical metadata with an explicit physical-backing
refusal; they do not pretend to be prepared owned RAM.

## Append-only control ABI

ABI1 selectors0..12 and their layouts remain unchanged. New selectors are:

| Selector | Request | Result |
| --- | --- | --- |
|13 retain | caller-task Mach send right, READ plus optional WRITE | connection-scoped opaque token and copied backing metadata; partial errors can retain a token |
|14 describe | exact retained token | copied current backing metadata, flags and GPU-import refusal mask |
|15 close | exact retained token | actual descriptor completion and balanced release; failed completion retains ownership |
|16 import BO | exact retained token, plane and bounded access | Unsupported; every BO/VA/size/access/cache output remains zero |

New surface reply source layout is720 bytes, including20 fixed32-byte plane
records. These are authored ABI assertions, not compiled/validated results.
Each connection can retain at most16 pins; actual BOs plus surface pins share
the128MiB task budget. A new surface reserves64MiB before preparation, then
charges its validated actual backing on success. A failed pin remains charged
64MiB until real close. Repeated counters cannot wrap or alias a reused slot.

The existing DRE userclient requires the creating task on every call and the
exact connection generation. Tokens cannot authorize another task/connection.
Task death does not discard a failed-completion pin; the existing strong orphan
client owner remains until all actual resources close. No native provider
struct/layout, private diagnostic ticket or GPU capability changes here.

## Why GPU import refuses

The selected ordinary IOSurface allocator creates kernel-shared memory with
default cache mode. Its kernel alias and per-task mappings can already be live
and writable. It is not the separately allocated, verified Normal-NC native BO
path. Retagging a live WB page as NC would create incompatible aliases and is
not authorized by `prepare`, a physical-page list or a send right.

The returned refusal mask therefore always identifies live cache aliases,
missing GPU cache owner, missing task-VM import, missing shared-event contract
and unproven GPU allocation ownership. Additional physical/format/plane flags
make specific backing restrictions visible. The import selector always refuses
even if some metadata flags happen to pass. It cannot return a CPU snapshot as
a GPU texture, and it cannot enable Vulkan external-memory-host, IOSurface WSI,
raw PM4 submission, persistent-map capabilities or a Metal device.

Remaining implementation boundaries are concrete:

- Either allocate surfaces from a genuinely GPU-owned compatible backing at
  creation, before any alias exists, or implement a real all-task/kernel alias
  inventory, exclusion and cache-transition protocol. No retagging is provided.
- Qualify FDT RAM ownership, protected-page exclusion and full per-plane native
  mappings in an isolated task GPU VM; retain full backing/page references
  across submission, close, reset and actual TLB/cache retirement.
- Connect actual GPU cache barriers/completion and IOSurface CPU lock/access
  epochs without treating the selected seed/use counters as GPU fences.
- Implement authenticated cross-process resource rights and actual shared-event
  semantics. Selector12/36..42/45 in the selected CPU service stay Unsupported.
- Add explicit texture format/modifier/plane/stride qualification and protected
  command/descriptor validation before an actual importer can return a BO.
- Integrate the frontend's IOSurface texture path with that real importer;
  its reference host-pointer route remains unusable and must not be relabeled.

## Deferred integration and validation

The unrun `../vulkan/prepare_surface_bridge.py` recipe takes an already selected
DRE SweetSurface.cpp and writes a **new overlay**, preserving its original
bytes/hash. It adds a forward declaration and install call before root-service
publication, and includes this bridge fragment at the end of that same source.
The ordinary CPU service still starts if bridge installation fails. It also
copies `dre_surface_backing.cpp/.h`, this fragment and the UAPI header. The
future opt-in kernel source list must compile `dre_surface_backing.cpp`; the
fragment is not a separate translation unit. Root owns shared staging and must
apply/qualify these hooks separately. No recipe has been executed tonight.

Deferred checks must cover actual SDK/XNU signatures, full ABI sizes, ordinary
and multi-plane bounds, oversized/overlapping backing, unrelated/dead/foreign
Mach ports, exact token/generation/task isolation, partial prepare/inventory
failures, balanced table refs and descriptor completion, pin-budget exhaustion,
task-death orphan retention, and typed Unsupported imports with all GPU outputs
zero. A future real GPU importer additionally needs cache/VM/lifetime stress
and hardware render/readback tests before any capability can change.
