/* Diagnostic only: observe actual successful driver-start log records.
 * This is not a test of UIKit, CoreAnimation clients or a rendered iOS UI. */
.p2align 2
Ldre_check_ui:
    stp x29, x30, [sp, #-32]!
    stp x20, x21, [sp, #16]
    sub sp, sp, #16, lsl #12
    and w27, w27, #3
    cmp w27, #3
    b.eq Ldre_ui_finish
    /* proc_info(PROC_INFO_CALL_KERNMSGBUF, 0, 0, 0, buffer, size).
     * /dev/klog exports ASL by default on this XNU, not these records. */
    mov x0, #4
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, sp
    mov x5, #65536
    mov x16, #336
    svc #0x80
    b.cs Ldre_ui_unavailable
    mov x21, x0
    cmp x21, #16, lsl #12
    b.hi Ldre_ui_unavailable
    mov x0, sp
    mov x1, x21
    adr x2, Ldre_surface_log
    mov x3, #(Ldre_surface_end - Ldre_surface_log)
    bl Ldre_find_log
    cbz w0, 3f
    orr w27, w27, #1
3:  mov x0, sp
    mov x1, x21
    adr x2, Ldre_display_log
    mov x3, #(Ldre_display_end - Ldre_display_log)
    bl Ldre_find_log
    cbz w0, Ldre_ui_finish
    orr w27, w27, #2
    b Ldre_ui_finish
Ldre_ui_unavailable:
    orr w27, w27, #4
Ldre_ui_finish:
    add sp, sp, #16, lsl #12
    and w8, w27, #3
    cmp w8, #3
    b.eq 4f
    tbnz w27, #2, 5f
    adr x1, Ldre_ui_partial
    mov x2, #(Ldre_ui_unreadable - Ldre_ui_partial)
    b 6f
4:  adr x1, Ldre_ui_both
    mov x2, #(Ldre_ui_partial - Ldre_ui_both)
    b 6f
5:  adr x1, Ldre_ui_unreadable
    mov x2, #(Ldre_ui_text_end - Ldre_ui_unreadable)
6:  bl write_console
    ldp x20, x21, [sp, #16]
    ldp x29, x30, [sp], #32
    ret

/* Bounded raw-byte substring search; x0=data, x1=size, x2=key, x3=key size. */
Ldre_find_log:
    cmp x1, x3
    b.lo 4f
    sub x4, x1, x3
1:  mov x5, #0
2:  ldrb w8, [x0, x5]
    ldrb w9, [x2, x5]
    cmp w8, w9
    b.ne 3f
    add x5, x5, #1
    cmp x5, x3
    b.lo 2b
    mov w0, #1
    ret
3:  add x0, x0, #1
    subs x4, x4, #1
    b.pl 1b
4:  mov w0, #0
    ret
Ldre_surface_log: .ascii "DRE SURF: IOCoreSurfaceRoot registered\n"
Ldre_surface_end:
Ldre_display_log: .ascii "DRE FB: AppleCLCD "
Ldre_display_end:
Ldre_ui_both: .ascii "UI_DRIVER_LOGS=SURFACE+FRAMEBUFFER\n"
Ldre_ui_partial: .ascii "UI_DRIVER_LOGS=NOT_BOTH_SEEN\n"
Ldre_ui_unreadable: .ascii "UI_DRIVER_LOGS=UNAVAILABLE\n"
Ldre_ui_text_end:
