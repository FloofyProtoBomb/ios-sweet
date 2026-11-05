# Opt-in native source staging draft

**UNVALIDATED source recipe.** Written overnight at the user's request without
running staging, patch application, compilation, tests, CI, QEMU or phone
operations. Existing prepared images are unchanged. This is not a ready image.

The existing `bringup` and `ui` selections retain their intended behavior.
Native source inclusion requires explicit flags in a future fresh build tree:

```sh
python3 devices/dre/stage_xnu.py /private/new-patches --profile ui \
    --native-draft gpu --native-draft touch
```

The separate `--native-gpu-nc-proof` selection copies the authored pmap
observation include into the actual XNU8796 pmap translation unit and defines
its corresponding binding macro. It does not write PTE/cache/TLB state or
assume an alias has the requested cache mode. The private helper must compile
against the actual selected kernel and prove real per-page Normal-NC aliases;
unsupported page/PPL/physical-aperture layouts refuse. That integration is
also unexecuted and unvalidated, and is never selected by default.

`--native-gpu-firmware-draft` explicitly adds the private firmware loader's
public source, without any private initializer or firmware bytes. Its
`--native-gpu-fw-nc-proof` companion includes the separate low-DDR pmap
observation helper. Without a privately compiled trusted anchor the loader
refuses; this source recipe does not provision one or copy private inputs.
The boot-image package recipe is a separate opt-in operation after validation
resumes. Kernel firmware source file names are prefixed in the generated tree
to avoid generic `native`, `region` or `package` object-name collisions.

That firmware-source selection also includes the actual boot callback binding
and native provider-v2 lifecycle composition. Their required real platform
policies are not supplied by staging. The known GPU8KiB/IPA shared16KiB granule
still refuses before RPM/rail/clock/CP stores. The provider needs the genuinely
linked managed NC pmap proof (or it refuses its first allocation); source flags
are not alias proofs. No recipe constructs/publishes a provider automatically.

Do not execute this recipe during the overnight no-validation window. Its
generation, source edits, compiler assumptions and resulting linkage need
validation when the user resumes it.

The new helper uses strict source allowlists rather than recursively copying
the working directory. Firmware, private FDT/phone identities, SDK material,
offline shaders, test fixtures, host compiler code and runtime binaries are
not staging inputs. Source references remain under
`iokit/Kernel/arm/dre/{gpu,touch_native,touch}`. It copies only libfdt headers
and the DMA-range header beside them; the already staged storage sources
provide the shared function implementations, avoiding duplicate symbols.
The common `native_board` source obtains a private boot-held original FDT
without initializing UFS or a peripheral. It reuses the existing checked
ownership handoff and removable byte-copy mapping pattern. Nothing exports
the raw FDT or its private properties to userspace.

`0105-dre-native-draft-assets.patch` adds the selected translation units and
headers. `0106-dre-native-draft-bindings.sh` appends explicit IOKit build-list
entries after the existing UI patches. For touch, it adds the public batched
frame declaration and source include to the selected SweetHID class. Unknown
or already modified class layouts refuse the source edit. The C source names
for touch are prefixed in the generated tree to avoid generic object-name
collisions; relative header structure is preserved.

Both explicit native selections now include the common actual per-SPI GIC
source and authored exact XNU8796 hooks. A future staging invocation obtains
the IRQ helper's standalone program text; only the later explicitly executed
binding script edits selected PE/SLEH/IOService/namespace sources. Neither
operation has run overnight. The script preserves the existing IAR/sole EOI,
timer/SGI/PPI and foreign-handler paths; only genuinely reserved per-SPI slots
can consume their own OTHER-branch delivery. The namespace/cache bridge uses
actual private IOKit allocations and atomic dictionary ownership, rather than
an unbounded missing-name resolver or fabricated interrupt metadata.

Linking these sources installs no default broker or controller. Actual global,
Group1NS, per-vector/producer/CPU and pin/summary ownership still qualifies
publication. The GPU kernel-private provider is version2 and requires binding
to the service's actual mutex; public selector ABI remains1. New source paths
and their selected-source edits need real compilation and lifetime/IRQ checks
after the user resumes validation.

The opt-in path records the exact input source and generated recipe digests
in the patch manifest, marked `UNVALIDATED_SOURCE_RECIPE`. That manifest says
the preparation helper did not execute the generated recipe or compile its
sources. It is not a hardware-readiness, runtime or driver qualification.

No recipe installs a default native provider, automatically calls publication,
sets native boot arguments, publishes `IOAcceleratorES` or enables Vulkan/Metal
capabilities. The real GPU and touch bindings still require their acquired
policy, clock, interrupt, DMA and lifetime providers. Building a source file
does not satisfy those dependencies.

The GPU source selection includes the draft startup coordinator, existing
power/RBBM/SMMU/CP components, native IOKit transport and bounded VM adapter.
It also links the authored access-epoch state machine and narrow diagnostic
PM4 intake source. Neither linking that parser nor the new mapping ABI grants
RAW submission, persistent mapping or a complete command-validation claim.
The bounded private A619 cache-event planner is also linked as source only;
it executes nothing and supplies no renderer state/retirement proof. Named
GCC parent enable/hold source requires actual shared-source/interconnect policy
and register serialization, preserves other bits and performs no PLL/RCG/GDSC
configuration. No successful default callback substitutes for that authority.
Separate private task-VM/AP proposal source is included with prefixed C object
names. It requires an authoritative complete purpose/task-generation registry
and is deliberately non-installable: no PTE/context change, task-isolation
claim or RAW capability follows from linking it. Actual incoming transaction
privilege and unforgeable completion/retirement remain unqualified.
The same explicit selection now includes the kernel allocation-catalog helpers
and its synchronous private registry-inspection adapter. The private allocator
has its own version1/exact-size contract: constructors classify verified whole
allocations once and record only genuine successful VM mapping handles. Actual
task roles come from the registered client/task/connection, never user labels.
Catalog, workspace and output require genuine no-GPU CATALOG_SCRATCH storage;
unknown or foreign allocations remain recorded and counted. The catalog's
bootstrap window remains shared. Observations expire as authority when their
owner-lock-held visit ends and contain no PTEs or physical/object pointers.
There is deliberately no planner call in this adapter: actual data-only scope
returns TASKVM_REGISTRY_DATA_OBSERVED_PLANNER_REFUSED. Linking does not invoke it,
install PTEs or create task isolation. Its source preparation,
versioned interfaces and lifetime paths have not been executed or compiled.
Descriptor data and full allocation padding are inventoried. Existing heap
metadata's surrounding CPU granules are not qualified as isolated allocator
data; an explicit data-only coverage record cannot satisfy a stronger complete
private physical-inventory requirement. The adapter must refuse that boundary
until real metadata backing/alias authority exists, rather than asserting it
from an allocation-purpose tag or retaining a heap pointer alone.
Actual boot-memory source prepares real allocator-owned NC tables/bookkeeping
and CP buffers through the existing VM manager. The separate private trusted
queue links only fixed kernel-authored IB/cache-event/fence builders; it cannot
accept user command streams, shader code or BO addresses. Its actual mappings
must be staged before startup and the exclusive-task bootstrap snapshot.
CPU-only table preparation carries only actual reservation36 plus fresh NC
proof44. A real post-power domain qualifier is mandatory before activation;
future powered/global/quiescent bits cannot be prefilled in configure.

The GPU selection also includes the retained surface backing component and
narrow hooks in the actual selected SweetSurface translation unit after the
existing lifetime patches. The hooks use its real private classes/table/locks,
not copied offsets. Existing source layouts are checked before any source
replacement is attempted. This provides task Mach-right resolution and actual
descriptor retention/inventory only: live WB aliases, GPU cache ownership,
task-VM import and shared events are explicit blockers. Surface import keeps
returning unsupported with zero BO/VA; no Metal capability is enabled.

The touch selection includes the physical SPI/GPI/Novatek/input components,
existing portable firmware/report/descriptor helpers, retained XNU service
adapter and SweetHID frame bridge. It now includes actual apps-SMMU SID0x16
tables, QUP static/DFS/Fabia clock-readback decoding, and the actual GPIO22
awake TLMM controller backed by the common leased GIC259 route. The adapter
and controller use the same real pin engine/lock. Its private provider
ABI is v3; actual held-reference/observed-clock policy and broker ownership
remain required. The IRQ route component captures/retains an actual registered
controller, nub, specifier and interrupt cache; it supplies no new controller
or summary route. The selected platform currently lacks the required native
GIC/TLMM IOKit route in existing images. The new controller/hook source is
unexecuted and requires real policy, so source inclusion cannot prove delivery.
The GPU power-graph decoder likewise observes source identities/variants only;
it leaves aggregate floors and allowed-level masks zero for actual policy.
The actual native-owner and RPM Device-mapping sources are included explicitly.
Both still require a genuinely implemented external broker. RPM mapping never
grants access to the MPM register window inside the advertised shared RAM, or
to rounded CPU-page neighbors. Source inclusion negotiates no firmware channel
and requests no rail level. The owner requires an already-resolved actual
controller/nub cache and does not name-probe a missing controller.
The touch source also retains accepted-contact cancellation across observed
controller/transport/generation/source loss. Its actual XNU IRQ-fault path
posts the existing owned worker and requests serialized cancellation rather
than bypassing input. Failed release delivery can retry without more report
reads or IRQ rearm. This is lifetime source, not a native touch acceptance
result or a successful hardware recovery.
Missing expected source files are an error,
never an implicit feature downgrade.

Tomorrow's sequence is to inspect/generate the selected source tree, resolve
source/API and kernel-link problems, run meaningful protocol/lifetime/failure
checks, and only then prepare a specific hardware diagnostic for separate
authorization. Native graphics/input acceptance and a usable iOS installation
remain unproven.
