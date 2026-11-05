/* SPDX-License-Identifier: GPL-2.0-only
 * The embedded SB7ASSET container (fonts and icons, starbite/tools/mkassets.py). sb7_open scans
 * at 4 KiB steps, so it starts 4 KiB aligned. SB_ASSETS is the file, set by the build script. */
.section .rodata.assets, "a"
.balign 4096
.global sb_assets_start, sb_assets_end
sb_assets_start:
    .incbin SB_ASSETS
sb_assets_end:
    .balign 16
