/* sbsim: linked at 0x40100000 (QEMU virt RAM starts at 0x40000000; QEMU's own DTB sits at its base) */
ENTRY(_start)
SECTIONS
{
    . = 0x40100000;
    .text : { *(.text.start) *(.text*) }
    .rodata : ALIGN(16) { *(.rodata*) }
    .data : ALIGN(16) { *(.data*) }
    .bss (NOLOAD) : ALIGN(16) { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(16); __bss_end = .; }
    . = ALIGN(16);
    . += 0x10000;
    __stack_top = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
