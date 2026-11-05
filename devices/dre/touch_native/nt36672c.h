/* SPDX-License-Identifier: MIT -- UNVALIDATED overnight draft. */
#ifndef DRE_TOUCH_NT36672C_NATIVE_H
#define DRE_TOUCH_NT36672C_NATIVE_H
#include "gpi_spi.h"
#include "../touch/firmware.h"
#include "../touch/report.h"

enum dre_tn_controller_state { DRE_TN_CONTROLLER_OFF, DRE_TN_CONTROLLER_BOOTING,
    DRE_TN_CONTROLLER_READY, DRE_TN_CONTROLLER_FAULT };
struct dre_tn_controller {
    struct dre_tn_spi *spi;
    unsigned state, watchdog_count, generation;
    uint32_t firmware_version, crc_result[2];
    uint8_t crc_flags, reset_state;
    enum dre_touch_result last_decode;
    enum dre_tn_result last_error;
};
typedef int (*dre_tn_sha256_fn)(void *, const uint8_t *, size_t, uint8_t digest[32]);

/* The caller holds the panel/reset lifecycle and supplies an immutable private
 * firmware buffer throughout this synchronous call. Digest is computed over
 * the actual bytes here, not accepted as a precomputed ready flag. First boot
 * only; recovery requires a native input cancellation + fresh explicit handle.
 * No engineering reset/display-off fallback or arbitrary register API exists.
 */
enum dre_tn_result dre_tn_controller_boot(struct dre_tn_controller *,
    struct dre_tn_spi *, const uint8_t *firmware, size_t bytes,
    dre_tn_sha256_fn sha256, void *sha_cookie);
/* A rejected report leaves out unchanged. A valid empty controller report is
 * distinct from a firmware-reset marker, malformed packet or absent service. */
enum dre_tn_result dre_tn_controller_report(struct dre_tn_controller *,
                                            struct dre_touch_frame *out);
#endif
