/* SPDX-License-Identifier: MIT -- early-boot availability observations ONLY. */
#ifndef DRE_GPU_SCM_PROBE_H
#define DRE_GPU_SCM_PROBE_H
#include <stdint.h>
#include <stddef.h>
#include "../a619_pas13.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRE_SCM_PROBE_VERSION 1u
#define DRE_SCM_PROBE_QUERIES 6u
enum dre_scm_probe_status {
    DRE_SCM_PROBE_NOT_RUN=0,DRE_SCM_PROBE_OBSERVED,DRE_SCM_PROBE_UNSUPPORTED,
    DRE_SCM_PROBE_PARTIAL,DRE_SCM_PROBE_ARGUMENT,DRE_SCM_PROBE_BOARD,
    DRE_SCM_PROBE_PROFILE,DRE_SCM_PROBE_CONTEXT,DRE_SCM_PROBE_CONTEXT_CHANGED,
    DRE_SCM_PROBE_TRANSPORT,DRE_SCM_PROBE_CONTINUATION_LIMIT,DRE_SCM_PROBE_BUSY_LIMIT,
    DRE_SCM_PROBE_CLOCK_STALLED,DRE_SCM_PROBE_ALREADY_RUN,DRE_SCM_PROBE_ARCH_UNAVAILABLE
};
enum dre_scm_probe_reply {
    DRE_SCM_REPLY_NOT_QUERIED=0,DRE_SCM_REPLY_AVAILABLE,DRE_SCM_REPLY_UNAVAILABLE,
    DRE_SCM_REPLY_ERROR,DRE_SCM_REPLY_BUSY_EXHAUSTED,DRE_SCM_REPLY_UNCERTAIN
};
struct dre_scm_probe_context { uint64_t current_el,daif,sctlr,frequency,mpidr,counter; };
struct dre_scm_probe_reply_record {
    uint64_t function,target;
    int64_t transport,service;
    uint32_t state,calls,busy,interruptions;
};
struct dre_scm_probe_result {
    uint32_t version,struct_bytes,status,query_count;
    uint32_t available_mask,unavailable_mask,uncertain,boot_must_stop;
    struct dre_scm_probe_context initial;
    struct dre_scm_probe_reply_record replies[DRE_SCM_PROBE_QUERIES];
};
/* Raw observation/transport boundary for synthetic fixtures. No policy,
 * ownership, power-ready or authentication callback is accepted. Both return
 * EXACT1 only on successful raw read/instruction transport, never EL3 success.
 * Runtime callers must use fixed _early entry, not substitute these inputs. */
struct dre_scm_probe_io {
    void *cookie;
    int (*read_context)(void *,struct dre_scm_probe_context *);
    int (*smc64)(void *,struct dre_a619_smc_regs *);
};
enum dre_scm_probe_status dre_gpu_scm_probe_with_io(const void *original_fdt,size_t,
    const struct dre_scm_probe_io *,struct dre_scm_probe_result *);
/* Actual shim-only one-shot, BEFORE XNU/SMP/interrupt enable. Reads architectural
 * state and emits only FAST64 INFO_IS_CALL_AVAIL (including continuations).
 * Never invokes PAS13, loads firmware, writes clocks/reset/XPU/GIC or acquires
 * policy. Mask/EL/MMU/counter/known-board/profile mismatches refuse.
 * boot_must_stop MUST prevent all later SMCs and kernel handoff on uncertainty;
 * print the observation and remain in diagnostic state instead. Availability
 * masks refer to callable functions, not PAS13 permission or authentication. */
const char *dre_gpu_scm_probe_early(const void *original_fdt,size_t,struct dre_scm_probe_result *);
const char *dre_gpu_scm_probe_status_name(enum dre_scm_probe_status);
#ifdef __cplusplus
}
#endif
#endif
