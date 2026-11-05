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
    mov w28, #0x0102 /* stage 1; errno initially zero */
    adr x0, Lnative_part
    mov x1, #0
    mov x2, #0
    mov x16, #5
    svc #0x80
    b.cs Lnative_syscall_error
    mov x20, x0
    mov x0, x20
    mov w28, #0x0202 /* stage 2; errno initially zero */
    mov x1, #0x6418
    movk x1, #0x4004, lsl #16      /* DKIOCGETBLOCKSIZE */
    mov x2, sp
    mov x16, #54
    svc #0x80
    b.cs Lnative_syscall_error
    ldr w9, [sp]
    cmp w9, #512
    b.ne Lnative_cleanup
    mov x0, x20
    mov w28, #0x0302 /* stage 3; errno initially zero */
    mov x1, #0x6419
    movk x1, #0x4008, lsl #16      /* DKIOCGETBLOCKCOUNT */
    mov x2, sp
    mov x16, #54
    svc #0x80
    b.cs Lnative_syscall_error
    ldr x9, [sp]
    cbz x9, Lnative_cleanup
    mov x0, x20
    mov w28, #0x0402 /* stage 4; errno initially zero */
    mov x1, #0x641d
    movk x1, #0x4004, lsl #16      /* DKIOCISWRITABLE */
    mov x2, sp
    mov x16, #54
    svc #0x80
    b.cs Lnative_syscall_error
    ldr w9, [sp]
    cbnz w9, Lnative_cleanup
    mov x0, x20
    mov w28, #0x0502 /* stage 5; errno initially zero */
    mov x1, #512                  /* Exercise a 512-byte slice of userdata. */
    mov x2, #0
    mov x16, #199
    svc #0x80
    b.cs Lnative_syscall_error
    cmp x0, #512
    b.ne Lnative_cleanup
    mov x0, x20
    add x1, sp, #64
    mov w28, #0x0602 /* stage 6; errno initially zero */
    mov x2, #512
    mov x16, #3
    svc #0x80
    b.cs Lnative_syscall_error
    cmp x0, #512
    b.ne Lnative_cleanup
    mov x0, x20
    mov x16, #6
    svc #0x80
    mov x20, #-1
    mov w28, #0x0702 /* stage 7; errno initially zero */
    adr x0, Lnative_part
    mov x1, #1                   /* O_WRONLY must fail EROFS, no write call */
    mov x2, #0
    mov x16, #5
    svc #0x80
    b.cc Lnative_unexpected_open
    cmp x0, #30
    b.ne Lnative_syscall_error
    mov w28, #0x0802 /* stage 8; errno initially zero */
    adr x0, Lnative_disk
    mov x1, #0
    mov x2, #0
    mov x16, #5
    svc #0x80
    b.cs Lnative_syscall_error
    mov x20, x0
    mov x0, x20
    mov w28, #0x0902 /* stage 9 */
    mov x1, #0x6419
    movk x1, #0x4008, lsl #16
    mov x2, sp
    mov x16, #54
    svc #0x80
    b.cs Lnative_syscall_error
    ldr x24, [sp]
    tst x24, #7
    b.ne Lnative_cleanup
    lsr x24, x24, #3
    cmp x24, #3
    b.lo Lnative_cleanup
    mov x0, x20
    mov w28, #0x0a02 /* stage 10; errno initially zero */
    mov x1, #4096
    mov x2, #0
    mov x16, #199
    svc #0x80
    b.cs Lnative_syscall_error
    cmp x0, #4096
    b.ne Lnative_cleanup
    mov x0, x20
    add x1, sp, #64
    mov w28, #0x0b02 /* stage 11; errno initially zero */
    mov x2, #4096
    mov x16, #3
    svc #0x80
    b.cs Lnative_syscall_error
    cmp x0, #4096
    b.ne Lnative_cleanup
    mov w28, #0x0c02 /* stage 12; errno initially zero */
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
Lnative_syscall_error:
    bfi w28, w0, #16, #16
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
    bl Lnative_driver_status
    adr x1, Lnative_fail
    mov x2, #(Lnative_fail_end - Lnative_fail)
    cmp w28, #1
    b.ne 2f
    adr x1, Lnative_pass
    mov x2, #(Lnative_pass_end - Lnative_pass)
2:  bl write_console
    cmp w28, #1
    b.eq 3f
    adr x1, Lnative_stage_text
    mov x2, #(Lnative_stage_text_end - Lnative_stage_text)
    bl write_console
    ubfx w0, w28, #8, #8
    bl Lnative_decimal
    adr x1, Lnative_errno_text
    mov x2, #(Lnative_errno_text_end - Lnative_errno_text)
    bl write_console
    lsr w0, w28, #16
    bl Lnative_decimal
    adr x1, Lnative_newline
    mov x2, #1
    bl write_console
3:
    ldp x24, xzr, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x20, x21, [sp], #16
    ldp x29, x30, [sp], #16
    ret
Lnative_part: .asciz "/dev/rufsroot0"
Lnative_disk: .asciz "/dev/rdremedia0"
.p2align 3
Lnative_signature: .ascii "EFI PART"
Lnative_pass: .ascii "NATIVE_UFS=BSD_PARTITION+GPT_READ_PASS + WRITE_OPEN_REJECTED\n"
Lnative_pass_end:
Lnative_fail: .ascii "NATIVE_UFS=UNAVAILABLE_OR_FAILED\n"
Lnative_fail_end:

/* Decimal output is bounded to three digits; kernel errno values fit. */
.p2align 2
Lnative_decimal:
    stp x29, x30, [sp, #-16]!
    sub sp, sp, #16
    mov w8, #999
    cmp w0, w8
    csel w0, w0, w8, ls
    mov w8, #100
    udiv w9, w0, w8
    msub w0, w9, w8, w0
    add w9, w9, #48
    strb w9, [sp]
    mov w8, #10
    udiv w9, w0, w8
    msub w0, w9, w8, w0
    add w9, w9, #48
    add w0, w0, #48
    strb w9, [sp, #1]
    strb w0, [sp, #2]
    mov x1, sp
    mov x2, #3
    bl write_console
    add sp, sp, #16
    ldp x29, x30, [sp], #16
    ret

/* Read only this driver's bounded status, never arbitrary kernel logs. */
Lnative_driver_status:
    stp x29, x30, [sp, #-16]!
    stp x20, x21, [sp, #-16]!
    sub sp, sp, #128
    mov x8, #96
    str x8, [sp]
    adr x0, Lnative_state_key
    mov x1, #18
    add x2, sp, #32
    mov x3, sp
    mov x4, #0
    mov x5, #0
    mov x16, #274
    svc #0x80
    b.cs Lnative_state_missing
    ldr x20, [sp]
    cmp x20, #2
    b.lo Lnative_state_missing
    cmp x20, #96
    b.hi Lnative_state_missing
    sub x20, x20, #1
    add x21, sp, #32
    ldrb w8, [x21, x20]
    cbnz w8, Lnative_state_missing
    mov x9, #0
Lnative_state_validate:
    ldrb w8, [x21, x9]
    cmp w8, #32
    b.lo Lnative_state_missing
    cmp w8, #126
    b.hi Lnative_state_missing
    add x9, x9, #1
    cmp x9, x20
    b.lo Lnative_state_validate
    adr x1, Lnative_state_prefix
    mov x2, #(Lnative_state_prefix_end - Lnative_state_prefix)
    bl write_console
    mov x1, x21
    mov x2, x20
    bl write_console
    adr x1, Lnative_newline
    mov x2, #1
    bl write_console
    b Lnative_state_done
Lnative_state_missing:
    adr x1, Lnative_state_unavailable
    mov x2, #(Lnative_state_unavailable_end - Lnative_state_unavailable)
    bl write_console
Lnative_state_done:
    add sp, sp, #128
    ldp x20, x21, [sp], #16
    ldp x29, x30, [sp], #16
    ret
Lnative_stage_text: .ascii "NATIVE_STAGE="
Lnative_stage_text_end:
Lnative_errno_text: .ascii " ERRNO="
Lnative_errno_text_end:
Lnative_newline: .ascii "\n"
Lnative_state_key: .asciz "kern.dre_ufs_state"
Lnative_state_prefix: .ascii "NATIVE_DRIVER="
Lnative_state_prefix_end:
Lnative_state_unavailable: .ascii "NATIVE_DRIVER=STATUS_UNAVAILABLE\n"
Lnative_state_unavailable_end:
