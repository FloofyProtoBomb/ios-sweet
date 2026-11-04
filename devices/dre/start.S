/* Linux arm64 Image ABI: x0 points at the DTB selected by OnePlus ABL. */
.section .text.head,"ax"
.global _head
_head:
    b       start
    .long   0
    .quad   0
    .quad   _end
    .quad   0xa
    .fill   3,8,0
    .ascii  "ARM\x64"
    .long   0
start:
    msr     daifset, #0xf
    adrp    x1, _stack_top
    add     x1, x1, :lo12:_stack_top
    mov     sp, x1
    bl      dre_main
    dsb     sy
1:  wfe
    b       1b
