# Read-only UI boot diagnosis

The combined 67-patch kernel starts real iOS 16.7.16 (20H392) userspace in
QEMU. At 300 guest seconds the visible frame is Apple's boot logo. Setup.app
has not been observed running. This is a userspace boot investigation, separate
from native A619 initialization and Metal, which remain incomplete.

## What the captured run establishes

The run used kernel SHA256
`2d388d12a28ed31dcffb316099604950c747127c5f6edd182985be72a8223f79`
and the frozen full-root image. It created real double-buffered 1125x2436
IOSurfaces and submitted AppleCLCD framebuffer swaps. The CPU frame path still
works with `0104-dre-iosurface-lifetime.patch`. Repeated `IOAcceleratorES`
matching with zero results did not prevent these CPU-rendered frames.
Registering a dummy accelerator or forcing a different renderer is not
supported by this observation.

The final 2 GiB RAM capture was made while all CPUs were paused. Its SHA256 is
`530a436f1d2180b1321512eba0f13f0687c38cf445ac9dc1eac9e28196ffa2af`.
The offline walker found 128 processes without translation/list errors. The
configuration-derived `sizeof(struct proc)` is 1920, matching the captured
kernel global. The reports and raw Apple memory remain private and outside
the repository.

| Saved user stack | Evidence and practical implication |
| --- | --- |
| SpringBoard main thread | `__ulock_wait` through `_LSContextInitCommon`, `LaunchServices::Database::Context::_get`, and `LSApplicationRecord` into ControlCenterServices' associated-bundle filter. LaunchServices database readiness is a concrete dependency before further UI progress. |
| lsd main thread | Dispatch synchronization through `_LSInstallProgressService performJournalRecovery`, `beginListening` and `LSServerMain`. |
| lsd worker | Dispatch-group wait in `__LAUNCH_SERVICES_CLIENTS_ARE_WAITING_FOR_THE_DATABASE_TO_FINISH_SEEDING__`, called by `LSDatabaseBlockingFetchClient getServerStoreBlockingWithCompletionHandler`. Another worker waits in `restoreInactiveInstalls`. |
| lsd synchronous XPC worker | InstalledContentLibrary's `GetEligiblePersonaForBundle` and UserManagement lead into an NSXPC synchronous reply wait. This identifies a service dependency; it does not demonstrate an unsupported persona syscall. |
| usermanagerd main thread | Ordinary Mach-message/CFRunLoop service wait. |
| usermanagerd worker | `TH_RUN`, saved in `__CFStringChangeSizeMultiple`, `CFStringAppend`, string formatting, and `-[NSXPCConnection _initWithPeerConnection:name:options:]` while creating an incoming XPC peer connection. This worker is not shown blocked on a persona API. |

The last worker's saved CPSR is `0x60000000`, ESR `0x56000080` (SVC64 #0x80),
and exception field zero. FAR and ESR can describe an earlier exception; they
are not evidence of a current memory fault at the saved PC. Kernel CPU register
snapshots in timebase functions or virtual disk I/O likewise do not establish
a permanent spin or deadlock.

Datamigrator was absent from this final process list. Repeated earlier exec
records alone do not establish that migration is actively progressing or that
it caused the displayed logo. The independently read-back mobile migration
preferences already specify the selected build, the migration sentinel is
absent, and no migration-plugin wrapper/progress UI has been observed. No
renderer, persona or migration preference patch follows from this snapshot.
A longer bounded run with another frozen capture is needed to distinguish
slow service startup from a persistent wait.

## Reproducing the inspection

`saved_stack_layout.c` emits constants only. It is never linked into or run by
the kernel. Compile it using the captured kernel's exact configured source and
generated headers plus its recorded compiler command log. The BSD and Mach
records compile separately because their private headers have conflicting
typedefs. The output directory must be new and outside the input build tree.

```sh
python devices/dre/gpu/build_saved_stack_layout.py \
  --buildtree /private/matched-build-tree \
  --build-log /private/matched-build.log --zig /path/to/zig \
  --out /private/new-layout

python devices/dre/gpu/inspect_saved_stacks.py \
  --layout /private/new-layout/layout.json \
  --ram /private/capture/final-ram.bin --ram-base 0x100000000 \
  --cpu-ttep /private/capture/cpu_ttep.bin \
  --allproc /private/capture/allproc.bin \
  --native-pt-attr /private/capture/native_pt_attr.bin \
  --proc-struct-size /private/capture/proc_struct_size.bin \
  --capture-report /private/capture/result.json \
  --target SpringBoard --target backboardd --target lsd \
  --target usermanagerd --out /private/new-stacks.json
```

The capture report must mark the final memory capture complete and all CPUs
paused, identify the exact input files and bounds, and contain the RAM SHA256.
The inspector hashes RAM, verifies the captured proc size, and opens it
read-only. Existing output paths are refused. A matching proc size is one
configuration guard, not a substitute for matching all source/configuration
and generated headers. The tool does not attach to a running VM or phone.

The walker supports the captured native 16 KiB kernel tables and reads each
task's actual 4/16 KiB page-table geometry. It respects XNU queue-iterate
method 2: thread-list links point to thread objects, not embedded link fields.
The shared-cache slide is the actual 32-bit `sr_slide` field. Frame reads
cross virtual page boundaries separately. Process/thread lists, frame depth,
physical reads and total read bytes are bounded. PAC configurations are
refused pending a separate decoder. Nonresident pages and invalid lists stop
that part of the inspection with an error rather than inventing a stack.

Symbolicate cached addresses by subtracting that process's captured
shared-cache slide, using its exact private dyld cache. For a main executable,
use its Mach header/segment slide instead. Do not subtract the cache slide
from executable addresses. No Apple binaries or disassemblies are needed in
the repository.

Saved user states can predate the pause, and frame-pointer walking cannot
reconstruct every optimized frame. These are dependency observations, not
live kernel-stack samples, Metal evidence, or proof of a permanent deadlock.
Across separate boots, compare process roles and symbolicated call chains;
process/thread IDs and address slides can change.

## Validation

`python devices/dre/gpu/test_saved_stacks.py` passes 26 synthetic cases:
separate 16 KiB kernel/4 KiB user translations, a frame spanning physically
nonadjacent pages, block descriptors, fixed-native paging attributes,
unaligned 32-bit shared-cache slides, actual queue-link semantics, saved
exception fields, bad/PAC layouts, missing pages, cycles, bounds/read budgets,
and incomplete/unpaused/hash-mismatched/configuration-mismatched captures.
Both compile-only record objects were also compiled as ARM64 Mach-O against
the configured XNU headers. The resulting tool then verified the complete
real frozen RAM hash and walked the four services above without errors.
