/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight draft. */
#ifndef DRE_TOUCH_NATIVE_INPUT_H
#define DRE_TOUCH_NATIVE_INPUT_H
#include "nt36672c.h"
#ifdef __cplusplus
extern "C" {
#endif

enum dre_tn_cancel_reason { DRE_TN_NOT_CANCELLED, DRE_TN_DEVICE_LOST,
                            DRE_TN_STOPPED_BY_OWNER };
struct dre_tn_hid_contact { uint16_t x, y, pressure, width; };
struct dre_tn_hid_frame {
    uint32_t version, active_mask, generation, cancel_reason;
    uint64_t timestamp; /* Real mach_absolute_time from the binding. */
    struct dre_tn_hid_contact contacts[10]; /* ID-1, raw physical panel pixels. */
};
/* Optional source recipe supplies this ABI; the current kernel does not. A
 * zero return means dispatchEvent was called with existing HID clients. It
 * cannot prove that backboardd consumed or acknowledged the collection. The
 * binding obtains and retains the real SweetHIDTouch IOService, serializes its
 * stop against this callback, and passes that retained service as the cookie.
 * There is deliberately no unretained global gSweetTouch lookup here. */
int dre_sweet_hid_submit_frame(void *retained_service, const struct dre_tn_hid_frame *);

struct dre_tn_input_ops {
    void *cookie;
    /* Hard-IRQ-safe, leased TLMM22 only; no sleeping/synchronous drain here. */
    int (*mask_gpio_fast)(void *, void *lease);
    /* Coalesces one worker, native scheduler keeps input/lease alive. delay_ms
     * is0 from IRQ,10 for a service retry. Delivery-only scheduling does not
     * set the input's independent pending-report mark. Never runs inline. */
    int (*queue_worker)(void *, unsigned delay_ms);
    int (*sample_gpio)(void *, void *lease, unsigned *level);
    /* Clear owned TLMM pending, arm the qualified falling-edge IRQ, then
     * sample/recheck pending under the IRQ lock. If a report is already low
     * or an edge raced, leave masked and set needs_worker=1. No invented IRQ. */
    int (*rearm_gpio)(void *, void *lease, unsigned *needs_worker);
    uint64_t (*absolute_time)(void *);
    /* Transactional complete-frame submission. ONLY DRE_TN_OK means the
     * retained service accepted the whole batch and committed its contacts.
     * A failure must leave its preceding contact state unchanged. Dispatch
     * still supplies no backboardd/userspace acknowledgement. */
    enum dre_tn_result (*submit)(void *, const struct dre_tn_hid_frame *);
    /* External teardown only: mask+join IRQ AND cancel/join queued workers.
     * Must not call this synchronously from the worker it would join. */
    int (*quiesce)(void *, void *lease);
};
struct dre_tn_input {
    struct dre_tn_controller *controller;
    struct dre_tn_input_ops ops;
    struct dre_tn_hid_frame submitted, held;
    uint32_t pending, worker_active, stopping, has_held;
    /* Worker-owned terminal stream-loss latch. Never cleared to recover this
     * input object. A fresh controller/input lifecycle and genuine boot
     * generation are required; held pre-loss reports are never replayed.
     * No public HID-frame ABI field or hardware-ready fact is added. */
    uint32_t device_lost;
    uint64_t valid_reports, rejected_reports, submitted_frames;
};
enum dre_tn_result dre_tn_input_start(struct dre_tn_input *, struct dre_tn_controller *,
                                      const struct dre_tn_input_ops *);
/* Native GPIO IRQ trampoline calls this; no SPI/DMA/event allocation in IRQ. */
enum dre_tn_result dre_tn_input_irq(struct dre_tn_input *);
/* ONE sleepable native worker, with the controller and GPI lease held.
 * After a confirmed fault, source ownership failure, non-running transport or
 * generation mismatch it performs no further report read/IRQ rearm. Only an
 * explicit all-up cancellation of previously submitted contacts may retry,
 * at most once per worker invocation; failed acceptance retains that state.
 */
enum dre_tn_result dre_tn_input_worker(struct dre_tn_input *);
/* Actual outer IRQ/source-owner loss notification, not a public touch command.
 * Same sleepable serialized worker/owner policy as input_worker. Refuses a
 * stopping/concurrent worker, latches terminal loss and attempts at most one
 * cancellation batch of previously accepted contacts. Failed acceptance is
 * retained and may schedule one existing10ms delivery retry. It performs no
 * GPIO/SPI/IRQ/quiesce/reset/MMIO operation and changes no controller state.
 * Even accepted all-up returns RECOVERY, never healthy input. Caller retains
 * actual source-fault evidence and must not use this as a simulated event.
 */
enum dre_tn_result dre_tn_input_source_lost(struct dre_tn_input *);
/* External, sleepable owner. Failed cancellation retains submitted state for
 * a subsequent explicit stop retry; it never fabricates a controller release. */
enum dre_tn_result dre_tn_input_stop(struct dre_tn_input *);
#ifdef __cplusplus
}
#endif
#endif
