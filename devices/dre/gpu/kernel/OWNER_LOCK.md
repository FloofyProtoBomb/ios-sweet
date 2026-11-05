# Actual native GPU owner mutex binding

**UNVALIDATED source only.** No build, compiler, test, source recipe, interrupt,
hardware or phone action has run for this change.

The earlier native owner contract correctly required one common mutex but
could not receive the IOLock actually used by `DREAdrenoGPU`. A second external
advisory lock would not serialize its timer, user clients or fence waits with
native fault invalidation. Kernel-private provider version2 now appends the
mandatory `bind_owner_lock` callback; public selector ABI1 and layouts remain
unchanged.

Before acquisition, the binding passes `dre_gpu_owner_lock` callbacks for its
actual IOLock while that same lock is held. All driver client, timer, close and
fence-wait paths enter/leave the real lock through these wrappers. The current
thread record is updated only within real lock/unlock boundaries. Task-context
`check_held` compares the actual current thread, rejects interrupt context and
calls the selected XNU8796 `lck_mtx_assert` on `IOLockGetMachLock`. It does not
use a separate mutex or turn a caller-provided flag into lock ownership.

The native binder must copy the descriptor and connect its broker, worker,
SCM/RPM, VM/cache and queue operations to these exact callbacks. `check_held`
uses status0 for success, matching the native broker contract; it is different
from lease/cache alias proof predicates that require exact boolean1. Enter is
sleepable and nonrecursive, so an already-held owner must not enter again.
Hard IRQ code continues to use its separate fast lease/mask operations and
never takes this mutex. A fence wait releases the actual lock before sleeping
and reacquires it before re-resolving opaque resources.

Binding grants serialization only. It cannot acquire hardware resources,
qualify inherited rail policy, create an interrupt route, complete a fence or
enable a Vulkan/Metal capability. Missing binding prevents acquire/configure.

Set `retained=1` before any lock cookie/reference escapes, including partial
failure. A successful binding is intentionally held for the boot lifetime.
Once a native worker can reference the real service lock, ordinary service
failure cannot free it. A failure with `retained=0` promises no retained object
or future callback exists. Unexpected lifetime replies retain the owner rather
than free an uncertain callback target. No unload/rebind shortcut is provided.
