# Resident GPU/RPM I/O router — UNVALIDATED

`io_router.c/.h` connects the existing actual `native_owner` and `rpm_native`
Device providers through one resident kernel cookie. This source has not been
compiled, tested, staged, run in CI/emulation or executed on the phone. It
does not create a native GPU provider, power readiness, working DMA or Metal.

## Real inputs and lifetime

The GPU owner must already have acquired real native reset/source/bus/MMIO/IRQ
ownership and been bound to the original real GPU Device operations using
`dre_gpu_native_owner_bind_io`. The RPM owner must already have acquired its
real GLINK/IPCC ownership, remote epoch, Device alias proofs and actual receive
route policy. Both providers' brokers must be bound to the **same actual board
ownership mutex** used by the native GPU/VM/firmware/submit lifetime. That mutex
is held for every router operation. No new advisory lock or IRQ API is added.

`dre_gpu_io_router_bind` calls the actual owner guard, lease and I/O APIs plus
the actual RPM binding and lease operation. The nonzero leases must match
exactly. They must be the native broker's genuine common ownership identity;
coincidentally equal independent counters are not a valid common lease. The
router never rewrites/aliases lease IDs or supplies a provider that returns a
constant lease. Each later access repeats both real guards/owner-lock checks,
descriptor identities, exact resource shape and common lease checks.

The router borrows both provider objects; neither opaque C API transfers a
retain to it. The caller must keep those actual objects, their code/cookies,
all mappings, the actual owner mutex, this router and its optional outer
context resident through every plan use and uncertain failure. There is no
reset, close, remap, lower-vote, allocator, provider acquisition or reference
release hidden here. A guard/delegated-I/O/reentrancy failure invalidates this
connection and keeps all resource-lifetime decisions with the real owner.

## Physical routing and callback conventions

The GPU-only view forwards 32/64-bit accesses within the same six apertures
accepted by `native_owner.cpp`: GPU core, private GPU SMMU, wrapper/RSC,
GPUCC-related range, the separate GPU window and GCC. That range selection
only chooses a provider. The actual GPU owner still independently checks the
live mapping, lease/reset/IRQ state and **per-word powered `mmio_access`**.
The router cannot infer GPU core accessibility from accessible GCC/GPUCC.

The combined `dre_rpm_mmio_ops` view routes those same GPU accesses to that
owner, RPM message RAM to `rpm_native`, and only the zero SEND_ID write to
RPM IPCC `0x0020800c`. It rejects all IPCC reads, other endpoints, 64-bit RPM
accesses and unknown addresses. Foreign vMPM at `0x045f01b8+0x1000` is excluded
even though it lies inside the advertised RPM RAM. The actual RPM provider
repeats its original-FDT protected-consumer, per-word, remote/reset, receive,
Device translation/alias and real mutex/lease guards. No FIFO/TOC word or
neighboring rounded mapping is authorized merely by the routing decision.

The SMMU I/O view is further bounded to the actual GPU-private
`0x05940000+0x10000` resource. It does not access the touch/shared apps SMMU,
RAM, GIC, TLMM or arbitrary physical memory. Access widths and alignment are
checked before forwarding; unsupported accesses have no fallback.

Native binding APIs use `DRE_GPU_OK=0` status. Actual read/write/guard and
cold/policy callbacks require **exactly boolean1**; zero and negative errors
are never accepted as proof. A successful read is exposed only after the
forwarded operation and a final common-owner check both succeed. Every
delegated store is recorded as attempted before it could publish; an uncertain
result is not treated as an unpublished write or permission to undo/free it.

Barriers call both actual Device ordering primitives under the common guard.
`now` forwards the GPU owner's actual qualified 19.2MHz physical counter;
native RPM creation independently checks that timer frequency. No clock rate
is inferred from an observed tick value. No cache flush, DMA retirement,
interrupt readiness, rail vote or hardware completion is inferred from a
barrier/counter callback. Void-barrier failures leave the connection invalid,
so subsequent normal guards refuse; they do not report a fabricated success.

## One-cookie plan connection

`dre_gpu_io_router_plan_io(router, plan)` fills only:

- Combined MMIO lease/read32/write32/barrier/now with `cookie=router`.
- GPU I/O basic guard/read32/write32/barrier/now, copied identically into RBBM
  and CP I/O, all using that same cookie.
- GPU-private SMMU read32/write32/read64/write64/barrier/now and that cookie.
- Cold and policy predicates which delegate to the actual native-owner cold
  and policy operations with the exact common lease. They do not derive those
  proofs from an RPM ACK, DT graph, ready mask or the startup phase.

It leaves the stronger SMMU guard/owns-RAM, RBBM providers-ready, CP
platform-ready/mapping/firmware verification/authenticated-PAS, VM, allocator,
table/physical-page ownership, cache/retirement and actual startup state with
their real caller-supplied implementations. Missing callbacks remain NULL;
the startup coordinator consequently continues to refuse incomplete plans.
Existing nonmatching functions/cookies are not overwritten, and existing
memory/readiness callbacks must already declare this common cookie.
The helper runs before VM preparation/binding and refuses an already-bound
VM rather than silently changing an immutable VM operation table.

The optional `resident_context` is the caller's actual native context for
those memory/firmware/provider callbacks. `dre_gpu_io_router_context(cookie)`
returns that metadata without performing a guard: it grants no authority and
remains reachable after a fault for resource retention. Actual callbacks must
still consult their real allocator/VM/firmware providers. A context can also
embed the router as its first member when that layout is deliberately owned.

Intended order is: actual shared-lock binding and provider acquisition; bind
original GPU raw Device operations; initialize the router; fill known plan
I/O; supply actual stronger guard/memory/firmware operations using that cookie;
prepare/bind real boot memory and VM; supply actual RBBM/CP provider checks;
then let the existing coordinator evaluate and start the complete plan.
No part of this source runs that sequence or starts hardware automatically.

## Nonrecursive boundary and next work

Never bind the router's exported I/O back as the native owner's raw Device
operations. Underlying `check_lease`, raw MMIO/clock and RPM policy callbacks
must not recurse into router guards or the startup live checker. The separate
RBBM/CP provider predicates may call the proper coordinator phase checks, whose
raw I/O reaches only nonrecursive basic owner guards. Runtime entry guards
refuse recursive guard/access/predicate chains rather than recursing without
bound or treating them as acquired ownership.

This is a connection slice only. Real common broker leases, actual shared
mutex binding, RPM ACKed aggregate policy, source/clock/domain qualification,
native RAM/VM/SMMU/global reservation, private firmware/PAS authentication,
GPU cache/retirement and actual hardware execution remain prerequisites.
Tomorrow still requires source/build checks and ownership/error/recursion/
address/ABI fixtures before any authorized hardware use.

New translation unit: `gpu/platform/io_router.c`. New header:
`gpu/platform/io_router.h`. No staging, kernel provider, native owner, RPM
provider or other shared source was changed by this slice.
