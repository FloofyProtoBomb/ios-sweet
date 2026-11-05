# iOS 16.7 Metal runtime integration

No Metal device or native GPU acceleration is implemented here. This directory
qualifies the real iOS device factory and prepares a reusable frontend reference.
The read-only observer cannot open a GPU user client, map memory or submit work.

## Exact selected ABI

Read-only inspection used the legitimately obtained 20H392 plain-arm64 shared
cache. `inspect_abi.py` pins its UUID plus the Metal and IOGPU image UUIDs before
using fixed addresses. It checks actual call instructions, Objective-C references
and decoded class metadata. Apple binaries, disassembly and captures stay outside
the repository. The report explicitly keeps hardware and acceleration proofs false.

| Item | Observed value |
| --- | --- |
| Metal version / UUID | 306.6.6 / `0F0B7B96-90BF-3A07-B7E1-85E2FB6160E6` |
| IOGPU UUID | `8EB7ED27-E1AF-3C82-825B-130C091D7070` |
| Hardware enumeration | `IOServiceMatching("IOAcceleratorES")` |
| Bundle property | `MetalPluginName`, CFString |
| Class property | `MetalPluginClassName`, optional CFString |
| Bundle path | `/System/Library/Extensions/<MetalPluginName>.bundle` |
| Class resolution | `classNamed:`, falling back to `principalClass` when absent or unresolved |
| Required ancestry | `isSubclassOfClass:[_MTLDevice class]` |
| Device construction | `alloc`, `initWithAcceleratorPort:` |
| Registration | Conformance to `MTLDevice` and `MTLDeviceSPI`; `initLimits`, `initFeatureQueries` |
| `_MTLDevice` class extent | instance start 8, instance size **528 bytes** |

The selected class resolver is at `0x181a3c7c8`, enumeration at `0x181b144c8`
and `MTLAddDevice` at `0x181a1b368`. The base implementation of
`initWithAcceleratorPort:` at `0x181aab968` invokes an abstract-method path and
returns nil. Naming a service or inheriting the base class is insufficient.
`MTLCreateSystemDefaultDevice` returns a registered device; its once block is
not itself the hardware enumeration routine.

This provides a genuine custom bundle path without replacing
`MTLCreateSystemDefaultDevice` or pretending the A619 is an AGX GPU. A companion
kernel service must implement real ancestry, ownership and transport, and publish
the device only when the frontend and native backend can actually execute work.

## IOGPU is a substantial alternative ABI

`IOGPUMetalDevice` inherits `_MTLDevice`. Its initializer passes the API string
`Metal` to `IOGPUDeviceCreateWithAPIProperty` at `0x1ece1ce38`. That routine uses:

| Operation | Input | Output | Observed handling |
| --- | --- | --- | --- |
| `IOServiceOpen` | type `1 \| ((options & 0xffff) << 16)` | connection | Failure rejects device |
| Struct selector 6 | 16-byte API string buffer | none | Original caller does not check return |
| Struct selector 2 | none | `0x218` bytes | Success required; contains shared state addresses/offsets |
| Struct selector 0 | none | `0x40` bytes | Success required; device configuration |
| Struct selector 4 | none | `0x20` bytes | Additional communication data; failure tolerated here |

This is only the beginning of IOGPU's ABI. Correctly sized zero-filled replies
would not implement it. Resources, task mappings, command queues, completion,
faults and shared objects all require real implementations. A custom frontend
that manages its own backend can avoid depending on these Apple IOGPU objects.

## Reusing an existing real frontend

[UniXNU AdrenoMetal](https://github.com/UniXNU/AdrenoMetal/tree/e82788dd367753802ec2e00476ab27addfb46834)
contains an MIT Objective-C frontend plus a Rust C bridge to metal2vulkan and
Turnip. Its upstream qualification is A740/SM8550 with a different OS ABI.
The [primary porting procedure](https://unixnu.com/porting/gpu) describes the
memory, submission, compiler and presentation dependencies. It is useful source,
and requires adaptation for this A619 port.

`upstream.json` pins the exact reviewed source hashes. `prepare_reference.py`
requires the actual qualified ABI capture and that source revision. It copies
only frontend/bridge reference sources and their MIT license, changing the class
extent guard from 536 to 528, defaults MSL to 3.0, makes unknown selectors fail
and uses the bounded compiler envelope helpers below. It produces no bundle, executable, publish marker
or launch job. It excludes the A740 publication wrapper and its renderer patches.
Its report refuses publication for concrete remaining reasons:

- The real A619 BO/address-space/queue/fence and Darwin Vulkan backend are absent.
  Offline shader compilation and Android's Vulkan driver do not establish them.
- The A740 `gpuCoreCount=8` method is removed. Mesa's static A619 SP count is
  one; its equivalence to Apple's query is still unproven, so a required direct
  call fails as unsupported.
- Forced Apple/common GPU-family answers and caller-dependent family overrides
  are removed. The bridge now uses only the runtime's strict family predicate.
  Private methods, inherited defaults and the runtime's predicates still need
  an iOS 16.7 audit. A feature table must reflect supported operations.
- The Rust bridge requires its matching translator and Turnip patch set.
- IOSurface import, ownership, cache synchronization and cross-process events
  must work for system clients. In-process event support does not prove those.
- No adapted frontend SDK build, real factory run or native Metal workload has
  passed. The existing end-to-end `metal_probe.m` remains the acceptance test.

The upstream unknown-selector fallback fabricates an object-return signature
and returns zero/no-op. The adapted reference returns no method signature and
raises `doesNotRecognizeSelector:` if directly forwarded. This exposes a required
missing operation instead of allowing it to appear implemented. Explicit upstream
feature answers and private methods still need their own audit.

## Local source compiler envelope

The same cache's MTLCompiler 306.6.6 UUID is
`0ACFC872-8027-3DFD-8F30-397C9CB25639`. Its `MTLCodeGenServiceBuildRequest`
export is at `0x1bbdaacd0`; the producer and archive serializer were inspected
independently. `inspect_abi.py` now qualifies their relevant instructions too.

Request 13 has a 16-byte header of four little-endian 32-bit words: source length
including NUL, inline preprocessor-metadata bytes, argument length including NUL,
and flags. Source starts at 16 and is followed by padding to four-byte alignment,
inline metadata, then NUL-terminated arguments. The producer caps its default
language at `0x30000` (3.0). The upstream default 3.1 differs. Its two uint64 length
writes happen to encode the no-inline-metadata/flags=0 subset when the lengths
fit 32 bits; they are not by themselves an incompatible layout.

`compiler_request.c` implements that bounded no-metadata subset, refusing NULs,
overlap, oversized lengths and insufficient capacity. The request encoder does
not invoke a compiler. The archive reply's header is 104 bytes: warning offset
and size are at 8/12, archive offset and size at 40/44, optional debug offset and
size at 48/52. The parser checks those ranges and MTLB magic, distinguishes the
archive type from legacy function-table replies, and returns only the archive
span. It does not mistake trailing debug data for metallib bytes. Envelope
validation is not library semantic validity or native shader execution.

## Tools and verification

Run from Linux/macOS with the verified `ipsw` tool and local cache. Capture files
include Apple disassembly and must remain in private storage:

```sh
python3 devices/dre/gpu/runtime/inspect_abi.py \
  --cache /private/dyld_shared_cache_arm64 --ipsw /tools/ipsw \
  --capture /private/metal306-capture --out /reports/metal306-runtime-abi.json
python3 devices/dre/gpu/runtime/prepare_reference.py \
  --source /sources/AdrenoMetal --capture /private/metal306-capture \
  --out /private/metal306-reference
python3 devices/dre/gpu/runtime/test_runtime.py --zig /tools/zig --out /tmp/dre-runtime-tests
```

The actual 20H392 capture passes the inspector. The tests pass 11 malformed ABI,
branch-decoding and metadata guards, 16 synthetic discovery/ownership/refusal
cases and three source-guard checks. The complete C observer cross-compiles to
a plain ARM64 Mach-O object with iOS 16.0 load metadata using Zig's Darwin C
headers. The discovery core also passes GCC ASan/UBSan. Neither is a device run.
For an actual SDK-linked observer, use `build_factory_probe.sh` on a macOS Xcode
runner. It builds only a read-only executable and never installs or starts it.

`factory_probe.c` enumerates at most 16 matching services, reads both properties
with CF type checks, and releases all objects on success and failure. Optional
class absence or wrong type is reported so the caller knows the original factory
would use its principal-class fallback. Non-ASCII, embedded-NUL and overlong
identifiers are reported as unrepresentable; no path is loaded from them. Status
0 means metadata was read, 1 means no accelerator, 2–5 are observer failures.
The JSON always says `metal_acceleration_pass:false`.

## Official SDK compile entry point

On a macOS Xcode runner:

```sh
python3 devices/dre/gpu/runtime/build_frontend.py --out out/metal306-frontend
```

This fetches the pinned public frontend, prepares it from the committed selected
ABI profile, and compiles actual Objective-C++ and C translation units with the
official iPhoneOS SDK targeting plain ARM64/iOS16.0. It checks the object load
metadata and compares its imported bridge symbol names with the matching Rust
source exports. No library is linked, plugin published, or native method run.
New-API availability warnings are suppressed only for this source compile;
runtime availability remains explicitly unqualified.

The root `*.json`/`*.log` reports and `objects/**` form the CI artifact. Public
sources remain under `prepared/` and `sources/`; no Apple header, binary or
capture is copied. `--prepare-only` performs source/provenance checks on other
hosts and reports `SOURCE_PREPARED_NOT_COMPILED`. A private cache is not needed
in CI, and its report records `private_cache_rechecked:false`.

`build_inputs.json` pins matching bridge sources and its 30 ordered translator
patches. The actual upstream build recipe uses translator `624bab39`, distinct
from the separately tested offline compiler's `219f0136`. Its Turnip recipe
reads Mesa `85fda3b3` plus 17 patches, while an older general catalog says
`c823a83e`; the recorded discrepancy must not be papered over by mixing trees.
The first compile stages matching bridge patch inputs, but does not fetch or
build Mesa, qualify a Rust Cargo lock or claim that every upstream workaround
is appropriate. Those remain separate, explicit link/runtime prerequisites.
