/* SPDX-License-Identifier: GPL-2.0-only
 * The BCM4775 sensor hub's firmware patch (docs/SENSOR-HUB.md 2.2), for shub.c's download. Empty in every image
 * but the staged test image starbite-os-shub.img: build.sh builds this file a second time with SHUB_PATCH_FILE set
 * to a blob kept outside the repository (starbite/sbos/tools/shub-patch.py makes it from a Samsung kernel tree and
 * checks its sha256). The blob is never committed. */
.section .rodata
.balign 16
.global shub_patch_start
shub_patch_start:
#ifdef SHUB_PATCH_FILE
    .incbin SHUB_PATCH_FILE
#endif
.global shub_patch_end
shub_patch_end:
.balign 16
