/* SPDX-License-Identifier: GPL-2.0-only OR MIT
 * jbd2.c: a read-only, in-memory replay of an ext4 journal (see the top of jbd2.c). */
#ifndef SBM_JBD2_H
#define SBM_JBD2_H
#include <stdint.h>
#include "ext4.h"

#define JBD2_E_NOJOURNAL   -1
#define JBD2_E_CORRUPT     -2
#define JBD2_E_UNSUPPORTED -3
#define JBD2_REVOKE_MAX    4096u
#define JBD2_PENDING_MAX   8192u

struct jbd2_map {
    uint64_t fsblk;         /* the file system block ... */
    uint64_t pblk;          /* ... is read from this partition block instead */
    uint32_t seq;
    uint8_t escaped;        /* its first 4 bytes were the journal magic: put them back (jbd2_fix_escape) */
};
struct jbd2 {
    uint32_t bs, maxlen, first, start, seq0, features;
    uint32_t transactions, blocks, skipped_revoked;
    int dropped_incomplete, replayed;
    struct jbd2_map *m;
    unsigned n, cap;
    const char *why;
};
/* Reads the journal of fs (whose read must not consult a map yet). 0: j->n blocks to overlay (0 = clean). */
int jbd2_scan(struct ext4 *fs, uint32_t jinum, struct jbd2 *j, struct jbd2_map *store, unsigned cap);
const struct jbd2_map *jbd2_find(const struct jbd2 *j, uint64_t fsblk);
void jbd2_fix_escape(uint8_t *block);
/* Bytes [off, off + len) of the file system as it is after the replay: blocks in the map come from the journal
 * (through scratch, one block), the rest from raw(). j = NULL reads raw only. 0 ok. */
int jbd2_read(const struct jbd2 *j, uint32_t bs, int (*raw)(void *, uint64_t, void *, uint32_t), void *ctx,
              uint64_t off, void *dst, uint32_t len, uint8_t *scratch, uint32_t scratch_len);
#endif
