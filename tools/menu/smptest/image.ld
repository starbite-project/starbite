ENTRY(_start)
SECTIONS
{
    . = 0x80080000;
    .text : { KEEP(*(.text.head)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
