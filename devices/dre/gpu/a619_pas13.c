/* SPDX-License-Identifier: MIT
 * PAS-13/SMCCC protocol from qcom_scm-smc.c/qcom_scm.h/mdt_loader.c at
 * LineageOS 98a38783f42f5588bf00cd933e299aedc589bc10. Independently implemented
 * guarded transport; no Qualcomm/Apple firmware, certificates or keys. */
#include "a619_pas13.h"
#define PAS_INIT 1u
#define PAS_MEMORY 2u
#define PAS_AUTH 5u
#define PAS_SHUTDOWN 6u
#define PAS_SUPPORTED 7u
#define SMC_INTERRUPTED INT64_C(1)
#define SMC_BUSY INT64_C(-12)
#define SMC_BASE UINT64_C(0x42000200) /* standard, 64-bit, SiP owner2, PIL svc2 */
static int equal(const char *a,const char *b)
{
    while (*a && *a==*b) { a++; b++; }
    return *a==*b;
}
static int valid(const struct dre_a619_pas13 *s,const struct dre_a619_pas13_ops *o)
{ return s && o && o->guard && o->smc64 && o->barrier && o->now; }
static int cpu_range(const void *p,size_t n)
{ return p && n && (uintptr_t)p<=UINTPTR_MAX-n; }
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{ return n && m && a<b+m && b<a+n; }
static int memory_valid(const struct dre_a619_pas13 *s)
{
    if (!cpu_range(s->metadata_cpu,s->metadata_capacity) ||
        !cpu_range(s->reserved_cpu,s->reserved_bytes) ||
        !cpu_range(s->firmware,s->firmware_bytes) ||
        ((uintptr_t)s->metadata_cpu&4095) || ((uintptr_t)s->reserved_cpu&4095) ||
        !s->metadata_pa || (s->metadata_pa&4095) ||
        !s->metadata_capacity || (s->metadata_capacity&4095) ||
        s->metadata_capacity>DRE_A619_ZAP_MAX_METADATA || s->metadata_pa>UINT32_MAX ||
        s->metadata_capacity>UINT64_C(0x100000000)-s->metadata_pa) return 0;
    return !overlap(s->metadata_pa,s->metadata_capacity,s->reserved_pa,s->reserved_bytes) &&
        !overlap((uintptr_t)s->metadata_cpu,s->metadata_capacity,(uintptr_t)s->reserved_cpu,s->reserved_bytes) &&
        !overlap((uintptr_t)s->firmware,s->firmware_bytes,(uintptr_t)s->metadata_cpu,s->metadata_capacity) &&
        !overlap((uintptr_t)s->firmware,s->firmware_bytes,(uintptr_t)s->reserved_cpu,s->reserved_bytes) &&
        !overlap((uintptr_t)s,sizeof(*s),(uintptr_t)s->metadata_cpu,s->metadata_capacity) &&
        !overlap((uintptr_t)s,sizeof(*s),(uintptr_t)s->reserved_cpu,s->reserved_bytes) &&
        !overlap((uintptr_t)s,sizeof(*s),(uintptr_t)s->firmware,s->firmware_bytes);
}
/* Returns 1 only for a completed SMC with transport status0. Service result
 * stays in state; query-supported legitimately returns1 instead of0. */
static int call(struct dre_a619_pas13 *s,const struct dre_a619_pas13_ops *o,
    uint32_t command,uint64_t a,uint64_t b)
{
    uint64_t args=command==PAS_INIT ? 0x82 : command==PAS_MEMORY ? 3 : 1;
    uint64_t start=o->now(o->cookie);
    s->last_command=command;
    for (unsigned retry=0;retry<=20;retry++) {
        uint64_t first=SMC_BASE|command,continuation=0;
        struct dre_a619_smc_regs r={{0}};
        for (unsigned interrupted=0;;interrupted++) {
            if (!o->guard(o->cookie)) { s->unsafe=1; return 0; }
            r=(struct dre_a619_smc_regs){{first,args,13,a,b,0,continuation,0}};
            if (!o->smc64(o->cookie,&r)) { s->unsafe=1; return 0; }
            s->transport_status=(int64_t)r.x[0]; s->service_status=(int64_t)r.x[1];
            if (!o->guard(o->cookie)) { s->unsafe=1; return 0; }
            if (s->transport_status!=SMC_INTERRUPTED) break;
            if (interrupted==63 || o->now(o->cookie)-start>=19200000) {
                s->unsafe=1; return 0; /* unresolved secure continuation */
            }
            first=SMC_INTERRUPTED; continuation=r.x[6];
        }
        if (s->transport_status!=SMC_BUSY) return s->transport_status==0;
        if (retry==20 || o->now(o->cookie)-start>=19200000) return 0;
        uint64_t delay=o->now(o->cookie);
        unsigned polls=0;
        while (o->now(o->cookie)-delay<576000) { /* pinned SCM30ms busy backoff */
            if (!o->guard(o->cookie)) { s->unsafe=1; return 0; }
            if (++polls==1000000) return 0;
        }
    }
    return 0;
}
static const char *cleanup(struct dre_a619_pas13 *s,const struct dre_a619_pas13_ops *o,
    const char *failure)
{
    if (s->unsafe) return "PAS_RESET_REQUIRED";
    if (!call(s,o,PAS_SHUTDOWN,0,0) || s->service_status) {
        s->unsafe=1; return "PAS_RESET_REQUIRED";
    }
    s->initialized=0; s->authenticated=0;
    return failure;
}
const char *dre_a619_pas13_authenticate(struct dre_a619_pas13 *s,
    const struct dre_a619_pas13_ops *o)
{
    if (!valid(s,o)) return "INVALID_ARGUMENT";
    if (s->unsafe) return "PAS_RESET_REQUIRED";
    if (s->chip_id!=DRE_A619_CHIP_ID) return "WRONG_GPU";
    if (s->prerequisites!=DRE_A619_PAS13_PREREQUISITES) return "PAS_PLATFORM_UNAVAILABLE";
    if (s->tick_hz!=19200000) return "UNSUPPORTED_TIMER";
    if (s->initialized || s->authenticated) return "PAS_ALREADY_ACTIVE";
    if (!o->guard(o->cookie)) return "PAS_OWNERSHIP_REJECTED";
    if (!memory_valid(s) ||
        overlap((uintptr_t)o,sizeof(*o),(uintptr_t)s->metadata_cpu,s->metadata_capacity) ||
        overlap((uintptr_t)o,sizeof(*o),(uintptr_t)s->reserved_cpu,s->reserved_bytes))
        return "PAS_MEMORY_REJECTED";
    struct dre_a619_zap_plan p;
    const char *status=dre_a619_zap_plan(s->firmware,s->firmware_bytes,s->reserved_pa,s->reserved_bytes,&p);
    if (!equal(status,"ZAP_PLAN_OK")) return status;
    if ((size_t)p.metadata_header_bytes+p.metadata_hash_bytes>s->metadata_capacity) return "PAS_METADATA_TOO_SMALL";
    if (!call(s,o,PAS_SUPPORTED,0,0)) return s->unsafe ? "PAS_RESET_REQUIRED" : "PAS_QUERY_FAILED";
    if (s->service_status!=1) return "PAS13_UNSUPPORTED";
    if (!o->guard(o->cookie)) { s->unsafe=1; return "PAS_RESET_REQUIRED"; }
    status=dre_a619_zap_metadata(s->firmware,s->firmware_bytes,s->reserved_pa,s->reserved_bytes,
        s->metadata_cpu,s->metadata_capacity,&s->metadata_bytes);
    if (!equal(status,"ZAP_METADATA_OK")) return status;
    o->barrier(o->cookie);
    /* Treat even a failed init-image as potentially stateful. */
    s->initialized=1;
    if (!call(s,o,PAS_INIT,s->metadata_pa,0) || s->service_status)
        return cleanup(s,o,"PAS_INIT_REJECTED");
    if (!call(s,o,PAS_MEMORY,s->reserved_pa,p.memory_bytes) || s->service_status)
        return cleanup(s,o,"PAS_MEMORY_SETUP_REJECTED");
    if (!o->guard(o->cookie)) { s->unsafe=1; return "PAS_RESET_REQUIRED"; }
    status=dre_a619_zap_copy_segments(s->firmware,s->firmware_bytes,s->reserved_pa,s->reserved_cpu,s->reserved_bytes);
    if (!equal(status,"ZAP_SEGMENTS_COPIED")) return cleanup(s,o,status);
    o->barrier(o->cookie);
    if (!call(s,o,PAS_AUTH,0,0) || s->service_status)
        return cleanup(s,o,"PAS_AUTH_REJECTED");
    s->authenticated=1;
    return "PAS13_AUTHENTICATED";
}
const char *dre_a619_pas13_shutdown(struct dre_a619_pas13 *s,const struct dre_a619_pas13_ops *o)
{
    if (!valid(s,o)) return "INVALID_ARGUMENT";
    if (s->unsafe) return "PAS_RESET_REQUIRED";
    if (!s->initialized && !s->authenticated) return "PAS_NOT_OWNED";
    if (s->chip_id!=DRE_A619_CHIP_ID || s->prerequisites!=DRE_A619_PAS13_PREREQUISITES ||
        s->tick_hz!=19200000) { s->unsafe=1; return "PAS_RESET_REQUIRED"; }
    return cleanup(s,o,"PAS13_SHUTDOWN");
}
