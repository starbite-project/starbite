/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS's passcode (Track A, sbos-core-api.md 3): pure C, no hardware, no libc (the kernel, the host shell's
 * fake and kern/host/passcode-test.c build the same file). The caller gives the time: now_ms a clock that only moves
 * forward during one boot (sb_ms), utc the wall clock in seconds (0: not known).
 *
 * What is kept: a 16-byte salt and an iterated SHA-256 of the digits (PASS_ROUNDS rounds), the length (4 or 6), the
 * wrong tries in a row and the lock-out still to wait. Never the digits. The back-off is iOS's: the 6th wrong try
 * in a row waits 1 min, the 7th 5 min, the 8th 15 min, the 9th and later 1 h; a try during a wait is refused and
 * not counted. The record (pass_pack / pass_load, PASS_REC in the state slot) keeps the tries and the wait across a
 * restart: the wait goes on from the wall clock when both ends know it, else it starts again in full at the load
 * (a restart never shortens a wait). There is no wipe. */
#ifndef SBOS_PASSCODE_H
#define SBOS_PASSCODE_H
#include <stdint.h>

#define PASS_REC "sbos.pass"            /* the state slot's record (settings.c) */
#define PASS_REC_LEN 76U                 /* its bytes */
#define PASS_ROUNDS 20000U               /* SHA-256 rounds: est. ~46 ms on the A55 at 1.8 GHz (host/passcode-test.c) */
#define PASS_SALT 16U

void pass_sha256(const void *p, uint32_t n, uint8_t out[32]);
/* the same SHA-256 in pieces (update.c: an image a chunk at a time) */
struct pass_sha { uint32_t h[8]; uint64_t len; uint32_t n; uint8_t buf[64]; };
void pass_sha_init(struct pass_sha *c);
void pass_sha_update(struct pass_sha *c, const void *p, uint64_t n);
void pass_sha_final(struct pass_sha *c, uint8_t out[32]);
void pass_reset(void);                                                  /* RAM: no passcode, no tries */
int pass_len(void);                                                     /* 4 or 6; 0 none */
int pass_fails(void);                                                   /* wrong tries in a row */
uint32_t pass_backoff_s(int fails);                                     /* the wait after this many (0 none) */
uint32_t pass_wait(uint64_t now_ms);                                    /* s until a try is taken (0 now) */
int pass_check(const char *digits, uint64_t now_ms, int64_t utc);       /* 1 right (or none set), 0 wrong, -N wait N s */
/* set (old "" when none), change, or remove (nw ""): 0 done, -1 refused (old wrong: counted as a wrong try; a wait;
 * nw not 4 or 6 digits) */
int pass_set(const char *old, const char *nw, const uint8_t salt[PASS_SALT], uint64_t now_ms, int64_t utc);
int pass_pack(uint8_t rec[PASS_REC_LEN], uint64_t now_ms, int64_t utc);   /* bytes to keep, 0 nothing to keep */
int pass_load(const uint8_t *rec, int n, uint64_t now_ms, int64_t utc);  /* 1 taken, 0 not valid (nothing changed) */

#endif
