/* Report selected and active CPUs through real Darwin sysctls. */
.p2align 2
Ldre_check_cpus:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    mov w8, #1
    str w8, [sp, #16]
    mov w8, #'?'
    strb w8, [sp, #32]
    strb w8, [sp, #33]
.macro cpu_sysctl key, offset
    adr x0, Lcpu_key_\key
    mov x1, #(Lcpu_end_\key - Lcpu_key_\key - 1)
    add x2, sp, #40
    add x3, sp, #24
    mov x8, #4
    str x8, [x3]
    mov x4, #0
    mov x5, #0
    mov x16, #274
    svc #0x80
    b.cs 7f
    ldr x8, [sp, #24]
    cmp x8, #4
    b.ne 7f
    ldr w8, [sp, #40]
    cmp w8, #9
    b.hi 7f
    add w9, w8, #'0'
    strb w9, [sp, #\offset]
    cmp w8, #DRE_EXPECT_CPUS
    b.eq 8f
7:  str wzr, [sp, #16]
8:
.endm
    cpu_sysctl total, 32
    cpu_sysctl active, 33
    adr x1, Lcpu_prefix
    mov x2, #(Lcpu_prefix_end - Lcpu_prefix)
    bl write_console
    add x1, sp, #32
    mov x2, #1
    bl write_console
    adr x1, Lcpu_slash
    mov x2, #1
    bl write_console
    add x1, sp, #33
    mov x2, #1
    bl write_console
    ldr w8, [sp, #16]
    cbz w8, 1f
    adr x1, Lcpu_pass
    mov x2, #(Lcpu_fail - Lcpu_pass)
    b 2f
1:  adr x1, Lcpu_fail
    mov x2, #(Lcpu_messages_end - Lcpu_fail)
2:  bl write_console
    ldp x29, x30, [sp], #64
    ret
Lcpu_key_total: .asciz "hw.ncpu"
Lcpu_end_total:
Lcpu_key_active: .asciz "hw.activecpu"
Lcpu_end_active:
Lcpu_prefix: .ascii "CPUS="
Lcpu_prefix_end:
Lcpu_slash: .ascii "/"
Lcpu_pass: .ascii " PASS\n"
Lcpu_fail: .ascii " FAIL\n"
Lcpu_messages_end:
