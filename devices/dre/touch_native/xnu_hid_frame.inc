/* SPDX-License-Identifier: MIT
 * UNVALIDATED opt-in source fragment. See README.md integration recipe.
 * Include ONLY inside SweetHID.cpp, after SweetHIDTouch and its old methods.
 * Uses the existing open IOHIDEventService ABI in patches0058/0061/0071; this
 * does not assert that stock iOS/backboardd accepts the resulting events yet.
 */
#include "input.h"

bool
SweetHIDTouch::dreNativeFrame(const struct dre_tn_hid_frame *frame)
{
    if (!frame || frame->version != 1 || !frame->generation ||
        frame->active_mask & ~0x3ffu || frame->cancel_reason > DRE_TN_STOPPED_BY_OWNER ||
        (frame->cancel_reason && frame->active_mask) || isInactive()) return false;
    if (!fLock || !_clientLock || !handleIsOpen(NULL)) return false;
    for (unsigned i = 0; i != 10; ++i) {
        if (!(frame->active_mask & (1u << i))) continue;
        if (frame->contacts[i].x > 1079 || frame->contacts[i].y > 2399 ||
            !frame->contacts[i].pressure || frame->contacts[i].pressure > 1000)
            return false;
    }
    IOLockLock(fLock);
    if (isInactive() || !handleIsOpen(NULL)) {
        IOLockUnlock(fLock);
        return false;
    }
    IOFixed nx[10], ny[10];
    bool edge[10];
    for (unsigned i = 0; i != 10; ++i) {
        bool down = (frame->active_mask & (1u << i)) != 0;
        nx[i] = fX[i]; ny[i] = fY[i]; edge[i] = fEdge[i];
        if (!down) continue;
        int x = frame->contacts[i].x, y = frame->contacts[i].y;
        /* Same physical-panel -> presented-display transform as patch0061.
         * No second crop/scale is applied by the portable input pipeline. */
        nx[i] = (IOFixed)(((int64_t)(x + fOffX) << 16) / fLogicalW);
        ny[i] = (IOFixed)(((int64_t)(y + fOffY) << 16) / fLogicalH);
        if (!fDown[i]) edge[i] = x < DRE_TOUCH_EDGE_PX ||
            x > 1079 - DRE_TOUCH_EDGE_PX || y < DRE_TOUCH_EDGE_PX ||
            y > 2399 - DRE_TOUCH_EDGE_PX;
    }
    IOHIDEvent *collection = IOHIDEvent::digitizerEvent(frame->timestamp, 0,
        kIOHIDDigitizerTransducerTypeHand, false, 0, 0, 0);
    if (!collection) { IOLockUnlock(fLock); return false; }
    collection->setIntegerValue(kIOHIDEventFieldDigitizerCollection, TRUE);
    unsigned children = 0;
    UInt32 collectionMask = 0;
    IOFixed centerX = 0, centerY = 0;
    bool haveCenter = false;
    for (unsigned i = 0; i != 10; ++i) {
        bool down = (frame->active_mask & (1u << i)) != 0;
        if (!down && !fDown[i]) continue;
        IOHIDEvent *child = IOHIDEvent::digitizerEvent(frame->timestamp, i + 2,
            kIOHIDDigitizerTransducerTypeFinger, down, down ? 1 : 0, nx[i], ny[i]);
        if (!child) {
            collection->release(); IOLockUnlock(fLock); return false;
        }
        UInt32 mask = edge[i] ? kIOHIDDigitizerEventFromEdgeTip : 0;
        if (down != fDown[i]) mask |= kIOHIDDigitizerEventTouch | kIOHIDDigitizerEventRange;
        if (down && (nx[i] != fX[i] || ny[i] != fY[i])) mask |= kIOHIDDigitizerEventPosition;
        if (down && !fDown[i]) mask |= kIOHIDDigitizerEventIdentity;
        if (down) mask |= kIOHIDDigitizerEventAttribute;
        if (frame->cancel_reason) mask |= kIOHIDDigitizerEventCancel;
        child->setIntegerValue(kIOHIDEventFieldDigitizerIndex, i);
        child->setIntegerValue(kIOHIDEventFieldDigitizerIdentity, i + 2);
        child->setIntegerValue(kIOHIDEventFieldDigitizerTouch, down);
        child->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, mask);
        child->setFixedValue(kIOHIDEventFieldDigitizerPressure,
            down ? (IOFixed)(((uint32_t)frame->contacts[i].pressure << 16) / 1000) : 0);
        collection->appendChild(child);
        child->release();
        ++children;
        /* appendChild is void in the shipped implementation; detect its
         * backing OSArray allocation failure before committing any state. */
        if (!collection->getChildren() || collection->getChildren()->getCount() != children) {
            collection->release(); IOLockUnlock(fLock); return false;
        }
        if (!haveCenter || down) { centerX = nx[i]; centerY = ny[i]; haveCenter = true; }
        collectionMask |= mask;
    }
    collection->setFixedValue(kIOHIDEventFieldDigitizerX, centerX);
    collection->setFixedValue(kIOHIDEventFieldDigitizerY, centerY);
    collection->setIntegerValue(kIOHIDEventFieldDigitizerRange, frame->active_mask != 0);
    collection->setIntegerValue(kIOHIDEventFieldDigitizerTouch, frame->active_mask != 0);
    collection->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, collectionMask);
    collection->setIntegerValue(kIOHIDEventFieldDigitizerButtonMask, frame->active_mask ? 1 : 0);
    /* All ten contacts commit together, after every allocation succeeds. The
     * native binding serializes service stop and every touch producer on this
     * work loop; the old Sweet hardware driver must not also produce events. */
    for (unsigned i = 0; i != 10; ++i) {
        fDown[i] = (frame->active_mask & (1u << i)) != 0;
        fEdge[i] = edge[i]; fX[i] = nx[i]; fY[i] = ny[i];
    }
    IOLockUnlock(fLock);
    dispatchEvent(collection);
    collection->release();
    return true; /* Submission only; no userspace acknowledgement ABI exists. */
}

extern "C" int
dre_sweet_hid_submit_frame(void *retainedService, const struct dre_tn_hid_frame *frame)
{
    if (!retainedService) return 1;
    SweetHIDTouch *service = OSDynamicCast(SweetHIDTouch, (OSObject *)retainedService);
    return service && service->dreNativeFrame(frame) ? 0 : 1;
}
