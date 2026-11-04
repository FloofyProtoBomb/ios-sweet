/* EL1, DAIF masked, MMU off. Guard one MMIO load or store at a time. */
.text
.global dre_read32
dre_read32:
    mrs x2, vbar_el1
    adr x3, Lguard_vectors
    msr vbar_el1, x3
    isb
Lguard_load:
    ldr w3, [x0]
    msr vbar_el1, x2
    isb
    str w3, [x1]
    mov w0, #1
    ret
Lguard_failed:
    msr vbar_el1, x2
    isb
    mov w0, wzr
    ret

.global dre_write32
dre_write32:
    mrs x2, vbar_el1
    adr x3, Lguard_vectors
    msr vbar_el1, x3
    isb
Lguard_store:
    str w1, [x0]
    dsb sy
    msr vbar_el1, x2
    isb
    mov w0, #1
    ret

.macro vector slot
    .balign 128
    .if (\slot == 0) || (\slot == 4)
    mrs x4, esr_el1
    lsr x4, x4, #26
    cmp x4, #0x25                 /* data abort from current EL */
    b.ne 1f
    mrs x4, elr_el1
    adr x5, Lguard_load
    cmp x4, x5
    b.eq 2f
    adr x5, Lguard_store
    cmp x4, x5
    b.ne 1f
2:
    adr x4, Lguard_failed
    msr elr_el1, x4
    eret
    .endif
1:  msr vbar_el1, x2
    isb
    add x4, x2, #(\slot * 128)
    br x4
.endm

.balign 2048
Lguard_vectors:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vector \n
.endr
