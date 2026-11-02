/* SPDX-License-Identifier: GPL-2.0-only
 * Project Starbite boot menu and Starbite Engineer mode (menu.c). */
#pragma once
#include <stdint.h>

struct star_info {
    uint64_t el, sctlr, fdt, rd_start, rd_end;
    uint64_t phys_base, mem_size, entry_va, nsegs;
};

/*
 * Boot menu under Samsung's logo: the catalog's OS rows (catalog.h), Recovery (TWRP), Download,
 * and Starbite Engineer. Returns the catalog entry to start + 1 (0: Download mode), with
 * `cmdline` (BOOT's iOS boot-args) edited in place by Engineer; Recovery and Download reboot
 * from here. If the start fails, star_menu_failed says why and star_menu is called again: it
 * comes back as it was, with the reason on screen.
 */
int star_menu(const struct star_info *in, char *cmdline, unsigned len);
void star_menu_failed(const char *name, const char *why);
/* Catalog entry + 1 of iOS from BOOT (what Engineer's "boot iOS" starts), 0 if there is none. */
int star_ios_pick(void);

/* xnu_boot.c: draw the Starbite overlay again (over black). */
void star_overlay_redraw(void);

/* xnu_boot.c: the v5 menu's sprites ("SBMENU01" in the BOOT ramdisk slot,
 * tools/brand/menu.mjs). star_sprite draws every sprite with that id and
 * returns how many it drew (0 without assets, -1 if malformed). */
int star_menu_assets(void);
int star_sprite(unsigned id);
/* Rectangle of the first sprite with this id; 0 if there is none. */
int star_sprite_rect(unsigned id, unsigned *x, unsigned *y, unsigned *w, unsigned *h);
