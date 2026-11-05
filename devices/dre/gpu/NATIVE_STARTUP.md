# Native startup coordinator draft

**UNVALIDATED overnight source.** No compile, test, CI, emulator or phone run
has been performed on this addition. Existing tested image artifacts are
unchanged. The user requested implementation first and validation tomorrow.

`a619_startup.c` calls the existing production components in one serialized
native lifetime. It does not provide an allocator, clock policy, IRQ driver,
Vulkan device, Metal publication or a fabricated initialized device.

The native binder supplies a zeroed `dre_a619_startup`, a prepared start plan
and the same private kernel cookie for its MMIO/ownership operations. The plan
contains actual held RPM votes, the qualified inherited aggregate policy,
real source/bus and reset/IRQ ownership, and prepared memory/domain/firmware
operations. The plan and pointed-to providers remain immutable and pinned
through reset or independently proven retirement. User-supplied readiness
flags must not populate these providers.

The coordinator performs these actual calls:

1. Establish unchanged exclusive lease and qualified inherited policy. A first
   RPM ACK or empty local history cannot prove an inherited APSS vote was not
   reduced. Missing policy refuses before acquisition.
2. Acquire full-variant Holi domains and branches from actual ACK holds and
   observed accepted sources. No automatic rail request or PLL/RCG guess.
3. Enable or borrow SPTPRAC using the existing bounded power component.
4. Apply the static RBBM/protection/cache/HWCG profile while cold-owned.
5. Activate the real owned VM/SMMU domain and prepared boot mappings.
6. Start CP/SQE, perform actual PAS13 authentication and the verified secure
   transition, then require the existing hardware marker retirement.

`NATIVE_START_MARKER_CONFIRMED` means only that restricted native startup
marker completed. It is not shader execution or permission to expose a
working `MTLDevice`. Raw command validation, task isolation, persistent CPU
mapping/coherency, cache draining, general scheduling, graphics/compute and
IOSurface presentation remain separate required implementations.

RBBM and CP readiness adapters can call the corresponding coordinator
observers through their native device cookie. Raw ownership/MMIO callbacks
must not recurse into those observers. The RBBM callback requires cold
ownership. The CP callback observes live providers without repeating cold
idle or reset requirements while ME_INIT or later jobs are running.

The new SPTPRAC observer performs reads only. It never calls the enable or
release paths on a running GPU. Its conservative existing ON/clock mask
needs specific next-day coverage with the HWCG profile; a clock-gated state
must not be silently converted into power recovery.

After uncertainty, the coordinator invalidates admission, records the failing
phase, poisons a potentially published queue and retains resources. It never
frees mappings, lowers a rail, powers off a borrowed domain or assumes that
invalidating software state reset the hardware. The native reset owner must
prove recovery before releasing the retained allocations.

Next-day work includes actual compilation, successful and interrupted order
fixtures, changing leases/policies, leaf failures after effective stores,
SPTPRAC/HWCG observations, cold versus running guards, and native bring-up.
No such validation should be inferred from this document or draft source.
