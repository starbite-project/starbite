/* SPDX-License-Identifier: GPL-2.0-only
 * libsb image layout: a static PIE linked at 0 (SB_LINK_BASE for the relocation test), raw
 * binary from _start to _image_end with .bss stored as zeros (boot_bare loads the file, CRC
 * checked, and clears nothing). .rela.dyn holds the R_AARCH64_RELATIVE entries crt0.S applies. */
ENTRY(_start)
SECTIONS
{
    . = SB_LINK_BASE;               /* --defsym: 0 for the image, 0x40000000 for the test */
    _image_start = .;
    .text : { KEEP(*(.text.head)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .gnu.hash : { *(.gnu.hash) }
    .hash : { *(.hash) }
    . = ALIGN(16);
    .rela.dyn : { *(.rela.dyn) *(.rela.*) }
    __rela_start = ADDR(.rela.dyn);
    __rela_end = ADDR(.rela.dyn) + SIZEOF(.rela.dyn);
    . = ALIGN(4096);
    .dynamic : { *(.dynamic) }
    .data : { *(.data .data.*) *(.got) *(.got.plt) *(.bss .bss.*) *(COMMON) }
    . = ALIGN(4096);
    _image_end = .;
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) *(.interp) }
}
