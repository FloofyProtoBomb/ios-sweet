# Metal306 package integration — UNVALIDATED

The new `dre_metal_factory.mm` and `package_dre_metal.py` are overnight source
drafts. Neither has been compiled, tested, linked, signed, packaged, installed
or run. Earlier official-SDK frontend and Linux Rust archive results remain
valid for their exact original artifacts; they do not cover this factory or
the new Darwin transport.

The package recipe consumes real SDK frontend object/hash reports, a true
iPhoneOS Rust archive (a host archive is rejected), and the exact DRE Turnip
source/build/UAPI profile. It attempts a strict SDK link without dynamic-lookup
or synthetic symbol waivers, then locally constructs an ad-hoc development
bundle. These are deferred actions, not completed results. Logs and input
objects remain available for real link diagnosis and relinking. It does not
copy into an iOS root, alter the phone or register any kernel service.

The principal-class bootstrap is a real `_MTLDevice` subclass selected through
NSBundle's existing class path. The original Metal306 factory calls its
`initWithAcceleratorPort:` method with the inspected32-bit IOKit port ABI. The
method checks the actual DRE registry identity and typed native capability
reply, then opens the matching frontend over the bundled Turnip library and
returns that actual frontend device. Missing capability, dependency, writable
cache directory, private protocol or class extent returns nil. No
`MTLCreateSystemDefaultDevice` override, AGX class spoof, selector tunnel,
arbitrary loader environment override or successful NSObject no-op is added.

The current kernel control service is an IOService named DREAdrenoGPU and
deliberately does not advertise IOAcceleratorES or a Metal plugin. Therefore
the original factory will not discover this bundle today. A real custom
accelerator publication/ancestry path and its registry identity relationship
must be implemented and validated later, after actual native render/compute
acceptance. The factory draft currently accepts only the exact direct DRE
service identity; it does not trust a random child or a claimed registryID.

This does not establish correct inherited `_MTLDevice` initialization,
`initLimits`/`initFeatureQueries`, complete method coverage, shared events,
IOSurface imports, source/format/family behavior, sandbox access or signature
acceptance on the selected20H392 system. All remain runtime qualification
steps. Current missing raw PM4/persistent-map capabilities make construction
fail even if the draft bundle is manually loaded.

The frontend and bridge retain their MIT provenance. The matching Rust
translator/runtime is GPL-2.0-or-later OR LGPL-3.0-or-later; Mesa retains
per-file licenses. A private development package and retained relink objects
do not establish redistribution-license completion. The recipe explicitly
keeps that audit false; complete license texts/source/relink obligations must
be addressed before distributing a product.

All new source states and native Metal/publication flags stay UNVALIDATED or
false until tomorrow's authorized checks provide exact evidence. The frozen
iOS image, tested kernel and earlier reports are unchanged.

The later source-only compiler connection adds `compiler_service.mm/.h` and
`connect_compiler.py`. The explicit future `build_frontend.py
--dre-compiler-service` profile connects actual request13 calls to the selected
20H392 compiler image, owns its real callback buffers and refuses unqualified
request14 stitching. Its three translation units are now mandatory future
package inputs; the old tested two-object artifacts stay untouched and cannot
qualify the new connection. No new recipe or compile has run. See
[COMPILER_SERVICE.md](COMPILER_SERVICE.md) for the actual call/lifetime path,
source boundaries and remaining runtime/system-client blockers.

The next unvalidated device-policy overlay is separate from all tested sources.
`--dre-device-policy` (with `--dre-compiler-service`) installs actual selected
Vulkan-backed allocation/workgroup facts and curated feature queries, blocks
unimplemented inherited device methods with exact-type forwarding, and withholds
Apple-family, shared-handle, no-copy and IOSurface claims. Matching Rust sources
and four real SDK objects are required by the future package recipe. No new
source recipe/compile/runtime has run. See [DEVICE_POLICY.md](DEVICE_POLICY.md).
