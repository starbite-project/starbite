/* SPDX-License-Identifier: GPL-2.0-only
 * The built-in apps (mk-sba.py pack), embedded in the kernel image. SBOS_APPS is the file, set by build.sh. */
.section .rodata
.balign 16
.global sbos_apps_start
sbos_apps_start:
    .incbin SBOS_APPS
.global sbos_apps_end
sbos_apps_end:
.balign 16
