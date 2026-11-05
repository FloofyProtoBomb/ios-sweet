# Native GPU boot callback binding — UNVALIDATED

This is source-only overnight work. No test, compile, CI run, staging recipe,
emulator or phone operation has validated these files. The binding prepares
real callbacks and memory; it does not establish a working native GPU, Metal
runtime, shader execution, safe user submission, or a power policy.

`boot_binding.c` is the new translation unit. It includes the local
`boot_binding.h`, which uses `io_router.h`, `boot_memory.h`,
`../firmware/native.h` and `../kernel/trusted_queue.h`. Link the existing real
router, allocator-backed boot-memory helper, trusted-queue helper, startup,
VM/SMMU and privately anchored native firmware loader. There is no replacement
implementation, default provider, automatic service publication or standalone
recipe to execute.

The caller must hold the same actual kernel service owner mutex over every
operation. Both `dre_gpu_boot_binding` and `dre_gpu_boot_memory` must reside in
actual private wired allocations recognized by the supplied native allocator.
An arbitrary IOMalloc range, requested cache option, DT property or known PA
is not an ownership or Normal-NC proof. Native allocator, owner/RPM/router,
firmware, startup, domain-policy cookies and control storage remain resident
through all callbacks and uncertain failures. The binding releases nothing.

## Actual construction order

1. Acquire the real native GPU broker/map/IRQ/reset lease, bind its real raw
   kernel I/O and actual startup object, then allocate the private Normal-NC
   control object. Zero that fresh control object before use.
2. Load the actual privately anchored firmware package. Obtain actual held
   PAS13, SCM-discovery, SCM-clock, alias and consumer-retirement providers.
   Call `dre_gpu_boot_binding_firmware_prepare` once. This calls the existing
   `dre_gpu_fw_native_prepare_pas13` once and retains its exact native status.
   It also obtains and rechecks the actual entire anchored SQE source file.
   The real GPU8KiB/IPA shared16KiB mapping refusal must propagate unchanged
   here, before RPM endpoint/vote, GPU domain/clock or CP stores. Do not call
   firmware preparation separately and then ask this binding to repeat it.
3. Obtain the real shared-rail aggregate/rate policy and actual unused
   nonsecure SID0 CB/SMR/ASID/global-owner reservation. Its prerequisite state
   describes only already-held `CB_OWNED|ASID_OWNED` facts, exactly36. Reserve
   and retain the external reservation token on every partial failure. No
   table-NC, power, cache-drain or global-initialization promise belongs here.
4. Construct the real native RPM mapping/lease and bind the router with
   `router.context` equal to the exact `dre_gpu_boot_binding` address. GPU and
   RPM must already have the same real nonzero lease and common owner mutex;
   this module does not assign or remap lease identities to make them match.
5. `dre_gpu_boot_binding_init` composes actual router and allocator operations.
   It requires the firmware preparation recorded by step2, genuine reserved
   domain callbacks, fresh actual startup/control state and real storage
   ownership. Status0 is success. It does not power or initialize anything.
6. `dre_gpu_boot_binding_prepare` invokes `boot_memory_prepare` and then
   `trusted_prepare` synchronously within one tightly bounded private CPU-only
   section. Its guard requires the real reservation there. Fresh actual NC
   table-prefix proof derives `TABLES_NC` in the existing helper, giving44.
   Both helpers stage real VM metadata; neither publishes GPU PTEs or executes
   a device operation. The queue receives `&memory.ops`, which is the exact
   callback-table address later stored in `startup.plan.vm_ops`. A partial
   opaque queue stays reachable. The private scope is cleared on every normal
   exit, including failures; no public toggle or caller-supplied scoped body
   exists. Only `BOOT_BINDING_CPU_STAGED` means both helpers succeeded.
7. Supply actual votes, power configuration, an owned fresh RBBM state and a
   bounded19.2MHz CP budget in a fresh plan. `dre_gpu_boot_binding_plan`
   installs only the actual router, memory, firmware and coordinator callback
   composition; existing nonmatching callbacks or foreign state pointers
   refuse. It preserves real external power/policy inputs. Its success is
   `BOOT_BINDING_PLAN_BOUND`. It does not call `startup_begin` or request votes.
8. The native provider obtains actual matching RPM ACK holds. The kernel
   lifecycle marks real allocations retained/published and calls the existing
   `startup_begin` exactly once. There is no second hidden startup here.

The known shared firmware-granule refusal currently prevents reaching the
later construction steps on the captured board layout. This module does not
widen the GPU mapping, retag IPA's page, reset IPA or accept a readiness flag
to bypass that refusal.

## Real callbacks and phase boundaries

Every VM/CP/coordinator callback cookie is the same resident router. Metadata
lookup retrieves the exact binding and verifies router identity, actual owner
guards/lease, control allocation ownership and immutable VM-operation tuple.
Allocator `owns_cpu`, `owns_ram` and memory/table-prefix proofs are genuine
provider calls; they are not simulated with allowed address ranges.

VM hardware idle checks use its `mmu.read32` for both GPU idle/fault/ring
registers and SMMU registers. Its composition therefore uses the router's
genuine GPU-aperture read32 operation, while SMMU write32/read64/write64 remain
the router's existing SMMU-scoped operations. This adds no aperture, RPM access
or unguarded I/O. The exact VM MMU table is then bound to startup's SMMU table.

Outside the two private CPU-only preparation calls, the MMU guard requires
actual current powered/global/TBU/domain ownership plus the exact owned NC
table prefix. It never uses reserved-only semantics for activation, running
CP validation or runtime BO updates. It does not demand a cold-idle GPU while
CP is executing. VM quiescence additionally calls the actual external
quiescence provider; it is never inferred from ring idle or TLB synchronization.

At startup's actual DOMAIN phase, after real power/SPTPRAC/RBBM acquisition,
the mandatory `qualify_domain` callback checks reservation identity, pristine
inactive domain, prerequisite44, current live ownership, genuine quiescence
and the exact NC table prefix. It writes no prerequisite flags. The existing
coordinator derives44→63 only after that exact proof and its own still-held
power/cold-owner checks. The leaf SMMU code independently inspects real global,
CB/SID/fault/table state and performs actual activation.

RBBM-ready and CP-ready wrappers invoke the actual startup predicates at their
real phases. CP mapping checks use the existing exact allocator/VM/PTE proof.
Firmware verification checks full anchored SQE identity, size and actual CP
file pointer. The PAS callback calls the native authenticated loader only
after CP has reached its actual ME_INIT stage3 and real CP-ready checks pass;
only the native `PAS13_AUTHENTICATED` result permits its boolean success.

## Missing native authority still refuses

The caller's `dre_gpu_boot_domain_ops` is mandatory and supplies three real
exactboolean1 proofs, all under the genuine common owner lock:

- `reserved`: actual exclusive nonsecure unused CB/SMR/ASID and global/stream
  reservation, matching the retained reservation lease and requested tuple.
- `live`: additionally actual current power and already-initialized shared
  global/TBU/domain authority. It must check acquired power safely before any
  MMIO and must not require the still-disabled private CB to be active during
  preactivation qualification. It must remain valid while the owned CP runs.
- `quiescent`: actual submission/scheduling exclusion, job and firmware/DMA
  reference retirement and completed required GPU cache operations. Plain
  CP idle, a marker, CPU memory barriers or TLB sync cannot satisfy it.

These callbacks create no missing provider. In particular, actual shared-SMMU
initialization/global reservation, complete cache/firmware retirement,
cross-driver shared-rail floors, source/rate qualification and the GPU/IPA
firmware-alias policy remain external native obligations. No default callback
returns success for them. The domain callbacks must not recursively enter VM,
startup or this binding; the local recursion guard refuses such reentry.

Actual post-marker private queue attachment may be requested separately using
the retained `private_queue` and exact actual startup object. Preparation does
not attach it, submit commands, claim a user command validator or expose Metal.
All partial firmware/control/VM/queue allocations remain held on failure.
The first trusted-state allocation additionally writes a caller-owned wired
journal before verification, so a partial first buffer stays reachable even
when a safe opaque queue could not yet be created. This is a retained handle,
not permission to dereference or free unqualified memory.
There is deliberately no reset, cleanup, lower-vote or speculative free path.

Tomorrow's validation must cover callback identity/order, private CPU scope
exit on both helper failures, real allocator/table ownership, phase-specific
reservation/live/quiescence refusal, actual anchored firmware refusal and
authentication sequencing, partial lifetime retention and official SDK
compilation before any opted-in recipe or hardware diagnostic is considered.
