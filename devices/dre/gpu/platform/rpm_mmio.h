/* SPDX-License-Identifier: MIT */
#pragma once
#include "rpm_smd.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_RPM_IPCC_BASE UINT64_C(0x00208000)
#define DRE_RPM_IPCC_BYTES 0x1000u
struct dre_rpm_mmio_ops {
    uint64_t (*lease)(void *);
    int (*read32)(void *,uint64_t address,uint32_t *);
    int (*write32)(void *,uint64_t address,uint32_t);
    void (*barrier)(void *);
    uint64_t (*now)(void *);
    void *cookie;
};
struct dre_rpm_mmio_map {
    uint64_t message_base,ipcc_base;
    uint32_t message_bytes,ipcc_bytes,client_id,signal_id;
};
struct dre_rpm_mmio { struct dre_rpm_mmio_ops ops;uint64_t lease; };
/* Bind only the live-FDT Holi RPM RAM and AOP/GLINK_QMP mailbox. This performs
 * no MMIO or allocation. The owner supplies guarded Device mappings and an
 * exclusive lease, counter/ordering primitives and GIC_SPI334 (INTID366)
 * ownership. Polling
 * does not enable/clear receive interrupts or discard other clients' messages.
 * notify publishes a real SEND_ID(0x0c)=0, not a power-ready flag. */
int dre_rpm_bind_mmio(struct dre_rpm_mmio *,const struct dre_rpm_mmio_ops *,
    const struct dre_rpm_mmio_map *,struct dre_rpm_io *);
#ifdef __cplusplus
}
#endif
