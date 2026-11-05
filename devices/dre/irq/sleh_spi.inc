/* SPDX-License-Identifier: MIT
 * UNVALIDATED. Include near selected arm64/sleh.c headers. The opt-in hook
 * below is the only consumer of this registry. No CPU handler is replaced.
 */
#include "spi_low.h"
#if defined(DRE) && HAS_GIC_V3
#define DRE_SPI_SLOT_ACTIVE UINT32_C(0x40000000)
#define DRE_SPI_SLOT_CLOSING UINT32_C(0x80000000)
#define DRE_SPI_SLOT_REFS UINT32_C(0x3fffffff)
struct dre_spi_summary_slot {
    uint32_t gate;
    uint64_t token;
    void *cookie;
    dre_spi_dispatch_fn dispatch;
    dre_spi_fast_fn fast;
};
/* Fixed slots remain alive for the kernel lifetime. A single CAS gate joins
 * readers before cookie access; no load-pointer/increment-after-free race. */
static struct dre_spi_summary_slot dre_spi_slots[1020];
static uint64_t dre_spi_next_token=1;
static int dre_spi_slot_get(struct dre_spi_summary_slot *s)
{
    uint32_t gate=__atomic_load_n(&s->gate,__ATOMIC_ACQUIRE);
    for(unsigned retry=0;retry<32;retry++) {
        if(!(gate&DRE_SPI_SLOT_ACTIVE)||(gate&DRE_SPI_SLOT_CLOSING)||
           (gate&DRE_SPI_SLOT_REFS)==DRE_SPI_SLOT_REFS)return 0;
        uint32_t next=gate+1;
        if(__atomic_compare_exchange_n(&s->gate,&gate,next,0,
              __ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;
    }
    return 0;
}
static void dre_spi_slot_put(struct dre_spi_summary_slot *s)
{ __atomic_fetch_sub(&s->gate,1,__ATOMIC_RELEASE); }
int dre_spi_summary_install(uint32_t id,void *cookie,dre_spi_dispatch_fn dispatch,
                            dre_spi_fast_fn fast,uint64_t *token)
{
    if(token)*token=0;
    if(id<32||id>=1020||!cookie||!dispatch||!fast||!token)return DRE_SPI_ARGUMENT;
    struct dre_spi_summary_slot *s=&dre_spi_slots[id];uint32_t empty=0;
    if(!__atomic_compare_exchange_n(&s->gate,&empty,DRE_SPI_SLOT_CLOSING,0,
          __ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return DRE_SPI_BUSY;
    uint64_t value=__atomic_load_n(&dre_spi_next_token,__ATOMIC_RELAXED);
    unsigned tries=0;
    while(value&&value<UINT64_MAX&&tries++<1024) {
        uint64_t next=value+1;
        if(__atomic_compare_exchange_n(&dre_spi_next_token,&value,next,0,
              __ATOMIC_RELAXED,__ATOMIC_RELAXED))break;
    }
    if(!value||value==UINT64_MAX||tries>1024) {
        /* Saturating counter: no wrap can resurrect an earlier route token. */
        __atomic_store_n(&s->gate,0,__ATOMIC_RELEASE);return DRE_SPI_LOST;
    }
    s->token=value;s->cookie=cookie;s->dispatch=dispatch;s->fast=fast;
    *token=value;
    __atomic_store_n(&s->gate,DRE_SPI_SLOT_ACTIVE,__ATOMIC_RELEASE);
    return DRE_SPI_OK;
}
int dre_spi_summary_close(uint32_t id,uint64_t token)
{
    if(id<32||id>=1020||!token)return DRE_SPI_ARGUMENT;
    struct dre_spi_summary_slot *s=&dre_spi_slots[id];
    uint32_t gate=__atomic_load_n(&s->gate,__ATOMIC_ACQUIRE);
    if(!(gate&DRE_SPI_SLOT_ACTIVE)||s->token!=token)return DRE_SPI_LOST;
    __atomic_fetch_or(&s->gate,DRE_SPI_SLOT_CLOSING,__ATOMIC_ACQ_REL);
    return DRE_SPI_OK;
}
int dre_spi_summary_remove(uint32_t id,uint64_t token)
{
    if(id<32||id>=1020||!token)return DRE_SPI_ARGUMENT;
    struct dre_spi_summary_slot *s=&dre_spi_slots[id];
    uint32_t expected=DRE_SPI_SLOT_ACTIVE|DRE_SPI_SLOT_CLOSING;
    if(__atomic_load_n(&s->gate,__ATOMIC_ACQUIRE)!=expected||s->token!=token)
        return DRE_SPI_BUSY;
    /* All install/remove callers are serialized by the actual board broker
     * mutex. Closing blocks new readers; all earlier cookie users are joined. */
    s->token=0;s->cookie=0;s->dispatch=0;s->fast=0;
    __atomic_store_n(&s->gate,0,__ATOMIC_RELEASE);
    return DRE_SPI_OK;
}
int dre_spi_summary_live(uint32_t id,uint64_t token)
{
    if(id<32||id>=1020||!token)return DRE_SPI_ARGUMENT;
    struct dre_spi_summary_slot *s=&dre_spi_slots[id];
    if(!dre_spi_slot_get(s))return DRE_SPI_LOST;
    int status=s->token==token&&s->fast&&s->fast(s->cookie,id)==0?
        DRE_SPI_OK:DRE_SPI_LOST;
    dre_spi_slot_put(s);return status;
}
uint32_t dre_spi_summary_inflight(uint32_t id,uint64_t token)
{
    if(id<32||id>=1020||!token)return UINT32_MAX;
    struct dre_spi_summary_slot *s=&dre_spi_slots[id];
    uint32_t gate=__atomic_load_n(&s->gate,__ATOMIC_ACQUIRE);
    if(!(gate&DRE_SPI_SLOT_ACTIVE)||s->token!=token)return UINT32_MAX;
    return gate&DRE_SPI_SLOT_REFS;
}
int dre_spi_summary_dispatch(uint32_t id)
{
    if(id<32||id>=1020)return 0;
    struct dre_spi_summary_slot *s=&dre_spi_slots[id];
    uint32_t gate=__atomic_load_n(&s->gate,__ATOMIC_ACQUIRE);
    if(!(gate&DRE_SPI_SLOT_ACTIVE))return 0;
    /* A closing route still owns this delivery until masked, ACTIVE=0 and
     * removal. Never redirect it to an unrelated existing CPU handler. */
    if(!dre_spi_slot_get(s))return 1;
    if(s->fast&&s->fast(s->cookie,id)==0&&s->dispatch)s->dispatch(s->cookie,id);
    dre_spi_slot_put(s);
    return 1; /* Existing sleh_irq executes its unchanged EOIR1 exactly once. */
}
#endif
