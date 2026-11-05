# Opt-in native GPU lifecycle — UNVALIDATED

`native_provider.cpp/.h` composes the existing actual kernel provider-v2
callbacks. It is source only: no tests, compilation, source recipes, CI,
emulator or phone operations have run. Creation returns a private function
table; it does not call `dre_gpu_native_publish`, register a Metal device or
install a default policy.

The caller must supply genuine GPU/RPM external brokers, actual common-lock
binding, an existing real GIC controller/lifetime policy, acquired GPU source
owner, actual SCM/PAS policy, independently approved shared-rail floors/masks,
real GCC source/global-register policy and real CB/SMR/ASID/global ownership.
Missing policy refuses. No token, rail
plan, clock rate, context bank or provider readiness is invented.

The actual service's `dre_gpu_owner_lock` is copied only inside provider-v2
`bind_owner_lock` while that exact mutex is held. Before its cookie can escape,
the returned retained value becomes1. The external policy must bind **all**
GPU, GIC, IRQ worker, RPM, SCM, domain and allocator lifetime operations to that
same mutex. This code introduces no second advisory mutex. Partial binding
keeps the service and callback cookies boot-resident.

Acquire obtains the actual GIC policy/controller after lock binding, composes
the real `gic_route` broker and calls actual `native_owner_create`. A private
read-only `dre_gpu_gic_retained_acquire` accessor returns exactly the recorded
upstream acquire outputs on failures too; `owns()` is never misused as a
partial-ownership query. Actual route generation/nub/handler/security/CPU-source
checks remain in the real controller and external authority. All retained
partial objects and genuine lease/reset epochs remain reachable.

Configure binds the kernel's actual raw mapped I/O and startup object to the
native owner. It allocates one real wired Normal-NC control object through the
kernel allocator, checks actual memory/page/cache/registry proofs and places
the boot binding, tables/CP metadata, router, votes and RBBM state there. It
never exempts arbitrary IOMalloc bytes from `owns_cpu`. If the selected real
pmap proof is missing, the actual allocator refuses; no alias callback returns
a requested cache flag as proof.

The native allocator's `DRE_GPU_ALLOCATOR_VERSION`, exact `struct_bytes` and
one-time status0 `classify` and typed `record_mapping` APIs are mandatory before the control allocation is
attempted. After actual NC/PA/registry and whole-buffer
read/write permission proof, configure classifies that exact handle and exact
memory view as `DRE_GPU_ALLOC_PRIVATE_CONTROL`, before populating the control object or
letting its pointers escape into VM/CP construction. A missing/rejected API
stops configure while the provider retains the actual allocation record.
There is no default-success fallback, range-based purpose guess, retagging or
change to the public ABI. Purpose tagging supplies no readiness or free right.
This CPU-only control allocation receives no GPU mapping record. Downstream
boot-memory/trusted constructors separately record their actual successful
staged VM mappings through exact allocation/VM/ops/handle arguments.

The anchored private firmware is loaded through its checked boot handoff and
actual private SHA/parser path. `boot_binding_firmware_prepare` owns the single
real PAS-buffer prepare call. **The current GPU8KiB reservation shares a16KiB
CPU granule with IPA, so this call refuses before RPM mapping/negotiation, rail
votes, clock/GDSC writes, reserved-page retagging, SCM authentication or cold
CP.** It preserves the firmware/control/native ownership already retained.
There is no widening, IPA reset, bypass or alternative mapper in this provider.
Real PAS metadata must still remain below4GiB inside the independently owned
low-DDR handoff; the provider does not replace it with an upper-RAM allocation.

The later path, reachable only after legitimate firmware preparation, connects:

1. The read-only original supply/GDSC graph plus genuine independently approved
   aggregate floor and discrete-mask values. Other resource/profile fields stay
   unchanged; DT init/proxy values never become an authorization by themselves.
2. Actual reserved CB/SMR/ASID state36 and external domain operations, retaining
   its actual reservation token on partial error. NC tables come only from the
   allocator's real table proof. POWERED/GLOBAL/QUIESCENT is not prefilled.
3. Actual native RPM Device mappings and io_router, requiring the genuinely
   identical GPU/RPM common lease. No numeric lease remapping occurs. Foreign
   vMPM bytes inside RPM RAM stay excluded by the actual providers/router.
4. The real boot binding's bounded CPU-only boot-memory/private trusted-queue
   preparation, with router.context pointing to that exact owned binding.
5. The real typed startup plan, actual GLINK endpoint negotiation and three
   matching RPM ACK holds. Unknown/uncertain publications retain every vote,
   buffer, firmware/source/domain lease and mapping; no rollback/off occurs.
6. Actual `gcc_hold` acquisition after the real ACKs. Mandatory genuine
   GPLL0/XO/interconnect/source references and globally shared GCC register
   serialization qualify strictly named enable/vote OR writes and actual
   readbacks. Existing600MHz GPLL0 is observed; no PLL/RCG/GDSC/reset or guessed
   source policy is programmed. Partial references/stores remain retained.
7. Return of that real plan to the existing kernel binding. The kernel alone
   calls `startup_begin` once. Actual power/SPTPRAC/RBBM precede its explicit
   domain-qualification callback, which proves live/quiescent/table ownership
   before the coordinator derives activation prerequisites. No second hidden
   power sequence runs in configure.

The six original native-owner apertures remain intentional: GMU wrapper
0x0596a000+0x30000 ends at0x0599a000 and fully contains GPUCC
0x05990000+0x9000. A second overlapping Device mapping is unnecessary.

The source table leaves session/BO/raw-submit/hardware-fence/cache/persistent
and exclusive-user-VM callbacks absent. Aliases use the kernel's actual selected
pmap implementation or refuse. `describe` does no powered GPU read and reports
zero accelerator capabilities, even if a future native marker passes. Private
CP marker or trusted-template completion is not Metal/Turnip shader execution,
task isolation, IOSurface import or iOS presentation support.

`arm_fault_irq` is a separate explicit owner-locked post-start call. Actual
native-owner live/marker checks gate it; configure and describe never arm it or
write an RBBM interrupt mask. There is no speculative general unload path after
callbacks/resource references have escaped.

Future selection requires `gpu/platform/native_provider.cpp/.h`, actual
`boot_binding.c/.h`, `gic_route.cpp/.h`, `io_router.c/.h`, `gcc_hold.c/.h`, native GPU/IRQ/RPM,
boot-memory/firmware/VM/CP modules and their real source hooks. Root's source
selector owns those edits and the standard libfdt/string-header rewrites. No
recipe is executed by this module or by these notes.
