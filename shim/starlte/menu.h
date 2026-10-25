/* SPDX-License-Identifier: GPL-2.0-only
 * Project Starbite boot menu and Starbite Engineer mode (menu.c). */
#pragma once
#include <stdint.h>

struct star_info {
    uint64_t el, sctlr, fdt, rd_start, rd_end;
    uint64_t phys_base, mem_size, entry_va, nsegs;
};

/*
 * Boot menu under Samsung's logo: iOS (Starbite), Linux kiosk, Recovery
 * (TWRP), Download mode, Starbite Engineer. Returns 1 to boot iOS with
 * `cmdline` (edited in place by Engineer mode), 2 for the Linux kiosk
 * Linux; Recovery and Download reboot from here.
 */
int star_menu(const struct star_info *in, char *cmdline, unsigned len);

/* xnu_boot.c: draw the Starbite overlay again (over black). */
void star_overlay_redraw(void);
