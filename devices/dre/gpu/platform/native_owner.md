# Native owner and interrupt adapter draft

**UNVALIDATED source only.** No build, test, recipe, CI, emulator, phone action,
GPU/SCM operation or interrupt registration has run. No default broker or
controller is installed by these files.

`native_owner.cpp/.h` binds actual IOKit interrupt/resource objects to an
**externally acquired native platform lease**. It supplies real route
registration, retained controller/nub/specifier lifetimes, a work loop,
generation-aware hard-IRQ masking, deferred startup invalidation, bounded
MMIO forwarding and joined cleanup. It does not replace the power/startup
coordinator or create shared rail/reset policy.

## What the selected port actually provides

The inspected DRE kernel initializes GICv3 directly in
`pexpert/arm/pe_fiq.c`; `osfmk/arm64/sleh.c` handles timer/SGI delivery and sends
other Group1 INTIDs through the already installed per-CPU interrupt handler.
`SweetPlatformExpert.cpp` explicitly excludes the GIC from its nubs. The current
GPU ADT node has `qcom,irq-intid=209`, without an actual
`IOInterruptControllers`/`IOInterruptSpecifiers` route.

Those properties and a working controller cannot be invented from the INTID.
The adapter therefore requires a real externally leased IOService nub and
IOInterruptController whose existing dispatch path owns **SPI177 / INTID209,
level-high**. The nub's IOKit source index is supplied by that actual route and
is never guessed as177 or209. Missing controller/route/global dispatch evidence
refuses native acquisition. There is no new GIC-global handler, distributor
reset, IRQ affinity change or active-consumer takeover.

## Actual qualifications and resources

The adapter obtains the immutable original FDT from `dre_native_board_snapshot`
and independently checks the A619 GPU resources, inherited interrupt-parent,
unique GIC phandle, GICv3 cells and exact GICD/GICR/GPU/SMMU/GPUCC/GCC resource
ranges. This describes resources; it does not own them.

The native broker must perform an actual atomic shared-platform acquisition,
returning a lease and reset epoch, and must refuse inherited active DMA,
unknown aggregate APSS voltage/source/bandwidth policy or another GPU/reset
owner. Its resource-policy check must use actual held RPM ACK objects and
source/reset/parent supply ownership. The adapter calls the separate read-only
`profile.c` decoder and includes the actual verified supply/GDSC/regulator
graph with aggregate floors and allowed-level masks still zero. That decoder
cannot substitute for actual held policy. No default
aggregate floor or always-true provider is supplied.

The adapter retains the real nub/controller, requires its actual source cache
to have already resolved after real controller registration, captures and
rechecks its retained property arrays, source symbol/specifier objects and bytes, verifies
level type and rejects the shared-interrupt option. The broker must additionally
prove the actual controller rejects sharing atomically and that the vector is
unregistered before the real `registerInterrupt` call; after it, the real sole
handler target must be this owner. A failed or uncertain registration remains
owned until its exact controller state is proven; cleanup does not unregister
or mask a foreign handler.

The broker must inspect the nub's **actually resolved** cached controller and
installed dispatch target, not just its properties: old cached interrupt
sources cannot be safely rebound by a property edit. This adapter does not
force `resolveInterrupt` or replace a cached/global handler to make a route fit.
The selected platform's `lookUpInterruptController` waits indefinitely on an
absent name; it is never used as an existence probe here. Operations target
the actual captured controller directly. Actual generation-aware hardware
masking precedes the controller's soft-disable/handler join.

A local single-board ledger only prevents duplicate adapters. It is explicitly
not the external resource broker and grants no rails, clocks, DMA domain,
reset or interrupt ownership. Any partial acquisition keeps a resident owner
reference, including errors returned to the caller.

## Locking and interrupt behavior

Every normal provider/MMIO/coordinator operation uses the **same actual owner
mutex**. Broker `enter_owner`/`leave_owner` must operate that mutex, and
`owner_lock_held` must inspect the real held lock. A second advisory lock would
not serialize IRQ invalidation with submission and is not a valid provider.
Normal APIs are called with this lock held; only close runs outside it.

The primary handler immediately blocks new admission with an atomic fault,
records the first cause, and invokes only the broker's hard-IRQ-safe operation
to mask this still-owned route. That operation must check real route/lease
generations and leave a foreign vector untouched. No owner mutex, allocation,
sleep, policy acquisition or GPU/rail register access occurs in the handler.
The real controller owns GIC acknowledgement/EOI.

The private IOEventSource coalesces work into the real work loop. Its worker
enters the actual owner mutex and calls `dre_a619_startup_invalidate`, retaining
all resources. It does not speculatively read RBBM registers while power is
unknown, clear a GPU error, manufacture a fence completion or rearm a fault.
In the current polling/INT_MASK0 profile any route event is unexpected and is
treated as a fault. Explicit arming enables only the already qualified parent
route after actual startup; it does not program GPU interrupt masks.

Reset, source/clock and lease-loss notifications use the same invalidation
path. A kernel reset flag alone never authorizes freeing DMA resources or
silently reacquiring them.

## MMIO and coordinator binding

`bind_io` accepts the existing actually mapped Device callbacks from the native
binding and copies the descriptor. The returned owner IO object forwards only
aligned32/64-bit operations within the exact qualified GPU resource apertures.
Each access requires the same actual lease/reset epoch plus a broker **per
range** power/source/clock access qualification. GCC/GPUCC availability does not
make powered-off GPU core/wrapper access safe. The adapter never accesses GIC
MMIO through this path.

The basic owner guard is deliberately nonrecursive: it does not call startup,
power validation or MMIO that would recurse into itself. The broker's checks
must also remain nonrecursive. `dre_gpu_native_owner_guard`, `cold` and `policy`
convert the broker's status0 success to the exact boolean1 conventions used by
the existing coordinator. The broker interface itself uses status0, not truthy
negative proof returns.

The root native glue must bind this owner, the real allocator and IRQ worker
to that common lifetime mutex; no shared UAPI or staging file is modified here.
`native_owner.cpp` and `native_owner.h` are the only new compile/header inputs.

## Retirement

Close first requires actual external CP/firmware/DMA/domain/reset/consumer
retirement. It verifies whether the exact owned handler was installed, marks
admission closed and disables/joins the actual primary ISR/GIC ACTIVE while
holding the real board owner mutex. The primary handler never takes that
mutex. It then drops the mutex and disables pending work **inside the actual
work-loop gate** after any running callback. It never holds
the owner mutex while joining a worker that needs that mutex. Only then does
it recheck retirement, unregister this exact handler, release the actual route
lease and drop the platform's own references. Any failed proof leaves the
complete owner resident. There is no speculative reset or rail-off rollback.

The real shared resource broker, nonsharing IOKit GIC route/global dispatcher
binding, aggregate rail/source policy and physical native startup remain
unimplemented platform requirements. The adapter's real OS operations do not
turn those missing providers into GPU/Metal readiness.
