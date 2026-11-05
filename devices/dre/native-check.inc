/* Native BSD path, not shim report strings. Never submits a media write. */
.p2align 2
Ldre_check_native:
    stp x29, x30, [sp, #-16]!
    stp x20, x21, [sp, #-16]!
    stp x22, x23, [sp, #-16]!
    stp x24, xzr, [sp, #-16]!
    cbnz w28, Lnative_cached
    mov w28, #2
    mov x20, #-1
    sub sp, sp, #4096
    sub sp, sp, #64
    adr x0, Lnative_part
    mov x1, #0
    mov x2, #0
    mov x16, #5
    svc #0x80
    b.cs Lnative_cleanup
    mov x20, x0
    mov x0, x20
    mov x1, #0x6418
    movk x1, #0x4004, lsl #16      /* DKIOCGETBLOCKSIZE */
    mov x2, sp
    mov x16, #54
    svc #0x80
    b.cs Lnative_cleanup
    ldr w9, [sp]
    cmp w9, #512
    b.ne Lnative_cleanup
    mov x0, x20
    mov x1, #0x6419
    movk x1, #0x4008, lsl #16      /* DKIOCGETBLOCKCOUNT */
    mov x2, sp
    mov x16, #54
    svc #0x80
    b.cs Lnative_cleanup
    ldr x9, [sp]
    cbz x9, Lnative_cleanup
    mov x0, x20
    mov x1, #0x641d
    movk x1, #0x4004, lsl #16      /* DKIOCISWRITABLE */
    mov x2, sp
    mov x16, #54
    svc #0x80
    b.cs Lnative_cleanup
    ldr w9, [sp]
    cbnz w9, Lnative_cleanup
    mov x0, x20
    mov x16, #6
    svc #0x80
    mov x20, #-1
    adr x0, Lnative_part
    mov x1, #1                   /* O_WRONLY must fail EROFS, no write call */
    mov x2, #0
    mov x16, #5
    svc #0x80
    b.cc Lnative_unexpected_open
    cmp x0, #30
    b.ne Lnative_cleanup
    adr x0, Lnative_disk
    mov x1, #0
    mov x2, #0
    mov x16, #5
    svc #0x80
    b.cs Lnative_cleanup
    mov x20, x0
    mov x0, x20
    mov x1, #0x6419
    movk x1, #0x4008, lsl #16
    mov x2, sp
    mov x16, #54
    svc #0x80
    b.cs Lnative_cleanup
    ldr x24, [sp]
    tst x24, #7
    b.ne Lnative_cleanup
    lsr x24, x24, #3
    cmp x24, #3
    b.lo Lnative_cleanup
    mov x0, x20
    mov x1, #4096
    mov x2, #0
    mov x16, #199
    svc #0x80
    b.cs Lnative_cleanup
    cmp x0, #4096
    b.ne Lnative_cleanup
    mov x0, x20
    add x1, sp, #64
    mov x2, #4096
    mov x16, #3
    svc #0x80
    b.cs Lnative_cleanup
    cmp x0, #4096
    b.ne Lnative_cleanup
    add x21, sp, #64
    adr x10, Lnative_signature
    ldr x9, [x21]
    ldr x10, [x10]
    cmp x9, x10
    b.ne Lnative_cleanup
    ldr w9, [x21, #8]
    cmp w9, #0x10, lsl #12
    b.ne Lnative_cleanup
    ldr w22, [x21, #12]
    cmp w22, #92
    b.lo Lnative_cleanup
    cmp w22, #4096
    b.hi Lnative_cleanup
    ldr x9, [x21, #24]
    cmp x9, #1
    b.ne Lnative_cleanup
    ldr x9, [x21, #32]
    sub x10, x24, #1
    cmp x9, x10
    b.ne Lnative_cleanup
    ldr w23, [x21, #16]
    str wzr, [x21, #16]
    mov w9, #-1
    mov w10, #0x8320
    movk w10, #0xedb8, lsl #16
Lnative_crc_byte:
    ldrb w11, [x21], #1
    eor w9, w9, w11
    mov w12, #8
Lnative_crc_bit:
    and w13, w9, #1
    neg w13, w13
    and w13, w13, w10
    eor w9, w13, w9, lsr #1
    subs w12, w12, #1
    b.ne Lnative_crc_bit
    subs w22, w22, #1
    b.ne Lnative_crc_byte
    mvn w9, w9
    cmp w9, w23
    b.ne Lnative_cleanup
    mov w28, #1
    b Lnative_cleanup
Lnative_unexpected_open:
    mov x20, x0
Lnative_cleanup:
    cmp x20, #-1
    b.eq 1f
    mov x0, x20
    mov x16, #6
    svc #0x80
1:  add sp, sp, #4096
    add sp, sp, #64
Lnative_cached:
    adr x1, Lnative_fail
    mov x2, #(Lnative_fail_end - Lnative_fail)
    cmp w28, #1
    b.ne 2f
    adr x1, Lnative_pass
    mov x2, #(Lnative_pass_end - Lnative_pass)
2:  bl write_console
    ldp x24, xzr, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x20, x21, [sp], #16
    ldp x29, x30, [sp], #16
    ret
Lnative_part: .asciz "/dev/rufsroot0"
Lnative_disk: .asciz "/dev/rdremedia0"
.p2align 3
Lnative_signature: .ascii "EFI PART"
Lnative_pass: .ascii "NATIVE_UFS=BSD_GPT_READ_PASS + WRITE_OPEN_REJECTED\n"
Lnative_pass_end:
Lnative_fail: .ascii "NATIVE_UFS=UNAVAILABLE_OR_FAILED\n"
Lnative_fail_end:
