/* SPDX-License-Identifier: MIT
 * UNVALIDATED. GPIO bottom half -> real checksum-validated contacts -> HID.
 */
#include "input.h"

static void *lease(struct dre_tn_input *i) { return i->controller->spi->lease; }
static enum dre_tn_result queue(struct dre_tn_input *i, unsigned delay)
{
    __atomic_store_n(&i->pending, 1, __ATOMIC_RELEASE);
    return i->ops.queue_worker(i->ops.cookie, delay) ? DRE_TN_PROVIDER : DRE_TN_OK;
}
static enum dre_tn_result queue_delivery(struct dre_tn_input *i)
{
    /* A service retry is not a GPIO/report event. In particular, accepting
     * held HID delivery later must not force a stale SPI reread at HIGH GPIO.
     * Any real concurrent IRQ still sets the independent pending mark.
     */
    return i->ops.queue_worker(i->ops.cookie, 10) ? DRE_TN_PROVIDER : DRE_TN_OK;
}
static enum dre_tn_result cancel(struct dre_tn_input *i, unsigned reason)
{
    if (!i->submitted.active_mask) return DRE_TN_OK;
    struct dre_tn_hid_frame frame = i->submitted;
    frame.active_mask = 0;
    frame.cancel_reason = reason;
    frame.timestamp = i->ops.absolute_time(i->ops.cookie);
    enum dre_tn_result result = i->ops.submit(i->ops.cookie, &frame);
    if (!result) { i->submitted = frame; ++i->submitted_frames; }
    return result;
}
static int same_stream(const struct dre_tn_input *i)
{
    const struct dre_tn_controller *c = i->controller;
    return !i->device_lost && c->state == DRE_TN_CONTROLLER_READY && c->spi &&
        c->spi->state == DRE_TN_RUNNING && i->submitted.generation &&
        c->generation == i->submitted.generation;
}
static enum dre_tn_result lose_once(struct dre_tn_input *i)
{
    i->device_lost = 1;
    i->has_held = 0;
    enum dre_tn_result result = cancel(i, DRE_TN_DEVICE_LOST);
    return result == DRE_TN_OK ? DRE_TN_RECOVERY : result;
}
static enum dre_tn_result retry_lost_delivery(struct dre_tn_input *i, enum dre_tn_result result)
{
    if (!__atomic_load_n(&i->stopping, __ATOMIC_ACQUIRE) && i->submitted.active_mask &&
        (result == DRE_TN_INPUT || result == DRE_TN_PROVIDER)) {
        enum dre_tn_result queued = queue_delivery(i);
        if (queued) return queued;
    }
    return result;
}
enum dre_tn_result dre_tn_input_start(struct dre_tn_input *i,
    struct dre_tn_controller *controller, const struct dre_tn_input_ops *ops)
{
    if (!i || !controller || !controller->spi || !ops) return DRE_TN_ARGUMENT;
    if (i->controller || i->device_lost || !controller->generation ||
        controller->state != DRE_TN_CONTROLLER_READY ||
        controller->spi->state != DRE_TN_RUNNING) return DRE_TN_STATE;
    if (!ops->mask_gpio_fast || !ops->queue_worker || !ops->sample_gpio ||
        !ops->rearm_gpio || !ops->absolute_time || !ops->submit || !ops->quiesce)
        return DRE_TN_PROVIDER;
    *i = (struct dre_tn_input){.controller = controller, .ops = *ops};
    i->submitted.version = 1;
    i->submitted.generation = controller->generation;
    unsigned again = 0;
    if (ops->rearm_gpio(ops->cookie, lease(i), &again)) return DRE_TN_PROVIDER;
    return again ? queue(i, 0) : DRE_TN_OK;
}
enum dre_tn_result dre_tn_input_irq(struct dre_tn_input *i)
{
    if (!i || !i->controller) return DRE_TN_ARGUMENT;
    if (__atomic_load_n(&i->stopping, __ATOMIC_ACQUIRE)) return DRE_TN_STATE;
    if (i->ops.mask_gpio_fast(i->ops.cookie, lease(i))) return DRE_TN_OWNERSHIP;
    /* Binding performs the source-specific TLMM/GIC acknowledge/EOI. This
     * portable handler never acknowledges an unrelated interrupt or polls SPI. */
    return queue(i, 0);
}
enum dre_tn_result dre_tn_input_worker(struct dre_tn_input *i)
{
    if (!i || !i->controller) return DRE_TN_ARGUMENT;
    if (__atomic_load_n(&i->stopping, __ATOMIC_ACQUIRE)) return DRE_TN_STATE;
    if (__atomic_exchange_n(&i->worker_active, 1, __ATOMIC_ACQUIRE)) return DRE_TN_STATE;
    enum dre_tn_result result = DRE_TN_OK;
    unsigned pending = __atomic_exchange_n(&i->pending, 0, __ATOMIC_ACQ_REL);
    unsigned budget = 0;
    /* Do not replay an authentic but now stale held frame from a previous
     * controller/transport generation. Stream loss cannot reset IDs or make
     * newly booted hardware part of this existing submitted contact lifetime.
     */
    if (!same_stream(i)) goto lost;
    if (i->has_held) {
        if (i->held.generation != i->submitted.generation) goto lost;
        result = i->ops.submit(i->ops.cookie, &i->held);
        if (result) goto finish;
        i->submitted = i->held;
        ++i->submitted_frames;
        i->has_held = 0;
    }
    for (budget = 0; budget != 16; ++budget) {
        unsigned level;
        if (__atomic_load_n(&i->stopping, __ATOMIC_ACQUIRE)) break;
        if (!same_stream(i)) goto lost;
        if (i->ops.sample_gpio(i->ops.cookie, lease(i), &level) || level > 1) {
            goto lost;
        }
        if (level && !pending) break;
        pending = 0;
        struct dre_touch_frame report;
        result = dre_tn_controller_report(i->controller, &report);
        if (result) {
            ++i->rejected_reports;
            if (!same_stream(i)) goto lost;
            /* Bad frames/early watchdog markers don't become empty frames.
             * Only another real controller report can change active contacts. */
            if (result == DRE_TN_CHECKSUM || result == DRE_TN_RECOVERY ||
                result == DRE_TN_PROTOCOL) continue;
            break;
        }
        if (!same_stream(i)) goto lost;
        ++i->valid_reports;
        struct dre_tn_hid_frame frame = { .version = 1,
            .active_mask = report.active_mask, .generation = i->submitted.generation,
            .timestamp = i->ops.absolute_time(i->ops.cookie) };
        for (unsigned c = 0; c != 10; ++c) {
            frame.contacts[c].x = report.contacts[c].x;
            frame.contacts[c].y = report.contacts[c].y;
            frame.contacts[c].pressure = report.contacts[c].pressure;
            frame.contacts[c].width = report.contacts[c].width;
        }
        result = i->ops.submit(i->ops.cookie, &frame);
        if (result) {
            /* Keep the most recent genuine report while HID is unavailable.
             * No success is logged and no prior submitted contacts are lost. */
            i->held = frame;
            i->has_held = 1;
            break;
        }
        i->submitted = frame;
        ++i->submitted_frames;
    }
    goto finish;
lost:
    /* This all-up is explicitly a cancellation of this producer's previously
     * accepted contacts, not a fabricated successful controller report. A
     * held frame was never accepted, so it must not reappear after the loss.
     * One cancellation attempt per invocation; no SPI/reset/panel operation.
     */
    result = lose_once(i);
finish:
    if (!__atomic_load_n(&i->stopping, __ATOMIC_ACQUIRE) && i->device_lost) {
        /* A faulted controller can no longer raise a useful report IRQ. The
         * previously submitted active mask remains the pending release until
         * the real HID batch is accepted. Retry only delivery, never transport
         * or IRQ ownership, and never log a failed cancellation as success.
         */
        result = retry_lost_delivery(i, result);
    } else if (!__atomic_load_n(&i->stopping, __ATOMIC_ACQUIRE) &&
        i->controller->state == DRE_TN_CONTROLLER_READY) {
        if (result == DRE_TN_INPUT || result == DRE_TN_PROVIDER) {
            enum dre_tn_result queued = queue_delivery(i);
            if (queued) result = queued;
        } else if (result == DRE_TN_OK || result == DRE_TN_CHECKSUM ||
                   result == DRE_TN_PROTOCOL || result == DRE_TN_RECOVERY) {
            unsigned again = budget == 16;
            if (!again && i->ops.rearm_gpio(i->ops.cookie, lease(i), &again))
                goto lost;
            else if (again) result = queue(i, budget == 16 ? 10 : 0);
        }
    }
    __atomic_store_n(&i->worker_active, 0, __ATOMIC_RELEASE);
    return result;
}
enum dre_tn_result dre_tn_input_source_lost(struct dre_tn_input *i)
{
    if (!i || !i->controller) return DRE_TN_ARGUMENT;
    if (__atomic_load_n(&i->stopping, __ATOMIC_ACQUIRE)) return DRE_TN_STATE;
    if (__atomic_exchange_n(&i->worker_active, 1, __ATOMIC_ACQUIRE)) return DRE_TN_STATE;
    (void)__atomic_exchange_n(&i->pending, 0, __ATOMIC_ACQ_REL);
    /* Actual outer IRQ-owner fault has already established source loss.
     * Do not ask an invalid GPIO route or transport to confirm it by MMIO,
     * and do not pretend that source loss is a controller firmware fault.
     */
    enum dre_tn_result result = lose_once(i);
    result = retry_lost_delivery(i, result);
    __atomic_store_n(&i->worker_active, 0, __ATOMIC_RELEASE);
    return result;
}

enum dre_tn_result dre_tn_input_stop(struct dre_tn_input *i)
{
    if (!i || !i->controller) return DRE_TN_ARGUMENT;
    __atomic_store_n(&i->stopping, 1, __ATOMIC_RELEASE);
    if (i->ops.quiesce(i->ops.cookie, lease(i))) return DRE_TN_OWNERSHIP;
    if (__atomic_load_n(&i->worker_active, __ATOMIC_ACQUIRE)) return DRE_TN_STATE;
    i->has_held = 0; /* Never deliver a queued pre-stop contact after teardown. */
    return cancel(i, DRE_TN_STOPPED_BY_OWNER);
}
