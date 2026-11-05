# Persistent mapping/access ownership — UNVALIDATED

These are source drafts. No tests, compiles, preparation/build recipes, CI,
QEMU or phone operations have run. **The persistent capability remains
unconditionally absent.** RAW remains a separate missing capability; no map,
epoch transition or structural PM4 parse can grant it.

The existing exclusive CPUMap/CPUUnmap mode still advertises contract1. This
source adds a separate pointer-lifetime/ownership protocol, private transition
engine and real IOKit entrypoint wiring behind the withheld capability. It
does not yet provide a usable coherent persistent Vulkan mode.

## Lifetime and ownership are separate

A persistent IOMemoryMap lease owns an actual task address, actual mapping
object and ordinary Normal-NC BO aliases. Keeping that pointer alive does not
mean the CPU owns access indefinitely. `dre_gpu_access_epoch.c` tracks the
distinct ownership states below under the BO/VM/native owner lock:

| Operation | Required state | Actual provider work | Result |
| --- | --- | --- | --- |
| Attach | Fresh BO or detached idle BO | Prove actual kernel/task NC aliases, mapping and identities | Mapped, idle, no CPU epoch |
| CPU begin | Idle, no GPU ticket | Real FROM_GPU cache/ownership operation, `cpu_begin`, DSB and ownership recheck | Unique CPU access epoch |
| CPU end | Exact active CPU epoch | Real TO_GPU operation, `cpu_end`, DSB and ownership recheck | Pointer remains mapped, access idle |
| GPU prepare | Idle, CPU epoch released | Real TO_GPU operation, DSB and ownership recheck | Unique prepared GPU epoch |
| GPU publish | Exact prepared epoch | Record actual native published ticket before rechecking ownership | Ticket/resources retained |
| GPU retire | Exact published ticket/epoch | Actual native query proves terminal resources-retired result, FROM_GPU operation and DSB | Access idle; workload error stays an error |
| Cancel unpublished | Exact prepared epoch, actual native published0/ticket0 | Real ownership/cache restoration | Idle only after success |
| Detach | Idle or published GPU epoch, no active CPU access | Actual IOMemoryMap unmap/release | Pointer revoked; GPU ticket/pins stay retained |

Every cache/CPU/provider callback is mandatory. Missing callbacks return
unsupported. Status success is accepted only after the actual operation;
Normal-NC does not turn GPU UCHE/cache/retirement work into a successful no-op.
Proof callbacks require exactly1, excluding negative error values. The code
records an epoch before ownership transfer and retains ticket/epoch metadata
if partial operation, alias verification, barrier or ownership fails. Unsafe
state is quarantined; the engine cannot erase it, speculate a reset, release
pages or free/reuse an address.

The kernel wrapper uses `verifyMemory` and the actual task pmap/IOMemoryMap
proof for both kernel and task aliases, exact BO/session/GPU-map identities,
native reset generation, real existing `native.cache`/`cpu_begin`/`cpu_end`,
and DSB followed by renewed native/physical ownership checks. CPU addresses
come from `createMappingInTask`; no caller virtual/physical alias is trusted.
The original descriptor allocation and both aliases stay Normal-NC, never
Device or a guessed cacheable/coherent mapping. A later descriptor/tag cannot
substitute for the actual proof.

The mapping persists when a CPU epoch ends. Ending the map can revoke only its
CPU pointer while a GPU-published epoch and BO/job pins remain held. A detached
idle BO can receive a new mapping lease without resetting its monotonically
allocated ownership epochs. Mode mixing into the old exclusive API is refused.
An uncertain map/ownership path retains the underlying allocation/session.

## Append-only ABI1 extension

Existing structures and selectors0..9 are unchanged; provider version1 and its
struct layout are unchanged. Three opt-in selectors are appended:

- **10 `CPU_MAP_PERSISTENT`**: existing CPUMap request/reply layouts, contract2.
  Successful reply grants pointer lifetime, not CPU access.
- **11 `CPU_ACCESS_BEGIN`**: new56-byte request with BO/map lease, epoch0 and CPU
  access; new32-byte reply returns the actual unique ownership epoch.
- **12 `CPU_ACCESS_END`**: same request with exact epoch and access0; ordinary
  status reply. Only a successful real transfer clears that epoch.

All reserved fields remain zero and the full request is copied before use.
Older clients and old exclusive semantics remain intact. Older kernels cannot
advertise the required persistent contract; new userspace checks the capability
before these selectors. `dre_darwin.c` tracks actual map leases separately from
CPU epochs, exposes explicit acquire/release, retains partial-error epochs,
rejects GPU submission while an acquired CPU epoch is live, and performs an
actual end before revocation. It does not report successful generic partial
cache operations on a persistent mapping. An existing owned epoch can be
retained without pretending that a new cache operation occurred.

## Why support is still withheld

The current exclusive bootstrap-sharing VM does not establish untrusted task
protection. It forcibly withholds persistent and RAW capabilities. No native
provider has yet qualified real GPU cache completion, persistent alias
ownership, command immutability or hardware completion on this port.

The private GPU prepare/publish/retire/cancel transitions are implemented as
source, but **are not connected to the raw submit/refresh path**. Current
submission still refuses mapped resources. That refusal remains intentionally
in place while the full native job integration is unqualified. Before it can
change, each resource's GPU epoch must be acquired before publication, its
ticket preserved on published failure, and the epoch retired by actual query
and cache completion before releasing the existing full-resource pins. A
prepared partial failure needs proven cancellation or retained quarantine.

The Vulkan/Metal caller integration is also incomplete: Mesa retains
`bo->map` pointers and can bypass KMD map hooks; application contents access
does not itself invoke a driver callback. Explicit access epochs must be
integrated with real Vulkan/Metal synchronization and immutable command copies,
including waits, uploads, CPU reads, device loss, task death and memory pressure.
The new selectors do not intercept ordinary CPU loads/stores. An application's
out-of-epoch data race is not prevented by a persistent mapping and cannot be
used to justify accepting mutable command/state descriptors.

Full resource pins, native normal-RAM ownership, immutable command snapshots,
protected GPU task VM, real GPU cache operations, hardware error/completion,
and actual TLB/BO retirement remain mandatory. Normal-NC aliases alone do not
prove the whole contract, and this draft enables no Metal or Vulkan device.
