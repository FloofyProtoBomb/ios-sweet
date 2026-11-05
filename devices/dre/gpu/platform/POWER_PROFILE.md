# Original-FDT GPU power graph

**UNVALIDATED source, unexecuted.** `profile.c` reads the complete private
original Holi FDT. It performs no allocation, MMIO, firmware operation or
resource acquisition. Source facts are pinned by `platform/sources.json` and
the already captured live nonidentity supply graph; no raw FDT or identifiers
are added to git.

The decoder follows actual unique phandles from GPU/GPUCC supplies to the
supported CX/GX GDSCs and actual RPM level children/resources. It checks
`rwcx`, `rwmx`, `rwgx`, IDs0, level-versus-microvolt semantics, active+sleep
set3, bounds, init/proxy settings, shared parent identity and the actual syscon
references used by this restricted GDSC variant. CX hardware-status/500us/
clock-wait8/no-disable-status and GX AON/reset/domain properties must match.
Unknown or duplicate GDSC/regulator/syscon properties refuse, including a new
clock, reset, collapse, retention or mailbox variant.

The init384/384/64 and proxy384..512 values are **DT source facts**. They do
not report a live inherited APSS aggregate vote. Output resource allowed-level
masks and aggregate floors deliberately remain zero, so passing the result
straight into RPM/power acquisition refuses. A real native provider still
needs independently qualified inherited aggregate rail/source/bus policy and
actual acknowledged held votes. The graph does not choose voltages or derive
them from the GPU's slow operating point.

Nominal253MHz/200MHz and XO19.2MHz fields identify the already supported
restricted provider's expected clock configuration. The existing provider
must read and qualify actual hardware PLL/RCG/branch state before mutation;
the graph decoder does not observe a frequency or acquire a clock. Global
reset, IRQ, SCM and source/bus ownership are separate mandatory providers.

`HOLI_POWER_GRAPH_OBSERVED_POLICY_REQUIRED` means only this narrow graph was
decoded. It never means power acquired, GPU ready, Vulkan enumerated or Metal
working. The helper is included only by the explicit native GPU source draft;
it has not been staged, compiled or tested.
