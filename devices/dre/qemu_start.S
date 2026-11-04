/* QEMU-only entry adapter. -M virt,virtualization=on provides PSCI via SMC
 * and starts at EL2. Enter the unchanged phone kernel at non-secure EL1. */
#ifndef BOOTARGS_PA
#error BOOTARGS_PA is required
#endif
#ifndef ENTRY_PA
#error ENTRY_PA is required
#endif
.text
.global _start
_start:
    msr daifset, #15
    mrs x1, CurrentEL
    cmp x1, #8
    b.eq Lqemu_el2
    cmp x1, #4
    b.eq Lqemu_el1
1:  b 1b
Lqemu_el2:
    mov x1, #0x80000000
    msr hcr_el2, x1
    msr vttbr_el2, xzr
    msr cptr_el2, xzr
    msr mdcr_el2, xzr
    mov x1, #3
    msr cnthctl_el2, x1
    msr cntvoff_el2, xzr
    mrs x1, icc_sre_el2
    orr x1, x1, #1
    orr x1, x1, #8
    msr icc_sre_el2, x1
    movz x1, #0x800
    movk x1, #0x30d0, lsl #16
    msr sctlr_el1, x1
    adr x1, Lqemu_el1
    msr elr_el2, x1
    mov x1, #0x3c5
    msr spsr_el2, x1
    isb
    eret
Lqemu_el1:
    ldr x0, Lqemu_bootargs
    ldr x1, Lqemu_entry
    br x1
.balign 8
Lqemu_bootargs: .quad BOOTARGS_PA
Lqemu_entry: .quad ENTRY_PA
