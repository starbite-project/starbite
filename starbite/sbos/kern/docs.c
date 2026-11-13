/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS documents (Track A, 8a; design-ref's Notes and Contacts specs: "Contacts has no store at all", Notes
 * kept 8 x 600 bytes): two collections of opaque documents, each with a stable id, kept in the state slot with the
 * settings, as prefs.c keeps its blobs (settings.c adds our records to its save and hands us the newest half at boot).
 *
 *   collections  DOC_NOTES (budget DOC_BUDGET_NOTES) and DOC_CONTACTS (DOC_BUDGET_CONTACTS); a document is up to
 *                DOC_MAX bytes, in the app's own format (the kernel never reads it); ids 1.. (0 is "none")
 *   in RAM       one block from docs_init: each collection's region packed, a table of (collection, id, off, n)
 *   in the slot  "d<c>:<id, 8 hex>" holds version 1, the length (u32 LE) and the first bytes; "...#1" .. "#7" the rest
 *                in 4 KiB pieces. A save adds each document whole or not at all; a restore takes a document only
 *                when its pieces add up to its length, so a broken one is dropped, never half-read.
 * Pure C: no libc, no hardware (kern/host/docs-test.c runs it as is). */
#include "kern.h"

#define DOC_VER 1U
#define DOC_HEAD 5U                         /* version (1) + length (4) */

struct dent {
    uint32_t id, off, n;
    uint8_t c, used;
};

static uint8_t *db;                         /* the regions: notes first, then contacts */
static struct dent dt[DOC_ENTRIES];
static uint32_t dused[DOC_COLLS];
static const uint32_t dbase[DOC_COLLS] = { 0, DOC_BUDGET_NOTES };
static const uint32_t dbudget[DOC_COLLS] = { DOC_BUDGET_NOTES, DOC_BUDGET_CONTACTS };

static void mcpy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static int meq(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = a, *y = b;
    for (uint32_t i = 0; i < n; ++i) if (x[i] != y[i]) return 0;
    return 1;
}

void docs_init(void *mem)
{
    db = mem;
    docs_reset();
}

void docs_reset(void)
{
    for (int i = 0; i < DOC_ENTRIES; ++i) dt[i].used = 0;
    for (int c = 0; c < DOC_COLLS; ++c) dused[c] = 0;
}

static int ok_c(int c) { return db && c >= 0 && c < DOC_COLLS; }

static struct dent *find(int c, uint32_t id)
{
    for (int i = 0; i < DOC_ENTRIES; ++i)
        if (dt[i].used && dt[i].c == c && dt[i].id == id) return &dt[i];
    return 0;
}

/* the collection's region closes the gap a removed document leaves */
static void drop(struct dent *e)
{
    int c = e->c;
    uint32_t off = e->off, n = e->n, end = dused[c];
    uint8_t *r = db + dbase[c];
    for (uint32_t i = off; i + n < end; ++i) r[i] = r[i + n];
    for (int i = 0; i < DOC_ENTRIES; ++i)
        if (dt[i].used && dt[i].c == c && dt[i].off > off) dt[i].off -= n;
    dused[c] -= n;
    e->used = 0;
}

int docs_put(int c, uint32_t id, const void *p, uint32_t n)
{
    if (!ok_c(c) || !id || n > DOC_MAX || (n && !p)) return -1;
    struct dent *e = find(c, id);
    if (!n) {
        if (!e) return 0;
        drop(e);
        return 1;
    }
    if (e && e->n == n && meq(db + dbase[c] + e->off, p, n)) return 0;
    uint32_t have = dused[c] - (e ? e->n : 0);
    if (have + n > dbudget[c]) return -1;   /* full: the old document stays */
    if (!e) {
        for (int i = 0; i < DOC_ENTRIES && !e; ++i)
            if (!dt[i].used) e = &dt[i];
        if (!e) return -1;
    } else
        drop(e);
    e->used = 1;
    e->c = (uint8_t)c;
    e->id = id;
    e->off = dused[c];
    e->n = n;
    mcpy(db + dbase[c] + e->off, p, n);
    dused[c] += n;
    return 1;
}

int docs_get(int c, uint32_t id, void *p, uint32_t cap)
{
    if (!ok_c(c)) return -1;
    struct dent *e = find(c, id);
    if (!e) return -1;
    uint32_t k = e->n < cap ? e->n : cap;
    if (p) mcpy(p, db + dbase[c] + e->off, k);
    return (int)e->n;
}

int docs_count(int c)
{
    int n = 0;
    if (!ok_c(c)) return 0;
    for (int i = 0; i < DOC_ENTRIES; ++i) if (dt[i].used && dt[i].c == c) ++n;
    return n;
}

/* the k-th document's id, in id order (0: none) */
uint32_t docs_id_at(int c, int k)
{
    if (!ok_c(c) || k < 0) return 0;
    uint32_t last = 0;
    for (int step = 0; step <= k; ++step) {
        uint32_t best = 0;
        for (int i = 0; i < DOC_ENTRIES; ++i)
            if (dt[i].used && dt[i].c == c && dt[i].id > last && (!best || dt[i].id < best)) best = dt[i].id;
        if (!best) return 0;
        last = best;
    }
    return last;
}

uint32_t docs_new_id(int c)
{
    uint32_t m = 0;
    if (!ok_c(c)) return 0;
    for (int i = 0; i < DOC_ENTRIES; ++i) if (dt[i].used && dt[i].c == c && dt[i].id > m) m = dt[i].id;
    return m + 1 ? m + 1 : 0;
}

void docs_usage(int c, uint32_t *used, uint32_t *budget)
{
    if (used) *used = ok_c(c) ? dused[c] : 0;
    if (budget) *budget = ok_c(c) ? dbudget[c] : 0;
}

/* ---- the slot ----------------------------------------------------------------------------------------- */
static void rec_id(char id[24], int c, uint32_t doc, unsigned piece)
{
    static const char hx[] = "0123456789abcdef";
    int n = 0;
    id[n++] = 'd';
    id[n++] = (char)('0' + c);
    id[n++] = ':';
    for (int s = 28; s >= 0; s -= 4) id[n++] = hx[(doc >> s) & 0xfU];
    if (piece) {
        id[n++] = '#';
        id[n++] = (char)('0' + piece);
    }
    id[n] = 0;
}

int docs_save(struct state_buf *b)
{
    int r = 0;
    static uint8_t first[STATE_REC_MAX];
    if (!db) return 0;
    for (int i = 0; i < DOC_ENTRIES; ++i) {
        if (!dt[i].used) continue;
        const struct dent *e = &dt[i];
        const uint8_t *p = db + dbase[e->c] + e->off;
        uint32_t mark = b->len, n0 = e->n < STATE_REC_MAX - DOC_HEAD ? e->n : STATE_REC_MAX - DOC_HEAD;
        char id[24];
        first[0] = DOC_VER;
        first[1] = (uint8_t)e->n, first[2] = (uint8_t)(e->n >> 8), first[3] = (uint8_t)(e->n >> 16), first[4] = (uint8_t)(e->n >> 24);
        mcpy(first + DOC_HEAD, p, n0);
        rec_id(id, e->c, e->id, 0);
        int bad = state_add(b, id, first, DOC_HEAD + n0) < 0;
        for (uint32_t at = n0, k = 1; !bad && at < e->n; at += STATE_REC_MAX, ++k) {
            uint32_t m = e->n - at < STATE_REC_MAX ? e->n - at : STATE_REC_MAX;
            rec_id(id, e->c, e->id, k);
            bad = state_add(b, id, p + at, m) < 0;
        }
        if (bad) {
            b->len = mark;                  /* never a cut document */
            r = -1;
        }
    }
    return r;
}

static uint32_t hexv(const char *s, int n, int *ok)
{
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        char ch = s[i];
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
        if (d < 0) { *ok = 0; return 0; }
        v = v << 4 | (uint32_t)d;
    }
    return v;
}

int docs_restore(const uint8_t *half)
{
    int r = 0;
    uint32_t at = 0, n;
    char id[24];
    const uint8_t *d;
    static uint8_t tmp[DOC_MAX];
    if (!db) return -1;
    docs_reset();
    while (state_next(half, &at, id, &d, &n)) {
        if (id[0] != 'd' || id[2] != ':' || id[1] < '0' || id[1] >= '0' + DOC_COLLS || id[11] != 0) continue;
        int ok = 1, c = id[1] - '0';
        uint32_t doc = hexv(id + 3, 8, &ok);
        if (!ok || !doc || n < DOC_HEAD || d[0] != DOC_VER) { r = -1; continue; }
        uint32_t len = (uint32_t)d[1] | (uint32_t)d[2] << 8 | (uint32_t)d[3] << 16 | (uint32_t)d[4] << 24;
        uint32_t got = n - DOC_HEAD;
        if (len > DOC_MAX || got > len) { r = -1; continue; }
        mcpy(tmp, d + DOC_HEAD, got);
        for (unsigned k = 1; got < len && k < 10; ++k) {
            char pid[24];
            const uint8_t *q = 0;
            rec_id(pid, c, doc, k);
            int m = state_find(half, pid, &q);
            if (m <= 0 || !q || got + (uint32_t)m > len) break;
            mcpy(tmp + got, q, (uint32_t)m);
            got += (uint32_t)m;
        }
        if (got != len || docs_put(c, doc, tmp, len) < 0) r = -1;   /* broken or over the budget: left out */
    }
    return r;
}
