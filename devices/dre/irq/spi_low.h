/* SPDX-License-Identifier: MIT -- UNVALIDATED, private source-hook ABI. */
#ifndef DRE_SPI_LOW_H
#define DRE_SPI_LOW_H
#include "dre_spi.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*dre_spi_dispatch_fn)(void *,uint32_t);
typedef int (*dre_spi_fast_fn)(void *,uint32_t);
/* Defined in SLEH's actual translation unit, never in an independent driver
 * which merely claims CPU-summary delivery. Zero=success except dispatch:
 * dispatch returns1 for a genuinely leased SPI,0 to preserve foreign path. */
int dre_spi_summary_install(uint32_t,void *,dre_spi_dispatch_fn,dre_spi_fast_fn,uint64_t *);
int dre_spi_summary_close(uint32_t,uint64_t);
int dre_spi_summary_remove(uint32_t,uint64_t);
int dre_spi_summary_live(uint32_t,uint64_t);
uint32_t dre_spi_summary_inflight(uint32_t,uint64_t);
int dre_spi_summary_dispatch(uint32_t);
/* Defined in pe_fiq.c using its already initialized real GIC mapping. bind is
 * called only at the existing DT-to-ml_io_map site, not by an IOKit caller. */
void dre_spi_pe_bind(uint64_t,uint64_t,uint64_t,uint64_t);
int dre_spi_pe_read(uint32_t,struct dre_spi_hw *);
int dre_spi_pe_enable(uint32_t,uint64_t,int);
uint64_t dre_spi_counter(void);
uint64_t dre_spi_counter_frequency(void);
#ifdef __cplusplus
}
#endif
#endif
