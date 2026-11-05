/* SPDX-License-Identifier: MIT -- UNVALIDATED source-only layout description. */
#ifndef DRE_RPM_LAYOUT_H
#define DRE_RPM_LAYOUT_H
#include "rpm_smd.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_RPM_LAYOUT_TOC_BYTES 256u
#define DRE_RPM_LAYOUT_MAX_ENTRIES 20u
#define DRE_RPM_LAYOUT_AP_TO_RPM UINT32_C(0x61703272)
#define DRE_RPM_LAYOUT_RPM_TO_AP UINT32_C(0x72326170)

/* All extents are RAM-relative half-open byte intervals. They are descriptions
 * of source-defined use or declared resources, NEVER an owned/MMIO allowlist.
 * The actual current native broker/router's full MPM exclusion stays intact.
 */
struct dre_rpm_layout_extent { uint32_t offset,bytes; };
struct dre_rpm_layout {
    uint64_t message_pa,foreign_mpm_ipc_pa;
    uint32_t message_bytes,foreign_mpm_ipc_bytes,mpm_irqs,mpm_words;
    struct dre_rpm_layout_extent mpm_declared,mpm_linux_footprint,mpm_unknown_tail,toc;
    /* Timer low/high together, then ENABLE,FALLING,RISING,POLARITY,STATUS.
     * These are the exact pinned Linux driver's accessed registers only;
     * they are not a complete RPM firmware or other-consumer footprint.
     */
    struct dre_rpm_layout_extent mpm_registers[6];
};
enum dre_rpm_layout_problem {
    DRE_RPM_LAYOUT_BAD_MAGIC=1u<<0,
    DRE_RPM_LAYOUT_BAD_COUNT=1u<<1,
    DRE_RPM_LAYOUT_ENTRY_RANGE=1u<<2,
    DRE_RPM_LAYOUT_TOC_OVERLAP=1u<<3,
    DRE_RPM_LAYOUT_ENTRY_OVERLAP=1u<<4,
    DRE_RPM_LAYOUT_MPM_LINUX_CONFLICT=1u<<5,
    DRE_RPM_LAYOUT_MPM_UNKNOWN_CONFLICT=1u<<6,
    DRE_RPM_LAYOUT_MISSING_TX=1u<<7,
    DRE_RPM_LAYOUT_MISSING_RX=1u<<8,
    DRE_RPM_LAYOUT_DUPLICATE_TX=1u<<9,
    DRE_RPM_LAYOUT_DUPLICATE_RX=1u<<10,
    DRE_RPM_LAYOUT_PIPE_TRANSPORT_LIMIT=1u<<11,
    DRE_RPM_LAYOUT_FOREIGN_ENTRY_SEMANTICS=1u<<12
};
enum dre_rpm_layout_entry_kind {
    DRE_RPM_LAYOUT_FOREIGN=0,DRE_RPM_LAYOUT_TX=1,DRE_RPM_LAYOUT_RX=2
};
struct dre_rpm_layout_entry {
    uint32_t id,offset,declared_bytes,kind;
    /* Only ap2r/r2ap have grounded cursor+payload extent:8+declared_bytes.
     * For foreign IDs this is ONLY their declared offset+size extent.
     * Their actual header/trailer/dynamic use remains unknown and unowned.
     */
    uint64_t described_bytes;
    uint32_t problems,overlaps; /* entry-index mask, not ownership slots */
};
struct dre_rpm_layout_report {
    struct dre_rpm_layout layout;
    struct dre_rpm_layout_entry entry[DRE_RPM_LAYOUT_MAX_ENTRIES];
    uint32_t count,problems,foreign_entries,tx_index,rx_index;
};
/* Pure bounded original-FDT inspection, no MMIO/TOC read/allocation/ownership
 * callback. Exact known Holi resource profile only. Output on description is
 * RPM_LAYOUT_FDT_DESCRIBED_UNOWNED; that string grants no access or lease.
 */
const char *dre_rpm_layout_from_fdt(const void *,size_t,struct dre_rpm_layout *);
/* Interpret a caller-supplied already captured EXACT256-byte LE TOC snapshot.
 * Never read hardware, manufacture offsets, reset cursors, negotiate GLINK,
 * request rails or infer a stable remote epoch from these bytes. Unknown IDs
 * retain unknown semantics and cannot identify free gaps. All descriptions
 * remain UNOWNED even when no overlap is observed. No live/cold/ready result.
 * Output is reset before parsing. Structural problems remain in the report;
 * malformed/unsupported data never becomes a native transport configuration.
 */
const char *dre_rpm_layout_describe_toc(const struct dre_rpm_layout *,
    const void *snapshot,size_t snapshot_bytes,struct dre_rpm_layout_report *);
#ifdef __cplusplus
}
#endif
#endif
