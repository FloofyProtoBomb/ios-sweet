/* SPDX-License-Identifier: MIT -- UNVALIDATED native source, no execution. */
#pragma once
#include "rpm_mmio.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_RPM_NATIVE_BROKER_VERSION 1u
struct dre_rpm_native;
struct dre_rpm_native_resources {
    struct dre_rpm_mmio_map map;
    struct dre_rpm_config protocol;
    uint64_t message_mapping_bytes,ipcc_mapping_bytes;
    uint64_t protected_mpm_pa,protected_mpm_bytes;
    uint32_t receive_spi,receive_intid,receive_flags,gic_phandle;
};
/* Status callbacks return0 ONLY for the actual operation/proof. This is a
 * mandatory externally implemented platform policy, not a local ready mask.
 * The SAME real owner mutex covers create, callbacks, GLINK/RPM and close.
 * No default broker/transport ownership or shared voltage policy is supplied. */
struct dre_rpm_native_broker {
    uint32_t version,struct_bytes;
    void *cookie;
    int (*owner_lock_held)(void *);
    /* Reserve actual exclusive GLINK-RPM FIFO/remote-reset/IPCC receive and
     * mailbox ownership, with no competing channel or inherited live client.
     * Other consumers, including MPM in the same RAM, remain preserved.
     * A partial acquired resource MUST return a nonzero retained lease. */
    int (*acquire)(void *,const void *original_fdt,size_t,
        const struct dre_rpm_native_resources *,uint64_t *lease,uint64_t *remote_epoch);
    int (*check)(void *,uint64_t lease,uint64_t remote_epoch);
    /* All CPU aliases of the FULL rounded16KiB mapping granules must really
     * have compatible Device attributes. Does not grant neighboring access.
     * A requested VM_WIMG_IO value or local translation alone is insufficient. */
    int (*device_aliases)(void *,uint64_t,uint64_t,uint64_t pa,size_t rounded_bytes);
    /* Actual per-word TOC/FIFO/mailbox ownership and source/clock availability.
     * Must not recurse into these MMIO callbacks. Header-declared FIFO ranges
     * and an empty cursor alone do not grant authority over another consumer. */
    int (*access)(void *,uint64_t,uint64_t,uint64_t pa,size_t bytes,int write);
    /* Prove the real receive route remains leased/masked or serviced under
     * current polling policy. No IRQ registration, mask, ACK or EOI is hidden
     * in this adapter; INTID366 is never used as an IOKit source index. */
    int (*receive_owned)(void *,uint64_t,uint64_t);
    /* Real remote/FIFO/device/CPU/rail-consumer retirement is mandatory.
     * No lower/off/rollback message is ever sent by this module. */
    int (*retired)(void *,uint64_t,uint64_t);
    int (*release)(void *,uint64_t,uint64_t);
};
/* Gets the boot-held original FDT, verifies actual resource identities,
 * acquires actual broker ownership and maps only the qualified resources.
 * A partial acquired owner is returned through out even on error and MUST
 * remain resident. No GLINK negotiation, rail request or hardware store occurs. */
int dre_rpm_native_create(const struct dre_rpm_native_broker *,struct dre_rpm_native **out);
/* Real guarded Device IO for existing dre_rpm_bind_mmio/votes_open. Returns
 * copies, not pointers to caller stack descriptors. No negotiation is run.
 * Resource descriptions never imply an ACK, voltage plan or power readiness. */
int dre_rpm_native_binding(struct dre_rpm_native *,struct dre_rpm_mmio_ops *,
    struct dre_rpm_mmio_map *,struct dre_rpm_config *);
/* Private resident failure metadata, available under the actual board mutex
 * even after a lost lease. No MMIO, raw TOC, address inventory, user API or
 * ownership/readiness proof. Described conflict bits never grant access. */
int dre_rpm_native_diagnostic(struct dre_rpm_native *,const char **reason,
    uint32_t *toc_problems,uint64_t *attempted_writes);
/* Always preserves all mappings/leases on an uncertain publication/fault.
 * close consumes *owner ONLY after actual external retirement and release. */
int dre_rpm_native_close(struct dre_rpm_native **owner);
#ifdef __cplusplus
}
#endif
