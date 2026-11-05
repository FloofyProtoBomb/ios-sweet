# IOSurface CPU mapping and lifetime audit

The current service is a real CPU-backed implementation, not just a registered
class. Patch 0046 allocates wired `IOBufferMemoryDescriptor` buffers and a shared
state page, maps both into each task, validates surface geometry, returns a
0xa68-byte result, and supports ID/Mach-port lookup. Its framebuffer path copies
CPU-visible pixels. It does not map these buffers into an A619 domain or provide
GPU cache ownership, fence synchronization, a Metal driver or presentation.

## Evidence from the selected iOS userspace

Read-only inspection used the actual iOS 16.7.16 (20H392) arm64 shared cache.
Apple binaries and disassembly stay in private local research storage. These
are ABI observations, not a claim that the phone has executed this path:

| Function | Unslid address | Observed interface |
| --- | --- | --- |
| `IOSurfaceClientIncrementUseCountForCategory` | `0x1a3986ad4` | Categories 0/1; trap 0 on a local category transition from zero |
| `IOSurfaceClientDecrementUseCountForCategory` | `0x1a3986c30` | Trap 1 when the local category reaches zero |
| `IOSurfaceClientGetUseCount` | `0x1a3986d84` | Returns the sum of the client's two local counters |
| `IOSurfaceClientIsInUseForCategory` | `0x1a3986dfc` | Reads packed 32-bit global categories at shared state +0x18 |
| `IOSurfaceClientLock` | `0x1a3981928` | Trap 2: ID, options, seed output |
| `IOSurfaceClientUnlock` | `0x1a3981ecc` | Trap 3: ID, options, seed output |
| `IOSurfaceClientRelease` | `0x1a39827bc` | Trap 4, userspace teardown, then trap 5 |
| `-[IOSurfaceSharedEvent init]` | `0x1a3988124` | Selectors 36 and 38; a failed method returns nil |
| `-[IOSurfaceSharedEvent initWithMachPort:]` | `0x1a3988234` | Selector 38 |

The normal release's two trap phases deserve further analysis, including failed
setup and reentrant lookups. The incremental fix deliberately preserves their
existing semantics; it does not claim that two-phase teardown is solved.

[Apple documents use counts and IOSurface functions](https://developer.apple.com/documentation/iosurface/iosurface-functions?language=objc).
The private category layout above comes from the selected binary, rather than
an assumption that SDK headers implement the runtime.

## Incremental kernel patch

`0104-dre-iosurface-lifetime.patch` targets `iokit/Kernel/arm/SweetSurface.cpp`
after the UI profile's 0046 and 0068 patches and DRE name substitution. Keep it
in this directory and have the staging pipeline copy it after 0103; it changes
no shared workflow or staging code by itself.

The patch fixes these concrete problems:

- Result construction receives a retained mapping reference on every path.
  Concurrent release/close can no longer free it before result data is copied.
- A racing lookup follows the same cached-client rule as the fast lookup:
  no extra count and base address -1 in the result.
- Closing a user client prevents new mappings before and after the unlocked
  allocation interval. Failed array insertion releases all temporary mappings.
- A failed result rolls back only its exact reference, preserving a replacement
  mapping another thread may have installed for the same surface ID.
- Traps 0/1 maintain each client's category ownership and atomically publish
  packed global counts at state +0x18. Close/release removes outstanding uses;
  category bounds and overflow/underflow are checked.
- BindAccel (12) and unimplemented event/accelerator operations (36–42,45)
  return `kIOReturnUnsupported`, with direct outputs cleared. Fresh inspection
  confirms that shared-event creation/mapping uses 36/38; the older notes
  grouping 36–38 as accelerator operations should not be relied upon.
  No native accelerator/event object exists to return. Existing CPU metadata
  selectors, including SetValue (9) and bulk attachments (27), retain their
  previous compatibility behavior; those ignored values remain unfinished.

The changed accelerator/event callers will now observe failure rather than a
zero-valued fake success. CoreVideo/Metal clients that require an accelerator or
shared event must handle that failure until actual providers exist. This patch
has not demonstrated that Setup.app or any other app is functional on the N200.
Lock/Unlock retain the current CPU seed behavior; no new GPU completion or cache
synchronization is claimed.

## Verification

Run `python3 devices/dre/gpu/test_iosurface.py --out /tmp/dre-surface-test` in a
Linux/macOS environment with git, a C++17 compiler, pthreads and ASan/UBSan.
The runner reconstructs the tracked 0046+0068 source, applies the actual new
patch, and extracts the affected production method bodies into instrumented
IOKit boundary mocks. It first reproduces the baseline missing-retain failure.
The fixed source passes 13 scenarios and 8,000 concurrent lookup/release calls,
including forced mapping races, close during allocation/result copy, exact-ref
rollback, category isolation/cleanup, allocation failure, unsupported selectors
and CPU seed behavior. These are host runtime/reference tests, not GPU tests.

The complete patched `SweetSurface.cpp` also compiles to an ARM64 Mach-O kernel
object using the recorded XNU build flags and generated headers, in an isolated
local build tree. A full kernel CI build and physical CPU-surface exercise are
still required after staging the incremental patch.
