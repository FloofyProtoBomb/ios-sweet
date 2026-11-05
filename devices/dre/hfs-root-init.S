/* SPDX-License-Identifier: MIT
 * Static PID 1 for a disposable QEMU HFSX root, not iOS or phone evidence.
 * One relocation-free, read-only __text section wrapped by tools/mkinit.py.
 */
.text
.globl _start
.p2align 2
_start:
    mov x9, sp
    and x9, x9, #~15
    mov sp, x9
    sub sp, sp, #128
    adr x0, console
    mov x1, #2
    mov x2, #0
    mov x16, #5
    svc #0x80
    b.cs hang
    mov x19, x0
    mov x27, #1
    mov x16, #20
    svc #0x80
    cmp x0, #1
    b.ne failed
    adr x1, started
    adr x2, started_end
    sub x2, x2, x1
    bl print
    mov x27, #2
    adr x0, check_path
    mov x1, #0x602                 /* O_RDWR|O_CREAT|O_TRUNC */
    mov x2, #0600
    mov x16, #5
    svc #0x80
    b.cs failed
    mov x20, x0
    mov x27, #3
    mov x0, x20
    adr x1, contents
    adr x2, contents_end
    sub x2, x2, x1
    mov x21, x2
    mov x16, #4
    svc #0x80
    b.cs failed
    cmp x0, x21
    b.ne failed
    mov x27, #4
    mov x0, x20
    mov x16, #95                  /* fsync(file) */
    svc #0x80
    b.cs failed
    mov x27, #5
    mov x0, x20
    mov x1, #51                   /* fcntl(F_FULLFSYNC): HFS journal flush */
    mov x2, #0
    mov x16, #92
    svc #0x80
    b.cs failed
    mov x27, #6
    mov x0, x20
    mov x16, #6
    svc #0x80
    b.cs failed
    mov x27, #7
    adr x0, check_path
    mov x1, #0
    mov x2, #0
    mov x16, #5
    svc #0x80
    b.cs failed
    mov x20, x0
    mov x27, #8
    mov x1, sp
    mov x2, x21
    mov x16, #3
    svc #0x80
    b.cs failed
    cmp x0, x21
    b.ne failed
    mov x27, #9
    adr x9, contents
    mov x10, sp
    mov x11, x21
compare:
    ldrb w12, [x9], #1
    ldrb w13, [x10], #1
    cmp w12, w13
    b.ne failed
    subs x11, x11, #1
    b.ne compare
    mov x0, x20
    mov x16, #6
    svc #0x80
    mov x27, #10
    adr x0, raw_path
    mov x1, #2
    mov x2, #0
    mov x16, #5
    svc #0x80
    b.cs failed
    mov x20, x0
    mov x27, #11
    mov x1, #0x6416
    movk x1, #0x2000, lsl #16     /* legacy DKIOCSYNCHRONIZECACHE */
    mov x2, #0
    mov x16, #54
    svc #0x80
    b.cs failed
    mov x27, #12
    stp xzr, xzr, [sp, #64]
    str xzr, [sp, #80]
    mov x0, x20
    mov x1, #0x6416
    movk x1, #0x8018, lsl #16     /* DKIOCSYNCHRONIZE, 24-byte struct */
    add x2, sp, #64
    mov x16, #54
    svc #0x80
    b.cs failed
    mov x0, x20
    mov x16, #6
    svc #0x80
    adr x1, passed
    adr x2, passed_end
    sub x2, x2, x1
    bl print
    mov x27, #0
tick:
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x3, #0
    adr x4, delay
    mov x16, #93
    svc #0x80
    adr x1, heartbeat
    adr x2, heartbeat_end
    sub x2, x2, x1
    bl print
    b tick
failed:
    mov x26, x0                   /* errno or unexpected return, low byte */
    adr x1, failure
    adr x2, failure_end
    sub x2, x2, x1
    bl print
    adr x9, hex
    ubfx x10, x27, #4, #4
    ldrb w10, [x9, x10]
    strb w10, [sp, #96]
    and x10, x27, #15
    ldrb w10, [x9, x10]
    strb w10, [sp, #97]
    mov w10, #':'
    strb w10, [sp, #98]
    ubfx x10, x26, #4, #4
    ldrb w10, [x9, x10]
    strb w10, [sp, #99]
    and x10, x26, #15
    ldrb w10, [x9, x10]
    strb w10, [sp, #100]
    mov w10, #10
    strb w10, [sp, #101]
    add x1, sp, #96
    mov x2, #6
    bl print
    b tick
print:
    mov x0, x19
    mov x16, #4
    svc #0x80
    ret
hang:
    b hang
.p2align 3
delay: .quad 5, 0
console: .asciz "/dev/console"
check_path: .asciz "/dre-hfs-rw-check"
raw_path: .asciz "/dev/rufsroot0"
hex: .ascii "0123456789abcdef"
contents: .ascii "DRE disposable QEMU HFSX root write/readback, not physical UFS.\n"
contents_end:
started: .ascii "\nDRE_HFS_ROOT=PID1_RUNNING\n"
started_end:
passed: .ascii "DRE_HFS_RW=CREATE_WRITE_FSYNC_READBACK_PASS\nDRE_HFS_FLUSH=FULLFSYNC_LEGACY_STRUCTURED_PASS\n"
passed_end:
failure: .ascii "DRE_HFS_RW=FAIL_STAGE_ERRNO_"
failure_end:
heartbeat: .ascii "DRE_HFS_ROOT=HEARTBEAT\n"
heartbeat_end:
