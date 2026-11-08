/* SPDX-License-Identifier: GPL-2.0-only
 * menuchain: a bare store image that puts another starlte loader Image (and optionally a new BOOT
 * ramdisk) where S-Boot would have, and enters it as S-Boot does (x0 = S-Boot's FDT, MMU off).
 * The BOOT loader starts it like any "bare" entry (RAM_BASE + text_offset, MMU and caches off), so a
 * new loader is tried without writing BOOT. Layout and packer: pack.py.
 */
.section .text.head, "ax"
.global _start
_start:
    b entry
    .long 0
    .quad 0x80000            /* text_offset */
    .quad 0                  /* image_size: pack.py */
    .quad 0
    .quad 0, 0, 0
    .byte 0x41, 0x52, 0x4d, 0x64
    .long 0
    /* +64: the chain header (pack.py): magic, loader off, loader len, rd off, rd len, loader entry,
     * watchdog counts (0: none) */
.global chain_hdr
chain_hdr:
    .quad 0x314e49414843374dULL   /* "M7CHAIN1" */
    .quad 0, 0, 0, 0
    .quad 0x80080000
    .quad 0
entry:
    msr daifset, #15
    mov x19, x0
    adr x1, chain_stack_top
    mov sp, x1
    adr x1, _start
    bl chain_main
    cbz x0, fail
    mov x16, x0
    dsb sy
    ic iallu
    dsb sy
    isb
    mov x0, x19
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    br x16
fail:
    /* nothing copied that matters: reboot normally (the BOOT loader comes back) */
    ldr x0, =0x14060000
    ldr w14, =0x12345678
    str w14, [x0, #0x808]
    ldr w14, =0x12345670
    str w14, [x0, #0x80c]
    dsb sy
    mov w14, #1
    str w14, [x0, #0x400]
    dsb sy
1:  wfe
    b 1b

.section .bss
.balign 16
chain_stack:
    .space 8192
chain_stack_top:
