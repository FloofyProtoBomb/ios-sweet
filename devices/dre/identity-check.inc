/* Darwin PID 1 smoke test. Report only status, never the phone identifiers. */
.p2align 2
Ldre_check_ids:
    stp x29, x30, [sp, #-96]!
    mov x29, sp
.macro check_sysctl key, value
    adr x0, Lkey_\key
    mov x1, #(Lkey_end_\key - Lkey_\key - 1)
    add x2, sp, #32
    add x3, sp, #16
    mov x8, #40
    str x8, [x3]
    mov x4, #0
    mov x5, #0
    mov x16, #274
    svc #0x80
    b.cs Ldre_bad_ids
    adr x1, Lval_\value
    mov x2, #(Lval_end_\value - Lval_\value)
    ldr x3, [sp, #16]
    cmp x2, x3
    b.ne Ldre_bad_ids
    add x0, sp, #32
    bl Ldre_compare
    cbnz w0, Ldre_bad_ids
.endm
    check_sysctl machine, product
    check_sysctl model, board
    check_sysctl product, product
    check_sysctl target, board
    add x0, sp, #32
    add x1, sp, #64
    mov x8, #2
    stp x8, xzr, [x1]
    mov x16, #142
    svc #0x80
    b.cs Ldre_bad_ids
#ifdef DRE_EXPECT_UUID
    add x0, sp, #32
    adr x1, Ldre_expected_uuid
    mov x2, #16
    bl Ldre_compare
    cbnz w0, Ldre_bad_ids
#else
    ldrb w8, [sp, #38]            /* UUIDv5 version and RFC variant */
    and w8, w8, #0xf0
    cmp w8, #0x50
    b.ne Ldre_bad_ids
    ldrb w8, [sp, #40]
    and w8, w8, #0xc0
    cmp w8, #0x80
    b.ne Ldre_bad_ids
#endif
    adr x1, Ldre_pass
    mov x2, #(Ldre_fail - Ldre_pass)
    b Ldre_report
Ldre_bad_ids:
    adr x1, Ldre_fail
    mov x2, #(Ldre_messages_end - Ldre_fail)
Ldre_report:
#ifdef DRE_BOARD_TESTS
    mov x25, x1
    mov x26, x2
#endif
    bl write_console
    ldp x29, x30, [sp], #96
    ret
Ldre_compare:
    ldrb w8, [x0], #1
    ldrb w9, [x1], #1
    cmp w8, w9
    b.ne 1f
    subs x2, x2, #1
    b.ne Ldre_compare
    mov w0, #0
    ret
1:  mov w0, #1
    ret
Lkey_machine: .asciz "hw.machine"
Lkey_end_machine:
Lkey_model: .asciz "hw.model"
Lkey_end_model:
Lkey_product: .asciz "hw.product"
Lkey_end_product:
Lkey_target: .asciz "hw.target"
Lkey_end_target:
Lval_product: .asciz "iPhone10,3"
Lval_end_product:
Lval_board: .asciz "D22AP"
Lval_end_board:
Ldre_pass: .ascii "DRE identity checks PASS\n"
Ldre_fail: .ascii "DRE identity checks FAIL\n"
Ldre_messages_end:
#ifdef DRE_EXPECT_UUID
Ldre_expected_uuid: .byte DRE_EXPECT_UUID
#endif
#ifdef DRE_BOARD_TESTS
/* Reprint a compact status card each heartbeat so one photo captures it.
 * Only the shim's dre_report token is shown. */
.p2align 2
Ldre_print_report:
    stp x29, x30, [sp, #-16]!
    sub sp, sp, #688
    adr x1, Lboard_title
    mov x2, #(Lboard_title_end - Lboard_title)
    bl write_console
    mov x1, x25
    mov x2, x26
    bl write_console
#ifdef DRE_EXPECT_CPUS
    bl Ldre_check_cpus
#endif
#ifdef DRE_CHECK_UI_SERVICES
    bl Ldre_check_ui
#endif
    adr x0, Largs_name
    mov x1, #13                   /* strlen("kern.bootargs") */
    add x2, sp, #32
    mov x3, sp
    mov x8, #640
    str x8, [x3]
    mov x4, #0
    mov x5, #0
    mov x16, #274
    svc #0x80
    b.cs Lboard_missing
    ldr x8, [sp]
    cmp x8, #640
    b.hi Lboard_missing
    add x1, sp, #32
    add x9, x1, x8
    adr x10, Lreport_key
Lboard_find:
    add x11, x1, #11              /* strlen("dre_report=") */
    cmp x11, x9
    b.hs Lboard_missing
    mov x12, #0
Lboard_compare:
    ldrb w13, [x1, x12]
    ldrb w14, [x10, x12]
    cmp w13, w14
    b.ne Lboard_advance
    add x12, x12, #1
    cmp x12, #11
    b.lo Lboard_compare
    mov x1, x11
    mov x12, x11
Lboard_format:
    cmp x12, x9
    b.hs Lboard_done
    ldrb w13, [x12]
    cmp w13, #32
    b.ls Lboard_done
    cmp w13, #124                 /* translate | into a line break */
    b.ne 1f
    mov w14, #10
    strb w14, [x12]
1:  add x12, x12, #1
    b Lboard_format
Lboard_advance:
    add x1, x1, #1
    b Lboard_find
Lboard_done:
    mov w14, #10
    strb w14, [x12]
    sub x2, x12, x1
    add x2, x2, #1
    b Lboard_write
Lboard_missing:
    adr x1, Lreport_missing
    mov x2, #(Lreport_missing_end - Lreport_missing)
Lboard_write:
    bl write_console
    add sp, sp, #688
    ldp x29, x30, [sp], #16
    ret
Lboard_title: .ascii "\n== DE2118 TEST | CPU0 + RAM ROOT + PID1: PASS ==\n"
Lboard_title_end:
Largs_name: .asciz "kern.bootargs"
Lreport_key: .ascii "dre_report="
Lreport_missing: .ascii "Board survey unavailable in boot arguments\n"
Lreport_missing_end:
#endif
