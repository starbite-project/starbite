/* SPDX-License-Identifier: MIT
 * Starbite OS app layout (docs/STARBITE-OS.md 6.1): a static PIE linked at 0, the 256-byte header first, the
 * entry trampoline right after it; the file runs to _file_end, the bss after it. mk-sba.py fills the header. */
ENTRY(_sbapp_entry)
SECTIONS
{
    . = 0;
    .sbapp.head : { KEEP(*(.sbapp.head)) }
    .text : { KEEP(*(.text.sbapp_entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .gnu.hash : { *(.gnu.hash) }
    .hash : { *(.hash) }
    . = ALIGN(16);
    .rela.dyn : { *(.rela.dyn) *(.rela.*) }
    __rela_start = ADDR(.rela.dyn);
    __rela_end = ADDR(.rela.dyn) + SIZEOF(.rela.dyn);
    . = ALIGN(16);
    .dynamic : { *(.dynamic) }
    .data : { *(.data .data.*) *(.got) *(.got.plt) }
    . = ALIGN(16);
    _file_end = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    _bss_end = .;
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) *(.interp) }
}
