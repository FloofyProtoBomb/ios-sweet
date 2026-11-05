# Selected306 compiler connection — UNVALIDATED

This is source implementation only. The connection, source connector, optional
SDK build path and package changes have not been run, compiled, tested, linked,
packaged or loaded. No CI, QEMU, iOS-image or phone action ran. Existing tested
two-object SDK frontend and Linux Rust archive artifacts remain unchanged and
do not qualify this new translation unit.

## Concrete source gap

The pinned AdrenoMetal frontend opens the local MTLCompiler service, but its
shared code searches top-level, Current and highest-numbered framework versions
for newer OS layouts. It also accepts an old shader-cache record independently
of the newly qualified306 reply envelope. Its stitching builder appends fields
described in that source as macOS27 requests. A528-byte device-class guard alone
does not qualify any of those compiler behaviors for20H392.

The separately inspected selected20H392 image exports the real
`MTLCodeGenServiceCreate` and `MTLCodeGenServiceBuildRequest`; its compiler UUID
is `0ACFC872-8027-3DFD-8F30-397C9CB25639`. Earlier source inspection/encoder
work qualified the no-inline-metadata/flags0 subset of request13 and the
104-byte archive reply, not the complete stitching request14 format. The
existing private captures show BuildRequest preserving its second argument as
a32-bit word before dispatch and invoking the reply block before return. No
Apple header, binary, disassembly or captured request is copied into this code.

## Real implementation

`compiler_service.mm` opens only the selected absolute top-level framework path
and resolves its actual Create/BuildRequest exports. It checks both symbols'
real loaded Mach-O image, plain ARM64 subtype and exact compiler UUID, requiring
the same provider image. It does not scan newer versions or claim UUID matching
alone is a code-signature or complete ABI proof. A missing/mismatched framework
or service fails explicitly.

The caller supplies the already encoded request bytes. The connection decodes
and checks all four little-endian request13 fields, exact source/argument sizes,
NULs, source padding and the existing16MiB-source/64KiB-argument bounds. It sends
flags0/type13 through the real BuildRequest function. Every other request type
is refused before opening the compiler.

Each serialized call owns a copy of its request and heap-owned callback state.
The worker has an actual16MiB pthread stack; failed stack/thread creation is an
error, without a smaller caller-stack fallback. Inside the actual reply block,
the connection checks status, a64MiB output bound and the existing archive
envelope, then copies warnings and the precise MTLB archive span before the
compiler can release its buffer. Malformed warning encoding, missing/multiple
replies, service failure, exceptions and join failures remain visible errors.
An unexpected callback/worker lifetime poisons further service use. In-progress
state survives a failed join through actual shared ownership; it is not freed
as though compilation had stopped.

A created private service and its library handle have process lifetime. No
uninspected destroy/cancel ABI is guessed. The selected call is synchronous;
the code does **not** promise a compiler deadline or kill an active worker and
release its inputs. Callback and concurrent/error lifetimes still need actual
runtime qualification. Archive-envelope acceptance is neither semantic library
validation, A619 compilation, successful pipeline creation nor GPU execution.

## Actual frontend connection and future build inputs

The **unrun** `connect_compiler.py` source connector operates only on a fresh
prepared copy from the exact pinned frontend. It replaces the actual
`codeGenRequest` implementation, removes the unchecked shader-cache bypass and
highest-version selection, and routes the real source compile method through
the new service. It changes the stitched-library method to a typed
`MTLLibraryErrorUnsupported` until the306 request14 format is qualified. Its
existing async and binary-archive overloads continue through that same method;
no successful empty library or fabricated shader is returned. The unused newer
stitching-only packet decoder is removed from that connected source profile.

`build_frontend.py --dre-compiler-service` is an explicit opt-in **deferred**
source/build path. It records the original and connected frontend hashes and
the new helper sources. A future official-SDK run must compile all three real
translation units: `NativeBridgeMetalFrontend.o`, `compiler_request.o` and
`compiler_service.o`. Existing default/two-object SDK reports are preserved.
No command using this new flag has been executed tonight.

`package_dre_metal.py` now requires those actual three-object proofs and exact
compiler-connection provenance, plus the same genuine iOS Rust and matching
DRE Turnip dependencies. An old successful frontend compile cannot silently
package the unchecked newer compiler path. Linking still uses strict unresolved
symbols, without `dynamic_lookup`. The local compiler is obtained at runtime
through its real selected exports, not a fabricated import library. The package
recipe has not been run and performs no image installation/publication.

## Remaining native/runtime work

No `_MTLDevice` initializer/private-feature qualification or accelerator-service
publication is provided here. The control service still lacks RAW, persistent
mapping/cache and genuine IOSurface import requirements, so the factory cannot
construct a native Metal device. Rust-to-iOS archive linkage, matching Turnip
build, loader identity/capabilities, actual source compiler calls and the full
translation/pipeline path remain unproven.

Source compile and precompiled-library paths are distinct. Request13 success
would prove only that the actual selected compiler returned a shaped archive;
the real translator/device must then accept it and hardware must execute it.
Stitching remains a material system-client blocker until its exact306 request
and reply semantics are implemented. No family/core feature table, software GPU
fallback, CPU-rendered frame or synthetic shader-success path is introduced.
