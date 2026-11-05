/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight draft. */
#ifndef DRE_TOUCH_GPI_SPI_H
#define DRE_TOUCH_GPI_SPI_H
#include "native.h"

enum dre_tn_spi_state { DRE_TN_CLOSED, DRE_TN_RESERVED, DRE_TN_RUNNING,
                        DRE_TN_INFLIGHT, DRE_TN_QUARANTINED };
struct dre_tn_spi {
    struct dre_tn_ops ops;
    struct dre_tn_profile profile;
    struct dre_tn_clock clock;
    struct dre_tn_memory memory[DRE_TN_BLOCKS];
    void *lease;
    unsigned gpii, blocks, allocated_channels, event_allocated;
    unsigned tx_wp, rx_wp, event_rp, event_wp;
    unsigned state, mutated, completion_mask, transfer_bytes;
    uint64_t expected_tre[2];
    enum dre_tn_result first_error;
    uint32_t last_event[4], last_irq_type, last_error_log;
};

/* Initialize zeroed caller-owned storage. Lease and buffers remain live on
 * ANY failure after a possibly effective MMIO write. Inspect state and retain
 * this object; do not destroy/free it while quarantined. */
enum dre_tn_result dre_tn_spi_open(struct dre_tn_spi *, const struct dre_tn_profile *,
                                  const struct dre_tn_ops *);
/* One full-duplex8bit/mode0/CS0 transfer; buffers may be ordinary caller memory
 * because only the provider's pinned NC bounce buffers are given to DMA. The
 * destination is unchanged on failure. Maximum4096wirebytes. */
enum dre_tn_result dre_tn_spi_transfer(struct dre_tn_spi *, const uint8_t *tx,
                                      uint8_t *rx, size_t bytes);
/* Normal teardown only. A quarantined handle cannot be closed by this API. */
enum dre_tn_result dre_tn_spi_close(struct dre_tn_spi *);
#endif
