/* SPDX-License-Identifier: GPL-2.0-only
 * Device trees for Android from the store (dtmerge.c): pick an FDT out of a Samsung "DTBH"
 * container, and rebuild it with what S-Boot sets at run time. Plain C on byte arrays (host
 * tested by tools/loader-test with LineageOS's dt.img and UN1CA's DS-ACK G960F DT).
 *
 * DTBH (Samsung's dtbtool, version 2; S-Boot reads it from the BOOT image's DT section):
 *   0 "DTBH"  4 u32 version (2)  8 u32 count
 *  12 count x 32 bytes { u32 chip, platform, subtype, hw_rev, hw_rev_end, offset, size, space }
 *     then a zero u32. offset/size: the entry's FDT inside the container (page aligned, 2048).
 *     chip is the SoC (0x2652 = 9810); hw_rev..hw_rev_end the board revisions it serves.
 *     platform and subtype are ids of the DT's model_info-platform/-subtype strings.
 *   Every DTBH seen here (TWRP, LineageOS, UN1CA) has one entry: 9810, 0x50a6, 0x217584da,
 *   rev 26..255.
 *
 * What S-Boot changes at run time (its FDT at 0x8a000000 compared with the DT it was built from,
 * see docs/STARBITE-HEALTH.md section 6): /chosen bootargs (its whole command line), linux,initrd-
 * start/-end, revision (u32 board revision), serial (u64); the top-level /memory@... nodes (four
 * banks; the file has none); the root property model_info-system_rev ("026").
 */
#pragma once
#include <stdint.h>

#define DTBH_MAGIC 0x48425444U   /* "DTBH" little endian */

/* blob (size bytes) is a DTBH container or a plain FDT. Picks: chip equal and hw_rev <= rev <=
 * hw_rev_end, else the first entry with that chip, else the first entry. 1 and the FDT's offset
 * and totalsize inside blob (magic and bounds checked); *idx = the entry, *n = how many (0 for a
 * plain FDT). 0 if nothing usable. */
int dtbh_pick(const uint8_t *blob, uint64_t size, uint32_t chip, uint32_t rev, uint32_t *off,
              uint32_t *fsize, unsigned *idx, unsigned *n);

/* S-Boot's board: root model_info-chip, /chosen revision (else root model_info-hw_rev). 1 if the
 * chip was found (*rev 0 when no revision). */
int fdt_board_id(const uint8_t *sboot, uint32_t *chip, uint32_t *rev);

/* out (cap bytes) = base with S-Boot's run-time state:
 *   - /chosen: all of base's /chosen properties are replaced by all of S-Boot's, bootargs with
 *     " " extra appended (extra may be 0 or ""); base's /chosen subnodes are kept;
 *   - top-level memory / memory@... nodes: base's dropped, S-Boot's copied;
 *   - root properties S-Boot has and base lacks are added (model_info-system_rev).
 * Everything else is base's, byte for byte (phandles untouched). Returns the new totalsize, 0 on
 * a bad blob or no room. */
uint32_t fdt_merge_sboot(const uint8_t *base, const uint8_t *sboot, uint8_t *out, uint32_t cap,
                         const char *extra);

/* In every property named prop, each pair of cells <key from> becomes <key to> (in place, same
 * size). The number of pairs changed. */
int fdt_patch_pairs(uint8_t *fdt, const char *prop, uint32_t key, uint32_t from, uint32_t to);
