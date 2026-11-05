# Bounded native BO mappings and retirement

`a619_vm.c` manages real SMMUv2 page-table entries and BO mapping lifetimes
for an exclusively owned SID0 context. It supports explicitly allocated
4–256 MiB VA windows, rather than being limited to the legacy 2 MiB bootstrap
map. This is source and synthetic verification; it is not integrated into a
native allocator, scheduler, IOSurface resource provider or Metal device, and
has not executed on the N200.

## Allocations and ownership

Choose a power-of-two page count `N` between 1024 and 65536. The VA base must
be 2 MiB aligned, below 39 bits, and the entire window must fit one 1 GiB L2
region. Allocate and pin, with no aliases:

| Object | Required storage |
| --- | --- |
| L1/L2/L3 tables | `(2 + ceil(N/512)) * 4096` contiguous physical bytes, 4 KiB aligned, exact matching Normal-NC CPU mapping |
| Physical page metadata | `N * sizeof(struct dre_a619_smmu_page)` writable wired kernel bytes |
| BO lease metadata | `N * 8` writable wired kernel bytes |
| Reverse-PA hash | `2 * N * 8` writable wired kernel bytes |

All L3 tables exist before context activation. They are not allocated or
freed under live translation. A 64 MiB window uses 34 table pages; the largest
256 MiB window uses 130. Explicit table/metadata capacities are mandatory for
larger windows. Existing SMMU callers with both new capacity fields zero keep
the original three-table, at-most 512-page bound.

`owns_cpu` must prove actual wired kernel bookkeeping storage.
`table_mapping` must prove the CPU mapping, contiguous physical allocation,
Normal-NC memory type and exact size agree. `owns_ram` must prove each BO page
is an exclusively held allocation, not kernel code, a table page, a secure
reservation or another client's allocation. Data pages remain pinned until
successful retirement. These are trusted kernel-provider obligations;
user-controlled addresses, flags or digests are not accepted as proof.

The caller holds the domain, submission and BO-lifetime locks throughout each
operation. Its mandatory `quiescent` provider must prove all submissions are
blocked, hardware jobs and firmware references are drained, and any required
GPU cache clean/invalidate has actually completed. Power, clocks and domain
ownership must remain held. The mapper complements this with actual RBBM
idle/fault/IRQ and RPTR/WPTR checks, plus owned CB/stream/TTBR/TCR/MAIR and
fault-state readbacks. It does **not** implement the scheduler, firmware
reference tracking, CPU data-cache policy or GPU data-cache flush commands.
Those missing providers cannot be replaced with a constant true callback.
The eventual allocator must also retain each complete native kernel-page
allocation and its padding until all constituent 4 KiB GPU views retire.

## API sequence and supported behavior

1. Configure the SMMU state with the owned table allocation, page array,
   explicit capacities, VA range, reserved context/SMR/ASID and real platform
   providers. Set up the VM's matching metadata, lease and reverse-PA arrays.
2. `dre_a619_vm_prepare` verifies ownership/ranges and initializes CPU memory.
   `dre_a619_vm_map` stages BOs into empty slots without publishing them to
   hardware. A BO cannot contain W+X pages, table pages or a PA alias anywhere
   in the VM. Checked input page records are copied once into owned metadata.
3. `dre_a619_vm_activate` invokes the real SMMU activation sequence. Initial
   PA uniqueness is checked in O(n log n) using exclusively owned unpublished
   table memory as scratch, then the actual table tree is built. The ASID,
   stream, disabled-context and global hardware guards remain enforced.
4. While active, `dre_a619_vm_map` only fills an empty range after the real
   quiescence/ownership checks. It publishes the exact PTEs, orders writes,
   issues ASID invalidation and context TLB sync, and verifies completion,
   unchanged context ownership and PTE readbacks. The new BO stays pinned
   throughout. There is no in-place replacement of a valid mapping.
5. `dre_a619_vm_unmap` requires the full original mapping handle. It first
   verifies every lease, descriptor and table path, then writes invalid PTEs,
   barriers and completes the real context/ASID TLB synchronization. Only
   after idle/fault/ownership and invalid-PTE checks succeed does it clear
   metadata and return `retired=true`. The caller may then release the BO's
   pins, subject to its other CPU/resource references already being drained.
6. `dre_a619_vm_release` rejects a VM with any live mappings. Only an empty VM
   may disable/retire/restore its owned context. Table and bookkeeping memory
   can be freed after successful release, while submissions remain blocked
   until power-down or reassignment.

Each handle has a kernel-generated VM generation and monotonically allocated
BO lease. A stale, partial, wrong-lease or previous-generation handle cannot
remove a new BO. The reverse-PA table uses bounded 128-probe searches, so large
mapping validation avoids quadratic page comparisons; exceeding that bound
fails closed even when nominal VA capacity remains.

If a published mapping or removal has an uncertain MMIO/TLB/ownership/PTE
outcome, the VM and active context become unsafe. Old and newly reserved BO
pages remain recorded and pinned; handles do not become retired. No guessed
rollback or allocation reuse is attempted. Recovery requires an independently
verified GPU **and owned SMMU context** reset/quiescence/retirement, or a full
reboot. Resetting only the command processor is not sufficient to infer that
stale SMMU translations have disappeared.

## Verification and practical limits

The new 76-case independent model test maps two actual-stride iOS framebuffer
BOs: 1125x2436 BGRA, 4608-byte rows, 11,225,088 bytes and 2741 pages each. It
walks every physical leaf across the L3 boundaries in a 64 MiB window, adds
and retires mappings while active, and checks RO/NX and executable-RO pages.
The packed 4500-byte-row payload would use 2677 pages; it is not the fixture
used for this dual-surface capacity check.

A separate dense 256 MiB test constructs and independently walks all 65536
pages, then retires the complete mapping. Other cases cover generations,
stale/partial handles, duplicate PAs, input mutation after validation,
permission/range/ownership errors, reverse-PA probe limits, tampered PTE/TTBR,
busy GPU/IRQ/fault state and failed/timed-out TLB retirement. The test observes
that leases remain live during TLB synchronization and after uncertain
failure. All 74 existing bootstrap SMMU regressions still pass. GCC ASan/UBSan
and freestanding ARM64 ELF/Mach-O builds also pass; the object has no hidden
atomic-runtime dependency.

This establishes bounded source behavior and table geometry, not hardware
execution, full Metal resource capacity, concurrent GPU remapping, heap
aliasing, paging, a memory allocator, shader cache coherency or complete
Metal support, or per-client GPU-address-space isolation. Real platform
providers, buffer allocation/mapping,
completion/cache/lifetime integration and native acceptance remain required.

Architectural facts and the hardware sequence are checked against the pinned
[ARM SMMU driver](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/iommu/arm-smmu.c),
[LPAE table implementation](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/drivers/iommu/io-pgtable-arm.c)
and [Holi GPU/SID binding](https://github.com/LineageOS/android_kernel_oneplus_sm4350/blob/98a38783f42f5588bf00cd933e299aedc589bc10/arch/arm64/boot/dts/vendor/qcom/holi-gpu.dtsi).
