/* Check the memory size reported by the running XNU kernel. */
#define DRE_RAM_TEXT_(x) #x
#define DRE_RAM_TEXT(x) DRE_RAM_TEXT_(x)
.p2align 2
Ldre_check_memory:
    stp x29, x30, [sp, #-16]!
    sub sp, sp, #32
    mov x8, #8
    str x8, [sp]
    adr x0, Lmemory_key
    mov x1, #10
    add x2, sp, #16
    mov x3, sp
    mov x4, #0
    mov x5, #0
    mov x16, #274
    svc #0x80
    b.cs Lmemory_fail
    ldr x8, [sp]
    cmp x8, #8
    b.ne Lmemory_fail
    adr x9, Lmemory_expected
    ldr x9, [x9]
    ldr x8, [sp, #16]
    cmp x8, x9
    b.ne Lmemory_fail
    adr x1, Lmemory_pass
    mov x2, #(Lmemory_pass_end - Lmemory_pass)
    b Lmemory_write
Lmemory_fail:
    adr x1, Lmemory_error
    mov x2, #(Lmemory_error_end - Lmemory_error)
Lmemory_write:
    bl write_console
    add sp, sp, #32
    ldp x29, x30, [sp], #16
    ret
.p2align 3
Lmemory_expected: .quad DRE_EXPECT_RAM_MIB * 1048576
Lmemory_key: .asciz "hw.memsize"
Lmemory_pass: .ascii "RAM_MIB="
    .ascii DRE_RAM_TEXT(DRE_EXPECT_RAM_MIB)
    .ascii " PASS\n"
Lmemory_pass_end:
Lmemory_error: .ascii "RAM_SIZE=UNEXPECTED\n"
Lmemory_error_end:
