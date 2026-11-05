# Actual SCM64 discovery and retained PAS13 transport

The new optional path supplies real SCM convention queries, packet validation,
serialization, continuation tracking and firmware-source lifetime. It does not
provide default PAS13 ownership or infer EL3 authentication from availability.
No actual SMC has been executed by the local validation tools.

`scm_native.c` implements the portable production state engine.
`scm_native_xnu.cpp` binds it to the actual common service owner mutex, a real
module-wide IOLock and logical transaction registry, the existing ARM64 SMC
instruction, checked EL1/19.2 MHz counter, original immutable board FDT, and a
real hold on the opaque privately verified firmware source. The hold counter
prevents the firmware object from being freed while the SCM adapter retains it.
Objects with escaped callbacks remain resident after retirement.

## Source and actual board facts

The exact original FDT has `/firmware/scm` compatible `qcom,scm` and no `clocks`
or `clock-names`. At pinned Lineage commit
`98a38783f42f5588bf00cd933e299aedc589bc10`, generic `qcom_scm_dt_match` has no
required clock mask; absent optional clocks become NULL and their enable calls
are no-ops. This narrowly qualifies the empty named SCM clock set. Unexpected
clock, method/conduit, register or compatible variants refuse. The separate
`/soc/pil_scm_pas` interconnect request remains mandatory platform policy;
observing its DT property does not acquire bandwidth or shared source holds.

Primary implementations:

- [qcom_scm.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/firmware/qcom_scm.c)
  defines named-clock handling and PAS wrappers.
- [qcom_scm-smc.c](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/firmware/qcom_scm-smc.c)
  probes the convention using a fast call, serializes standard calls and
  preserves the Qualcomm x6 continuation value. Only the actual 64-bit
  convention is implemented here; unsupported/legacy conventions refuse.

Discovery performs the real FAST64 call `0xc2000601`, arginfo 1, x2
`0x02000601`, requiring transport result 0 and service result 1. It then queries
availability of PAS service 2 commands 1, 2, 5, 6 and 7. Availability is recorded
only from those actual replies; it is neither request ownership nor permission
to load/authenticate the signed image. No synthetic discovered callback exists.

## Explicit request ownership and real reply state

Private platform config version 2 requires exactly one of the previous
`firmware_policy` factory or the new `scm_request_policy` factory. Outer native
provider version 2 and public selector ABI 1 are unchanged. The new metadata
factory supplies actual acquire/check/consumer-retirement/release operations
for PAS13 exclusion, the verified boot-held memory loan, PIL interconnect and
source references, reset lifetime and security references. No implementation of
that board request broker is invented by these files. Missing real policy
refuses before discovery or firmware mapping.

Configure explicitly creates/acquires/discovers this channel, then obtains
firmware ops based on the observed convention and exact clock profile. Actual
`PAS_INIT_IMAGE`, `PAS_MEM_SETUP`, `PAS_AUTH_AND_RESET` and shutdown replies
remain the sole secure state transitions. Pre-call ownership proves a
legitimate serialized AP request, never a future EL3 grant. The engine accepts
only PAS13, the exact metadata/destination/span from the opaque verified v2
source, the pinned argument types and valid phase order. It accepts no generic
SIP service, caller-chosen PA or arbitrary SMC packet through userspace.

Each raw SMC takes the real shared IOLock during the instruction. Continuations
retain an actual registry owner/ticket while ordinary mutexes are released on
every return. The next continuation must match the original arguments and
actual returned x6. Other channels cannot take that slot. An unresolved call,
lost owner proof or failed registry release marks it quarantined and retains
the request lease, security references and firmware hold. No API returns with
an extra IOLock held, and no uncertain registry is silently cleared. Recovery
needs a separately proven reset path, which is not supplied here.

Terminal BUSY results release only the transport transaction, keeping the PAS
request/source lease. Bounded retries/backoff use the actual counter. A service
failure does not advance secure phase; the existing PAS engine can issue its
real shutdown cleanup. The channel becomes retirable only after observed
shutdown/off state, no active or uncertain transaction and actual platform
consumer retirement. Policy release happens last; failure retains state.

## Verification and remaining blocker

`test_scm_native.c` runs the actual engine, libfdt parser and existing PAS13
engine against synthetic firmware replies. It checks convention/availability
failures, exact empty-clock profile, malformed/foreign packets, phase order,
continuation arguments/x6, BUSY, lost ownership, failed transport/registry
release, actual authentication/shutdown composition and retained uncertainty.
An optional private DT argument performs only read-only metadata inspection.
The host/sanitizer and selected-XNU object builds establish source behavior,
not actual EL3 acceptance, native GPU initialization or Metal support.
