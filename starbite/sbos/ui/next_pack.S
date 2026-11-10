/* SPDX-License-Identifier: MIT
 * The sbtk asset pack (fonts, icons) for Starbite OS Next: tools/sbtk-pack.py --phone. */
    .section .rodata
    .balign 64
    .global sbtk_pack_start, sbtk_pack_end
sbtk_pack_start:
    .incbin SBTK_PACK
sbtk_pack_end:
    .balign 64
