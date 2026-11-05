/* SPDX-License-Identifier: MIT */
#include "a619_pas13.h"
int dre_a619_smc64_arm64(void *cookie,struct dre_a619_smc_regs *r)
{
    (void)cookie;
#if defined(__aarch64__)
    if (!r) return 0;
    register uint64_t x0 __asm__("x0")=r->x[0];
    register uint64_t x1 __asm__("x1")=r->x[1];
    register uint64_t x2 __asm__("x2")=r->x[2];
    register uint64_t x3 __asm__("x3")=r->x[3];
    register uint64_t x4 __asm__("x4")=r->x[4];
    register uint64_t x5 __asm__("x5")=r->x[5];
    register uint64_t x6 __asm__("x6")=r->x[6];
    register uint64_t x7 __asm__("x7")=r->x[7];
    /* SMC #0 encoding: accepted by cross assemblers that omit EL3 from
     * their selected userspace CPU feature set. Execution remains EL1-only. */
    __asm__ volatile(".inst 0xd4000003" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),
        "+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7) : :
        "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory","cc");
    r->x[0]=x0; r->x[1]=x1; r->x[2]=x2; r->x[3]=x3;
    r->x[4]=x4; r->x[5]=x5; r->x[6]=x6; r->x[7]=x7;
    return 1;
#else
    (void)r; return 0;
#endif
}
