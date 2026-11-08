/* SPDX-License-Identifier: GPL-2.0-only
 * The guest inside guest-probe P2: the Linux Image, its DTB (mk-guest-dts.py --profile s9) and the initramfs.
 * P2_IMAGE / P2_DTB / P2_RD are the files, set by build.sh. sbghost copies them word-wise: 8-byte aligned. */
    .section .rodata.p2guest, "a"
    .balign 4096
    .global p2_image, p2_image_end, p2_dtb, p2_dtb_end, p2_rd, p2_rd_end
p2_image:
    .incbin P2_IMAGE
p2_image_end:
    .balign 8
p2_dtb:
    .incbin P2_DTB
p2_dtb_end:
    .balign 8
p2_rd:
    .incbin P2_RD
p2_rd_end:
    .balign 8
