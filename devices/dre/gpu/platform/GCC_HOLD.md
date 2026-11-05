# Named GCC parent acquisition draft

**UNVALIDATED source only.** No compiler, test, source recipe, register access,
clock/RPM operation or phone action has run.

`gcc_hold.c` adds concrete enable/hold operations for the external named
parents expected by the existing Holi power helper. Pinned `gcc-holi.c`,
`clk-branch.c` and Fabia definitions in `sources.json` ground these registers:

- GPLL0-div APSS vote:0x01479004 bit16.
- GPU config AHB:0x01436004 bit0.
- BIMC GPU AXI:0x01471154 bit0.
- GPU MEMNOC:0x0143600c bit0.

It verifies the actual existing600MHz GPLL0 shape before and after acquiring
real source references, then ORs only those known enable bits under actual
global shared-register serialization. It preserves inherited HWCG and all
unrelated register bits. Actual readback/branch-state polls use the checked
19.2MHz counter and finite per-stage bounds, including the pinned10us
BRANCH_HALT_DELAY for the GPLL0-div vote. It performs no PLL/RCG programming, frequency
selection, bandwidth vote, GDSC sequence, reset or voltage request.

The mandatory external policy must genuinely hold the GPLL0/XO/rate and shared
interconnect/source dependencies, qualify actual existing RPM ACK holds against
inherited aggregate platform policy, and serialize every competing APSS writer
of a shared control register. A local lock, DT min/proxy value or a successful
clock request alone cannot supply that authority. No default policy, controller,
fabricated token or successful ownership stub is supplied.

The source/register lock order is the real common board mutex followed by a
qualified sleepable GCC register-owner mutex, compatible with guarded IOKit
callbacks. Primary IRQ/spinlock context is unsupported. Parent policy must not
recurse through these operations or silently retune a source while held.

Partial source references and any possibly effective enable writes remain
recorded. There is no automatic clear/off/decrease/rollback/release routine.
`validate` observes real source/ACK/gate state without reacquiring anything.
`GCC_NAMED_PARENTS_HELD` reports this restricted hold only, never GPU, shader,
Vulkan or Metal readiness. The separate GPU PLL/RCG configuration and all real
global rail/source/IRQ/domain policies remain required. A future native provider
may use this after PAS-buffer acceptance and actual RPM ACKs; no call is made
by staging or writing this helper.
