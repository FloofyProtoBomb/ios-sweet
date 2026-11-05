.section .text.entry,"ax"
.global _start
_start:
    msr daifset, #15
    mrs x0, CurrentEL
    cmp x0, #8
    b.ne 1f
    mov x0, #0x80000000
    msr hcr_el2, x0
    msr cptr_el2, xzr
    adr x0, vectors
    msr vbar_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    movz x0, #0x800
    movk x0, #0x30d0, lsl #16
    msr sctlr_el1, x0
#if TEST_CASE == 3
    ldr x0, =19200000
    msr cntfrq_el0, x0
#endif
    adr x0, 1f
    msr elr_el2, x0
    mov x0, #0x3c5
    msr spsr_el2, x0
    isb
    eret
1:
    adr x0, vectors
    msr vbar_el1, x0
    mov x0, #0x300000
    msr cpacr_el1, x0
    isb
    ldr x0, =0x40200000
    mov sp, x0
    bl test_main
2:  wfe
    b 2b
.balign 2048
vectors:
.rept 16
    b exception
    .balign 128
.endr
exception:
    mrs x0, CurrentEL
    cmp x0, #8
    b.eq 3f
    mrs x1, esr_el1
    mrs x2, elr_el1
    mrs x3, far_el1
    b 4f
3:  mrs x1, esr_el2
    mrs x2, elr_el2
    mrs x3, far_el2
4:  ldr x4, =0x401f0000
    str x1, [x4, #16]
    str x2, [x4, #24]
    str x3, [x4, #32]
    str x0, [x4, #40]
    mov x1, #0xffff
    str x1, [x4, #8]
    ldr x1, =0x44524553454d4931
    str x1, [x4]
5:  wfe
    b 5b
