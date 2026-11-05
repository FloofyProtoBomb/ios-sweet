# Metal306 base construction evidence

Validation resumed after the overnight source freeze. The exact selected cache
collection completed privately, the combined frontend profile prepares, and the
real environment-profile fixture compiles and passes on Linux. Official SDK
compilation, actual class construction and private table/lifetime qualification
remain separate. Earlier tested objects/images remain unchanged. There is no
replacement Apple initializer, zero-filled table implementation or readiness
export here.

## Actual distinction in the existing private evidence

The selected20H392 Metal306.6.6 inventory identifies two different methods:

| Method | Unslid selected address | Existing evidence |
| --- | --- | --- |
| `_MTLDevice init` | `0x181a3634c` | Full95-instruction range collected privately |
| `initWithAcceleratorPort:` | `0x181aab968` | Captured abstract-method path, then nil |
| `initLimits` | `0x181a1b4bc` | Body collected; hardware-profile-derived table semantics unresolved |
| `initFeatureQueries` | `0x181a1bd38` | Calls family support and creates the real feature-query object |
| `initGPUFamilySupport` | `0x181a1ea28` | Body collected; selected feature-profile mapping unresolved |
| `_MTLDeviceFeatureQueries initWithDevice:` | `0x181a1c070` | Body collected; full consumer qualification unresolved |
| `.cxx_construct` | `0x181a47964` | Real three-instruction constructor initializes two once tokens |

The class extent is528 bytes with an8-byte NSObject superclass. Captured ivar
metadata describes a real limits aggregate, family pointer, queue objects,
compiler/library/pipeline builders, feature-query object, archive state,
architecture and once tokens. Captured deallocation consumes several of these
owned objects and private allocations. Their offset/encoding metadata does not
establish constructors, destructor invariants or valid table values.

The pinned public frontend's existing `objc_allocateClassPair` followed by
`[cls new]` uses normal inherited `init` dispatch. It does **not** call the
abstract accelerator initializer, nor does it prove the private base state is
valid for this subclass. Its empty `initLimits`/`initFeatureQueries` methods and
the separate new query-policy overlay cannot be counted as Apple private table
construction. A successfully allocated or zero-filled object is insufficient.

IOGPUMetalDevice's captured initializer is an alternative driver implementation
with real IOGPU creation/mapping/communication requirements. It is not a
constructor that can be borrowed by supplying fake AGX objects or zero selector
replies. The DRE custom backend must retain its own genuine transport.

## Concrete inherited-selector correction

The real base initializer creates queue and library/pipeline-builder objects,
calls `initSerializationPaths` dynamically on self, and initializes other owned
state. Blanket inherited-method refusal in the overnight policy incorrectly
replaced that helper and `.cxx_construct` with forwarding. That could break
allocation/construction before a usable device existed.

The policy now preserves these real inherited methods after an exact selected
base-image/method metadata guard. It still uses normal inherited `init`, never
the abstract accelerator initializer or an invented IOGPU object. The C++
constructor's actual once initialization is preserved, not copied as an ivar
write performed by this port.

The captured serialization helper has optional dump branches. Presence of
`MTL_DUMP_PIPELINES_TO_JSON_FILE` or `MTL_DUMP_LIBRARIES_TO_DIRECTORY`, including
an empty value, refuses this construction profile because those inherited
writer paths are unqualified. The real no-dump helper remains responsible for
its state. The environment checkpoint cannot serialize arbitrary concurrent
`setenv` in an application; it is not complete runtime/lifetime qualification.

## Typed observation adapter

`base_metadata.h/.mm` can inspect the already loaded exact Metal image without
loading a bundle or creating a device. It checks the actual plain-arm64 image
UUID, class/superclass extent, eight directly declared method signatures and original
same-image addresses. It returns bounded offsets/sizes/alignments for13 named
ivar metadata entries using the runtime's real type parser. It never reads an
object's ivar contents, writes those fields, invokes an IMP, initializes a table,
calls an abstract method, registers a class or publishes a service/device.

`METADATA_INSPECTED` means only that those runtime metadata observations matched;
it does not prove object construction, private feature tables, lifetime safety
or Metal support. No returned field is a GPU pointer, callable IMP or writable
object reference. Metadata may still race external swizzling/image mutation;
this is an inspection primitive, not an execution authorization. The opt-in
device policy compiles this source into its existing policy object and invokes
the metadata check before installing the policy. Do not additionally link a
standalone base_metadata.o into that same bundle. Four SDK objects remain the
profile's expected output count.

## Private collection and remaining evidence

`capture_missing.py` is a bounded private collection recipe. It requires the
exact cache UUID and unchanged existing
capture manifest, records the supplied official inspector's observed hash, and
writes only to a new directory outside this repository. The caller must use the
independently verified existing inspector; a computed hash alone does not verify
its origin. Fixed ranges use the selected inventory's following-symbol bounds.
Quiet disassembly avoids the known huge annotated stub-island allocation. No
automatic recursive code discovery, Apple header generation or captured-code execution
is involved. Partial collections never contain `collection.json` success output.

The collected direct BL destinations are observations for manual follow-up,
not a complete call graph: indirect calls, Objective-C sends, branches beyond
the symbol, exception cleanup and cold paths still require inspection. The
recipe never changes `base_initializer_semantics_qualified` or table qualification
to true merely because bytes were collected.

Before base construction can be qualified, establish:

1. The exact `init` control flow, superclass call, dynamic selectors, private
   allocations, stored ownership and every failure/exception cleanup path.
2. Which base fields it initializes and which require later subclass callbacks,
   including queue/once/compiler/library/archive/architecture dependencies.
3. The selected limits producer/consumer layout and units for every retained
   inherited consumer; querying a handful of public limits cannot fill a table.
4. Actual feature-query/family construction semantics without invented Apple
   family equivalence, zero tables or A740 waivers.
5. A consistent retained lifetime and failure disposal strategy for the custom
   subclass plus its real Rust/native backend, including inherited deallocation.
6. Official SDK compilation and bounded runtime construction/lifetime evidence
   on the exact image before device registration can be considered.

This slice adds no separate initializer entrypoint or permission to publish.
Even a qualified base construction will not supply the missing RAW command
admission, protected GPU VM, persistent cache-transfer/IOSurface contract or
actual A619 Metal compute/render execution.

The selected private `initLimits`, `initFeatureQueries`, `initGPUFamilySupport`
and `initWorkarounds` callbacks explicitly raise the typed unsupported-selector
exception. A previous draft checked the real facts object but returned from
these void methods without implementing their table postconditions. That was
insufficient: returning normally could let the original registration path use
uninitialized private tables. The callbacks now refuse rather than turn
queried Vulkan facts into a successful private initialization claim. Their
real table/ownership implementation and construction-failure cleanup remain
requirements before a functioning device can be returned.

## Read-only executable for later exact-image observation

`metadata_probe.mm` links the same metadata implementation against the official
iPhoneOS SDK's Foundation and Metal frameworks. It allocates no MTLDevice,
opens no accelerator/user client and submits no GPU work. Its bounded output
reports status, class extent and numeric method/member metadata, while keeping
device/private-table qualification explicitly false. A PID1 invocation remains
alive for at most30 ten-second observation intervals; ordinary invocation exits.

The separate iOS Rust workflow builds and ad-hoc signs this observer but never
executes it. A later isolated exact20H392 QEMU overlay can exercise the real
loaded metadata parser without modifying the frozen OS image or using a phone.
Successful compilation alone does not establish that its runtime guards pass.

The first isolated actual observer experiment reached the kernel exec log but
timed out without a status or heartbeat record. It established neither C main
entry nor a matching metadata result. A subsequent source version emits
`METAL306_OBSERVER_MAIN_ENTERED` using a raw POSIX console write before the
autorelease pool or inspector. That checkpoint separates main entry from a
later inspection stall if it is observed; absence remains inconclusive when
the console operation itself is unproven. No automatic VM retry is performed.
