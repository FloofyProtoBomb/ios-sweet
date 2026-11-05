/* SPDX-License-Identifier: MIT
 * Read-only early shim diagnostic. Pinned98a3878 __query_convention invokes
 * this FAST64 INFO query with IRQs masked, before optional SCM clocks. For
 * target queries only its INFO argument changes; an unsupported fast-target
 * reply is recorded, never replaced by a standard/PAS call or inferred grant.
 */
#include "scm_probe.h"
#include "scm_native.h"
#include <libfdt.h>
#define INFO_FAST64 UINT64_C(0xc2000601)
#define AFFINITY_MASK UINT64_C(0xff00ffffff)
static const uint64_t targets[DRE_SCM_PROBE_QUERIES]={
    UINT64_C(0x02000601),UINT64_C(0x02000201),UINT64_C(0x02000202),
    UINT64_C(0x02000205),UINT64_C(0x02000206),UINT64_C(0x02000207)};
static int known_board(const void *dt)
{
    int n;const fdt32_t *ids=fdt_getprop(dt,0,"oplus,dtsi_no",&n);
    if(!ids||n<4||n%4)return 0;
    for(int i=0;i<n/4;i++)if(fdt32_to_cpu(ids[i])==133144||fdt32_to_cpu(ids[i])==133180)return 1;
    return 0;
}
static int valid_context(const struct dre_scm_probe_context *c)
{
    return c->current_el==4&&(c->daif&UINT64_C(0x3c0))==UINT64_C(0x3c0)&&
        !(c->sctlr&5)&&c->frequency==19200000&&!(c->mpidr&AFFINITY_MASK);
}
static int context(const struct dre_scm_probe_io *io,struct dre_scm_probe_result *out,
    struct dre_scm_probe_context *last)
{
    struct dre_scm_probe_context next={0};
    if(io->read_context(io->cookie,&next)!=1||!valid_context(&next)||
       next.current_el!=out->initial.current_el||next.daif!=out->initial.daif||
       next.sctlr!=out->initial.sctlr||next.frequency!=out->initial.frequency||
       next.mpidr!=out->initial.mpidr||next.counter<last->counter)return 0;
    *last=next;return 1;
}
static enum dre_scm_probe_status failure(struct dre_scm_probe_result *out,
    struct dre_scm_probe_reply_record *row,enum dre_scm_probe_status status,int uncertain)
{
    out->status=status;out->boot_must_stop=1;out->uncertain|=(uint32_t)uncertain;
    if(row)row->state=uncertain?DRE_SCM_REPLY_UNCERTAIN:DRE_SCM_REPLY_ERROR;
    return status;
}
enum dre_scm_probe_status dre_gpu_scm_probe_with_io(const void *dt,size_t bytes,
    const struct dre_scm_probe_io *io,struct dre_scm_probe_result *out)
{
    if(!out)return DRE_SCM_PROBE_ARGUMENT;
    *out=(struct dre_scm_probe_result){.version=DRE_SCM_PROBE_VERSION,.struct_bytes=sizeof(*out)};
    if(!dt||!io||!io->read_context||!io->smc64) {
        out->status=DRE_SCM_PROBE_ARGUMENT;return DRE_SCM_PROBE_ARGUMENT;
    }
    if(bytes<40||bytes>(2u<<20)||
       fdt_check_full(dt,bytes)||fdt_totalsize(dt)!=bytes||
       fdt_node_check_compatible(dt,0,"qcom,holi")||!known_board(dt)) {
        out->status=DRE_SCM_PROBE_BOARD;return DRE_SCM_PROBE_BOARD;
    }
    struct dre_gpu_scm_profile profile={0};
    const char *observed=dre_gpu_scm_profile_from_fdt(dt,bytes,&profile);
    const char expected[]="SCM_EMPTY_NAMED_CLOCK_PROFILE_ONLY";
    unsigned i=0;while(observed[i]&&observed[i]==expected[i])i++;
    if(observed[i]!=expected[i]||profile.clock_mask) {
        out->status=DRE_SCM_PROBE_PROFILE;return DRE_SCM_PROBE_PROFILE;
    }
    if(io->read_context(io->cookie,&out->initial)!=1||!valid_context(&out->initial)) {
        out->status=DRE_SCM_PROBE_CONTEXT;out->boot_must_stop=1;return DRE_SCM_PROBE_CONTEXT;
    }
    struct dre_scm_probe_context last=out->initial;
    for(unsigned progress=0;;progress++) {
        if(!context(io,out,&last))return failure(out,0,DRE_SCM_PROBE_CONTEXT_CHANGED,0);
        if(last.counter>out->initial.counter)break;
        if(progress==1023)return failure(out,0,DRE_SCM_PROBE_CLOCK_STALLED,0);
    }
    for(unsigned index=0;index<DRE_SCM_PROBE_QUERIES;index++) {
        struct dre_scm_probe_reply_record *row=&out->replies[index];
        row->function=INFO_FAST64;row->target=targets[index];out->query_count=index+1;
        uint64_t start=last.counter;int terminal=0;
        for(unsigned retry=0;retry<=20&&!terminal;retry++) {
            struct dre_a619_smc_regs r={{INFO_FAST64,1,targets[index],0,0,0,0,0}};
            for(unsigned continuation=0;;continuation++) {
                if(!context(io,out,&last))return failure(out,row,DRE_SCM_PROBE_CONTEXT_CHANGED,continuation!=0);
                row->calls++;
                if(io->smc64(io->cookie,&r)!=1)return failure(out,row,DRE_SCM_PROBE_TRANSPORT,1);
                row->transport=(int64_t)r.x[0];row->service=(int64_t)r.x[1];
                if(!context(io,out,&last))return failure(out,row,DRE_SCM_PROBE_CONTEXT_CHANGED,1);
                if(r.x[0]!=1)break;
                row->interruptions++;
                if(continuation==63||last.counter-start>=19200000)
                    return failure(out,row,DRE_SCM_PROBE_CONTINUATION_LIMIT,1);
                uint64_t resume=r.x[6];
                r=(struct dre_a619_smc_regs){{1,1,targets[index],0,0,0,resume,0}};
            }
            if(row->transport!=-12){terminal=1;break;}
            row->busy++;
            if(retry==20||last.counter-start>=19200000)break;
            uint64_t before=last.counter;
            unsigned unchanged=0;
            for(unsigned polls=0;;polls++) {
                uint64_t previous=last.counter;
                if(!context(io,out,&last))return failure(out,row,DRE_SCM_PROBE_CONTEXT_CHANGED,0);
                if(last.counter-before>=576000)break;
                unchanged=last.counter==previous?unchanged+1:0;
                if(unchanged==65536||polls==31999999)
                    return failure(out,row,DRE_SCM_PROBE_CLOCK_STALLED,0);
            }
        }
        if(!terminal) {
            row->state=DRE_SCM_REPLY_BUSY_EXHAUSTED;out->status=DRE_SCM_PROBE_BUSY_LIMIT;
            return DRE_SCM_PROBE_BUSY_LIMIT; /* Terminal BUSY: no live continuation. */
        }
        if(row->transport==0&&row->service==1) {
            row->state=DRE_SCM_REPLY_AVAILABLE;out->available_mask|=1u<<index;
        } else if(row->transport==0&&row->service==0) {
            row->state=DRE_SCM_REPLY_UNAVAILABLE;out->unavailable_mask|=1u<<index;
        } else row->state=DRE_SCM_REPLY_ERROR;
        if(!index&&row->state!=DRE_SCM_REPLY_AVAILABLE) {
            out->status=DRE_SCM_PROBE_UNSUPPORTED;return DRE_SCM_PROBE_UNSUPPORTED;
        }
    }
    out->status=out->available_mask==63?DRE_SCM_PROBE_OBSERVED:DRE_SCM_PROBE_PARTIAL;
    return (enum dre_scm_probe_status)out->status;
}
const char *dre_gpu_scm_probe_status_name(enum dre_scm_probe_status s)
{
    /* Characters, NOT a pointer array/switch-return lookup table: origin0
     * link-time string addresses would not survive physical ABL relocation. */
    static const char names[][40]={
        "INFO_PROBE_NOT_RUN_OR_ARGUMENT","INFO_CALLS_OBSERVED_ONLY",
        "SMC64_UNSUPPORTED_OR_ERROR","INFO_PARTIAL_OR_ERROR",
        "INFO_PROBE_NOT_RUN_OR_ARGUMENT","ORIGINAL_DRE_FDT_REQUIRED",
        "SCM_CLOCK_PROFILE_UNSUPPORTED","EARLY_BOOT_CONTEXT_REQUIRED",
        "EARLY_BOOT_CONTEXT_CHANGED","SMC_TRANSPORT_UNCERTAIN",
        "INFO_CONTINUATION_UNRESOLVED","INFO_BUSY_EXHAUSTED",
        "COUNTER_NOT_PROGRESSING","INFO_PROBE_ALREADY_RUN","INFO_PROBE_ARCH_UNAVAILABLE"};
    unsigned index=(unsigned)s;
    return names[index<sizeof(names)/sizeof(names[0])?index:0];
}
#if defined(__aarch64__)
static int actual_context(void *cookie,struct dre_scm_probe_context *c)
{
    (void)cookie;
    __asm__ volatile("mrs %0,CurrentEL":"=r"(c->current_el));
    if(c->current_el!=4)return 1;
    __asm__ volatile("mrs %0,daif":"=r"(c->daif));
    __asm__ volatile("mrs %0,sctlr_el1":"=r"(c->sctlr));
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(c->frequency));
    __asm__ volatile("mrs %0,mpidr_el1":"=r"(c->mpidr));
    __asm__ volatile("isb; mrs %0,cntpct_el0":"=r"(c->counter)::"memory");return 1;
}
static int actual_info(void *cookie,struct dre_a619_smc_regs *r)
{
    (void)cookie;int target=0;
    for(unsigned i=0;i<DRE_SCM_PROBE_QUERIES;i++)if(r->x[2]==targets[i])target=1;
    if(!target||(r->x[0]!=INFO_FAST64&&r->x[0]!=1)||r->x[1]!=1||
       r->x[3]||r->x[4]||r->x[5]||r->x[7]||(r->x[0]!=1&&r->x[6]))return 0;
    return dre_a619_smc64_arm64(0,r);
}
#endif
const char *dre_gpu_scm_probe_early(const void *dt,size_t bytes,struct dre_scm_probe_result *out)
{
#if defined(__aarch64__)
    static uint32_t entered;
    static struct dre_scm_probe_result recorded;
    uint32_t empty=0;
    if(!out)return dre_gpu_scm_probe_status_name(DRE_SCM_PROBE_ARGUMENT);
    if(!__atomic_compare_exchange_n(&entered,&empty,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        if(empty==2)*out=recorded;
        else *out=(struct dre_scm_probe_result){.version=DRE_SCM_PROBE_VERSION,.struct_bytes=sizeof(*out)};
        out->status=DRE_SCM_PROBE_ALREADY_RUN;out->boot_must_stop=1;
        return dre_gpu_scm_probe_status_name(DRE_SCM_PROBE_ALREADY_RUN);
    }
    /* The shim links at0 and is physically relocated by ABL without applying
     * ELF data relocations. A constant callback table would contain origin0
     * addresses. ADR materializes each actual runtime PC-relative address. */
    uintptr_t read_pc,info_pc;
    __asm__ volatile("adr %0, %c1":"=r"(read_pc):"i"(actual_context));
    __asm__ volatile("adr %0, %c1":"=r"(info_pc):"i"(actual_info));
    const struct dre_scm_probe_io io={0,
        (int (*)(void *,struct dre_scm_probe_context *))read_pc,
        (int (*)(void *,struct dre_a619_smc_regs *))info_pc};
    enum dre_scm_probe_status status=dre_gpu_scm_probe_with_io(dt,bytes,&io,out);
    recorded=*out;__atomic_store_n(&entered,2,__ATOMIC_RELEASE);
    return dre_gpu_scm_probe_status_name(status);
#else
    (void)dt;(void)bytes;
    if(out)*out=(struct dre_scm_probe_result){.version=1,.struct_bytes=sizeof(*out),.status=DRE_SCM_PROBE_ARCH_UNAVAILABLE};
    return dre_gpu_scm_probe_status_name(DRE_SCM_PROBE_ARCH_UNAVAILABLE);
#endif
}
