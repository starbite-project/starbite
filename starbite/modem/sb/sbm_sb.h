/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SBM_SB_H
#define SBM_SB_H
#include "../sbm.h"
/* Fills p for a libsb image; line() gets every (already redacted) log line. */
void sbm_sb_plat(struct sbm_plat *p, void (*line)(const char *));
/* Step 4 (user present): adds EL3's CP_CTRL read call and the two-word store test. */
void sbm_sb_plat_step4(struct sbm_plat *p);
/* Stage 3 (user present): EL3's CP_CTRL / SSS clock / security calls, the PMU / mailbox / AP2CP_CFG stores and
 * the carve-out copy, each behind this file's own allow-list. */
void sbm_sb_plat_stage3(struct sbm_plat *p);
/* Stage 4 (user present): stage 3 plus the IPC region view and the message interrupt / acknowledge stores. */
void sbm_sb_plat_stage4(struct sbm_plat *p, uint32_t ipc_off);
/* RADIO's TOC, map, plan and BOOT/MAIN/VSS, and EFS's /nv_data.bin, read only, into sb_alloc'd RAM. */
int sbm_sb_load_image(struct sbm_image *im, void (*line)(const char *));
/* The small footprint (Starbite OS): only nv_data.bin (and its pristine copy) in RAM, BOOT/MAIN/VSS streamed from
 * RADIO at each start through a SBM_SB_CHUNK buffer; about 2.3 MiB, taken once (later calls reuse it). */
#define SBM_SB_CHUNK (256u * 1024u)
int sbm_sb_load_image_stream(struct sbm_image *im, void (*line)(const char *));
/* take the stream loader's buffers now (the chunk, nv_data and its pristine copy of nv_cap bytes); no I/O */
int sbm_sb_reserve(uint32_t nv_cap);
uint32_t sbm_sb_reserved(void);          /* bytes reserved, 0 if not */
/* SErrors that were already pending before a probe read (not caused by it). */
unsigned sbm_sb_stale_serrors(void);
#endif
