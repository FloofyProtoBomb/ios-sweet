ENTRY(_head)
SECTIONS
{
    . = 0;
    .text : { KEEP(*(.text.head)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) KEEP(*(.dre_gpu_firmware)) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.* COMMON) }
    . = ALIGN(16);
    . += 0x8000;
    _stack_top = .;
    . = ALIGN(16);
    _payload_start = .;
    .payload : { KEEP(*(.dre_payload)) }
    _payload_end = .;
    _end = .;
    /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
