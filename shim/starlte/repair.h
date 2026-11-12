/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Recovery's repair and reset tools (repair.c, docs/STARBITE-RECOVERY.md section 8).
 * Each write action has a plan (read only: what would change, where) and an apply that the caller
 * runs only after the user confirmed. Every finding is a line through struct fw_out (sbfwfs.h).
 */
#pragma once
#include <stdint.h>
#include "sbfwfs.h"

/* The home store: header, directory, settings, crash log, the card head's backup; full = 1 also
 * reads every entry and checks its CRC. Returns the problems found. Read only. */
int rp_store_check(struct fw_out *o, int full);

/* The card store's head (blocks 0, 1, 3..6) from its backup at p2 blocks RP_HEAD_BAK.. (written by
 * tools/store/card-store.sh). Plan: 1 if a repair is possible (head bad, backup good). */
#define RP_HEAD_BAK 64U
int rp_head_plan(struct fw_out *o);
int rp_head_apply(struct fw_out *o);

/* BOOT from the store entry "boot-good.img" (a full BOOT image, CRC checked): plan = 1 if it can be
 * written and differs; apply writes the differing 4 KiB blocks of the BOOT partition, each read back. */
int rp_boot_plan(struct fw_out *o);
int rp_boot_apply(struct fw_out *o);

/* A volatile entry's bytes set to zero (Starbite OS's "sbos-state": its settings and data). */
int rp_reset_plan(struct fw_out *o, const char *name);
int rp_reset_apply(struct fw_out *o, const char *name);

/* The logs (pmsg, dmesg, the loader's log page, the crash log, POST) into the volatile entry
 * "loader-logs" of the home store, for Starbite OS or the Mac (sbstore2.py logs). 0 on success. */
int rp_logs_save(struct fw_out *o);
