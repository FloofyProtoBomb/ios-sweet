# Selected306 device answers — UNVALIDATED source overlay

No new source, overlay, recipe, SDK/Rust compile, link, CI, emulator or device
operation has run. Earlier tested SDK objects and the host Rust archive remain
immutable. This is not a working/published MTLDevice or a feature qualification
report. Native RAW, persistent ownership/cache, IOSurface and Metal remain absent.

## Concrete differences from the pinned reference

The selected frontend previously installed every private runtime selector it
could answer, and silently inherited the remaining `_MTLDevice` methods. Empty
registration methods were described as leaving neutral0/NO, but that is not
evidence about a private base method reading Apple tables. The matching runtime
also has explicit private values such as shared-texture-handles=true, fixed
linear/IOSurface alignment16 and an effectively unlimited predication-depth
answer. Those do not establish the selected native DRE resource/event contract.

Other direct frontend methods returned whole CPU RAM for sharedMemorySize,
RAM/4 for recommended working set, hasUnifiedMemory=YES without checking the
selected Vulkan device, and generic linear-image alignment for IOSurface.
Legacy feature sets were inferred from Apple family numbers. This overlay
replaces or refuses those paths rather than qualifying A740 assumptions.

## Actual queried facts

The new matching Rust export in `dre_device_facts.rs.inc` reads the selected
runtime's real `DeviceProperties` and enabled `DeviceDescription`. It requires
Integrated, vendor0x5143, chip0x06010900 and the device's actual unified-memory
property. A CPU, virtual, discrete or differently identified provider is refused.
It returns a versioned72-byte facts structure containing:

- the real runtime maximum buffer length and Vulkan maximum allocation;
- runtime threadgroup-memory length bounded by actual Vulkan shared memory;
- actual Vulkan workgroup invocation limit and runtime axes bounded by Vulkan
  axis/invocation limits;
- runtime attachment capacity bounded by actual Vulkan color/fragment outputs.

No queried device exists today because required native capabilities are absent.
The new export/ABI has not been compiled or called. These are authored source
data paths, not measured values. Android's vendor Vulkan implementation supplies
none of these answers. Rust/C++ structure assertions are deferred compile checks.

`device_policy.mm` installs typed getters from those facts. The working-set
recommendation is conservatively one actual maximum buffer/allocation; it is
explicitly a port policy, not total GPU/shared RAM. Dedicated memory0 follows
the verified selected integrated/unified model. Shared memory size, low-power
classification, headless classification and unknown alignments are refused.
gpuCoreCount remains unsupported; no A740 value8 or guessed SP equivalence returns.

Curated flag queries read actual enabled runtime Vulkan features/formats or typed
RuntimeGap results:32-bit float filtering, raster-order interlock, depth24/stencil,
programmable sample positions, sampler reduction, texture sample count and
primitive type. Runtime refusal raises the unsupported-selector path, not a
fabricated0/NO. Private methods are installed only when the selected real class
has the exact return/argument encoding. No newer absent SPI signature is invented.

## Preventing inherited feature claims

The new source no longer uses the generic runtime answer/family installer.
Before registering the dynamic device class, it records the actual selected
base methods and overrides unimplemented device methods with the real ARM64
Objective-C forwarding entry using their original exact type encodings.
NSObject identity/introspection/lifetime methods and basic initialization are
preserved. Explicitly implemented frontend methods remain separate from inherited
defaults; unsupported own no-copy/IOSurface/shared-event methods are also forwarded.

`respondsToSelector:` returns NO for that refused set. A caller ignoring it gets
the existing `doesNotRecognizeSelector:` failure, not a generic object signature,
zero limit, nil-shaped successful object or silent no-op. The selected profile is
plain ARM64 only; missing forwarding, incompatible method types or unavailable
real facts refuse class construction. Runtime/class-association and structure-return
forwarding behavior still require actual SDK/runtime qualification.

supportsFamily and supportsFeatureSet answer NO for every family/set: no Apple
or common-family equivalence is claimed. Missing shared texture handles, address
ranges, backing detach, XR10/12-bit YCbCr and memoryless provider qualification
answer false only where the selected class has a verified BOOL/no-argument ABI.
They are withheld, not asserted impossible in future hardware work.

Registration entrypoints verify that the real facts/refusal state exists. They
do not initialize unknown private base tables. The inherited `_MTLDevice` object
initializer, direct private-ivar consumers and complete method coverage remain
unqualified. A caller using an unimplemented API fails; this partial policy does
not promise that SpringBoard, Core Image or Setup can use a Metal device yet.

## Separate source and matching build integration

The `connect_device_policy.py` overlay modifies only a new prepared
frontend/bridge copy. It removes the generic table/family waiver installer,
connects actual typed device facts, blocks misleading memory defaults and
adds the Rust facts include. It uses the source already connected to the
selected306 compiler and preserves previous tested artifacts.

The future explicit build profile is `--dre-compiler-service
--dre-device-policy`. It requires four real SDK translation units, including
compiler_service.o and device_policy.o. Imported bridge symbols from **all**
objects are checked against the matching Rust source. The matching Rust source
recipe copies the exact facts include; its build identity now includes adapted
bridge source hashes and this policy, preventing reuse of an earlier runtime
identity for different bridge behavior. Package inputs must have matching policy,
facts-source, object and Rust correspondence; old three/two-object builds cannot
silently qualify it. Combined source preparation now passes. A first actual
host Rust compile exposed five C-style integer-negation mistakes in the facts
overlay; they were corrected to Rust zero comparisons. Recompilation and true
iOS/SDK results must be read from their corresponding new reports.

The policy also checks the selected base metadata and preserves the real
`.cxx_construct` and `initSerializationPaths` required by the captured base
construction path. The latter's optional dump environment branches refuse.
See `base_init/README.md`; this correction is not full private-table qualification.

Deferred work includes exact SDK signatures, real queried values, typed failed
queries, method filtering/forwarding (including MTLSize), association/lifetime,
registration and class-construction refusal tests, true iOS Rust/static link,
every actual consumer's feature/API needs and physical render/compute/readback.
It also needs a real compatible IOSurface importer and event/cache/VM ownership;
CPU snapshots/host pointers are explicitly not that implementation. No capability
or kernel ABI extension is introduced by this policy.
