# iPhoneOS SDK boundary

The transport/factory previously included macOS's `IOKit/IOKitLib.h` and
requested an IOKit SDK framework link even though iPhoneOS SDKs need not expose
those headers/stubs. This did not implement an actual native provider and could
stop compilation before its real protocol was checked.

`dre_iokit_loader.h` uses actual SDK Mach and CoreFoundation types plus narrow
function-pointer signatures from the pinned [public Apple IOKitUser header](https://github.com/apple-oss-distributions/IOKitUser/blob/323ead896d04424f87184d8f6ff0cce811aab106/IOKitLib.h).
`iokituser-reference.json` records the source commit/header hash and eight APIs.
No Apple implementation or header is copied. The code resolves those genuine
entrypoints from the absolute existing IOKit framework path. It does not emulate
their operations, replace their selectors, manufacture Mach rights or try another
driver/library on failure.

Missing framework or any required entrypoint leaves the API unavailable;
connection operations return a real unsupported condition and matching fails.
The matching dictionary's consumed-reference contract is preserved. A once-bound
library reference remains process-held while clients can retain its pointers.
There is no weak import or `-undefined dynamic_lookup` waiver. Service identity,
typed reply sizes, task/connection generations, native capabilities and ownership
checks in the existing DRE transport remain mandatory.

## Actual checks and honest scope

The common non-Apple transport path compiles with strict Linux warnings.
Matching Mesa source preparation now applies all17 verified reference patches
and overlays the actual DRE backend. The pinned frame-diagnostics patch omitted
a Git new-file-mode header; preparation adds only explicit0644 metadata for the
declared `/dev/null` creation, recording both original and applied SHA256 plus
the normalization count. No context/shader payload or pin is changed.

`runtime/build_native_sdk.py` requires actual macOS/Xcode and cross-compiles the
Apple transport, IOSurface wrapper and factory object with the real iPhoneOS SDK.
It attempts a strict native transport dylib link with actual SDK frameworks and
checks that IOKitUser functions remain real runtime resolutions rather than
unavailable direct imports. It does not create a fake frontend export to complete
the factory link: the object retains its real `native_bridge_metal_open` import
until matching frontend/iOS Rust inputs can satisfy it.

The independent macos14 job in `dre-ios-rust.yml` runs this SDK boundary check
without depending on the ARM64 Rust runner queue. It links/signs the read-only
metadata observer first and writes a separate result/hash before attempting
native transport compilation, so a later native failure preserves the genuine
observer artifact. No target binary is run by CI. Reports distinguish object
compilation, strict transport linking, unresolved real factory dependencies and
runtime/GPU execution. An actual SDK result is still required; this document
does not claim that an unrun job passed.

This solves only source/SDK binding. Full Mesa KMD compilation/linkage, private
base tables, real Rust iOS ABI linking, task protection, raw PM4 validation,
persistent/cache/IOSurface synchronization and physical A619 compute/render
acceptance remain separate. No capability, physical-device publication or Metal
acceptance state is enabled by loading IOKitUser.
