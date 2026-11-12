/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS prefs and blobs (Track A, sbos-core-api.md 1 and 2): a key/value store and named blobs, kept in the
 * state slot with the settings (settings.c adds our records to its save and hands us the newest half at boot).
 *
 * In RAM (one block from prefs_init, so the image carries no 224 KiB of zeros):
 *   - prefs: packed entries [klen u8][key][vlen u8][value], PREFS_BYTES at most, PREFS_KEYS keys;
 *   - blobs: a small table (name, offset, size) over a packed arena of PREFS_BLOB_BYTES.
 * In the slot (each record <= STATE_REC_MAX):
 *   - "prefs0".."prefsN": version byte 1, then the packed entries, cut into 4 KiB pieces;
 *   - "blob:<name>", then "blob:<name>#1".."#7": the blob's bytes in 4 KiB pieces (a piece under 4 KiB is the
 *     last). The half's CRC covers everything; on top of it the entries are checked again at load, and a store
 *     that does not parse starts empty rather than half-read.
 * A save adds all of the prefs or none of them, and each blob whole or not at all (never a cut blob).
 * Pure C: no libc, no hardware (the host test runs it as is). */
#include "kern.h"

#define PREFS_VER 1U
#define NPIECES ((1U + PREFS_BYTES + STATE_REC_MAX - 1U) / STATE_REC_MAX)

struct blob {
    char name[PREFS_BLOB_NAME + 1];
    uint32_t off, n;
};

static uint8_t *pb;                         /* the packed prefs */
static uint32_t pn;                         /* bytes used */
static int nkeys;
static uint8_t *bb;                         /* the blobs' arena */
static uint32_t bn;
static struct blob bt[PREFS_BLOBS];
static int nblobs;

static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) ++n; return n; }
static void mcpy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }
static void mmove(uint8_t *d, const uint8_t *s, uint32_t n)
{
    if (d < s) while (n--) *d++ = *s++;
    else { d += n; s += n; while (n--) *--d = *--s; }
}
static int meq(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = a, *y = b;
    for (uint32_t i = 0; i < n; ++i) if (x[i] != y[i]) return 0;
    return 1;
}

static int name_ok(const char *k, uint32_t max)
{
    uint32_t n = 0;
    if (!k || !k[0]) return 0;
    for (; k[n]; ++n) {
        char c = k[n];
        if (n >= max || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}
int prefs_key_ok(const char *k) { return name_ok(k, PREFS_KEY_MAX); }

void prefs_init(void *mem)
{
    pb = mem;
    bb = mem ? (uint8_t *)mem + PREFS_BYTES : 0;
    pn = bn = 0;
    nkeys = nblobs = 0;
}

void prefs_reset(void)
{
    pn = bn = 0;
    nkeys = nblobs = 0;
}

/* ---- prefs -------------------------------------------------------------------------------------------------- */
/* the entry of key at *at (its offset): 1 found */
static int pfind(const char *key, uint32_t *at)
{
    uint32_t kl = slen(key);
    for (uint32_t o = 0; o < pn;) {
        uint32_t l = pb[o];
        if (l == kl && meq(pb + o + 1, key, kl)) {
            *at = o;
            return 1;
        }
        o += 1U + l + 1U + pb[o + 1U + l];
    }
    return 0;
}
static uint32_t esize(uint32_t o) { return 1U + pb[o] + 1U + pb[o + 1U + pb[o]]; }

int prefs_get(const char *key, char *val, int cap)
{
    uint32_t o;
    if (!pb || !prefs_key_ok(key) || !pfind(key, &o)) return -1;
    const uint8_t *v = pb + o + 1U + pb[o];
    int n = v[0];
    if (val && cap > 0) {
        int k = n < cap - 1 ? n : cap - 1;
        mcpy(val, v + 1, (uint32_t)k);
        val[k] = 0;
    }
    return n;
}

int prefs_set(const char *key, const char *val)
{
    if (!pb || !prefs_key_ok(key)) return -1;
    uint32_t o, kl = slen(key), vl = val ? slen(val) : 0, old = 0;
    int have = pfind(key, &o);
    if (val && vl > PREFS_VAL_MAX) return -1;
    if (have) {
        old = esize(o);
        const uint8_t *v = pb + o + 1U + kl;
        if (val && v[0] == vl && meq(v + 1, val, vl)) return 0;     /* the same: nothing to save */
    }
    if (!val) {
        if (!have) return 0;
        mmove(pb + o, pb + o + old, pn - o - old);
        pn -= old;
        --nkeys;
        return 1;
    }
    uint32_t need = 2U + kl + vl;
    if (pn - old + need > PREFS_BYTES || (!have && nkeys >= PREFS_KEYS)) return -1;   /* full: the old value stays */
    if (have) {
        mmove(pb + o, pb + o + old, pn - o - old);
        pn -= old;
    } else
        ++nkeys;
    uint8_t *p = pb + pn;
    p[0] = (uint8_t)kl;
    mcpy(p + 1, key, kl);
    p[1 + kl] = (uint8_t)vl;
    mcpy(p + 2 + kl, val, vl);
    pn += need;
    return 1;
}

/* ---- blobs -------------------------------------------------------------------------------------------------- */
static int bfind(const char *name)
{
    for (int i = 0; i < nblobs; ++i) {
        const char *a = bt[i].name;
        uint32_t k = 0;
        while (a[k] && a[k] == name[k]) ++k;
        if (!a[k] && !name[k]) return i;
    }
    return -1;
}
static void bdel(int i)
{
    uint32_t off = bt[i].off, n = bt[i].n;
    mmove(bb + off, bb + off + n, bn - off - n);
    bn -= n;
    for (int k = i; k + 1 < nblobs; ++k) bt[k] = bt[k + 1];
    --nblobs;
    for (int k = 0; k < nblobs; ++k) if (bt[k].off > off) bt[k].off -= n;
}

int prefs_blob_put(const char *name, const void *p, uint32_t n)
{
    if (!bb || !name_ok(name, PREFS_BLOB_NAME) || n > PREFS_BLOB_MAX || (n && !p)) return -1;
    int i = bfind(name);
    if (!n) {
        if (i < 0) return 0;
        bdel(i);
        return 1;
    }
    if (i >= 0 && bt[i].n == n && meq(bb + bt[i].off, p, n)) return 0;   /* the same bytes */
    uint32_t old = i >= 0 ? bt[i].n : 0;
    if (bn - old + n > PREFS_BLOB_BYTES || (i < 0 && nblobs >= PREFS_BLOBS)) return -1;   /* the old one stays */
    if (i >= 0) bdel(i);
    struct blob *b = &bt[nblobs++];
    uint32_t k = 0;
    for (; name[k]; ++k) b->name[k] = name[k];
    b->name[k] = 0;
    b->off = bn;
    b->n = n;
    mcpy(bb + bn, p, n);
    bn += n;
    return 1;
}

int prefs_blob_get(const char *name, void *p, uint32_t cap)
{
    if (!bb || !name_ok(name, PREFS_BLOB_NAME)) return -1;
    int i = bfind(name);
    if (i < 0) return -1;
    uint32_t n = bt[i].n < cap ? bt[i].n : cap;
    if (p) mcpy(p, bb + bt[i].off, n);
    return (int)n;
}

void prefs_counts(int *keys, uint32_t *bytes, int *blobs, uint32_t *blob_bytes)
{
    if (keys) *keys = nkeys;
    if (bytes) *bytes = pn;
    if (blobs) *blobs = nblobs;
    if (blob_bytes) *blob_bytes = bn;
}

/* ---- the slot ----------------------------------------------------------------------------------------------- */
static void pid(char *id, uint32_t k)     /* "prefs0" .. "prefs16" */
{
    mcpy(id, "prefs", 5);
    uint32_t i = 5;
    if (k >= 10) id[i++] = (char)('0' + k / 10);
    id[i++] = (char)('0' + k % 10);
    id[i] = 0;
}
static void bid(char *id, const char *name, uint32_t k)   /* "blob:<name>", "blob:<name>#k" */
{
    mcpy(id, "blob:", 5);
    uint32_t i = 5;
    for (; *name; ++name) id[i++] = *name;
    if (k) { id[i++] = '#'; id[i++] = (char)('0' + k); }
    id[i] = 0;
}
static uint32_t rec_bytes(uint32_t n) { return 28U + ((n + 7U) & ~7U); }   /* state.c's record with its id and padding */

int prefs_save(struct state_buf *b)
{
    int r = 0;
    char id[24];
    if (!pb) return 0;
    if (pn) {
        /* the version byte, then the entries: piece k holds stream bytes [k * 4096, (k + 1) * 4096) */
        uint32_t total = 1U + pn, need = 0;
        for (uint32_t o = 0; o < total; o += STATE_REC_MAX)
            need += rec_bytes(total - o < STATE_REC_MAX ? total - o : STATE_REC_MAX);
        if (b->len + need > STATE_HALF) r = -1;
        else {
            static uint8_t piece[STATE_REC_MAX];
            for (uint32_t o = 0, k = 0; o < total; o += STATE_REC_MAX, ++k) {
                uint32_t len = total - o < STATE_REC_MAX ? total - o : STATE_REC_MAX;
                for (uint32_t j = 0; j < len; ++j) piece[j] = o + j ? pb[o + j - 1U] : (uint8_t)PREFS_VER;
                pid(id, k);
                if (state_add(b, id, piece, len) < 0) return -1;     /* cannot happen: the room was counted */
            }
        }
    }
    for (int i = 0; i < nblobs; ++i) {
        uint32_t n = bt[i].n, need = 0;
        for (uint32_t o = 0; o < n; o += STATE_REC_MAX) need += rec_bytes(n - o < STATE_REC_MAX ? n - o : STATE_REC_MAX);
        if (b->len + need > STATE_HALF) { r = -1; continue; }       /* this one whole or not at all */
        for (uint32_t o = 0, k = 0; o < n; o += STATE_REC_MAX, ++k) {
            bid(id, bt[i].name, k);
            if (state_add(b, id, bb + bt[i].off + o, n - o < STATE_REC_MAX ? n - o : STATE_REC_MAX) < 0) return -1;
        }
    }
    return r;
}

/* the entries in pb[0..pn) parse: lengths inside, keys valid, no key twice */
static int pcheck(void)
{
    int n = 0;
    for (uint32_t o = 0; o < pn;) {
        uint32_t kl = pb[o];
        if (kl == 0 || kl > PREFS_KEY_MAX || o + 2U + kl > pn) return -1;
        uint32_t vl = pb[o + 1U + kl];
        if (o + 2U + kl + vl > pn) return -1;
        char k[PREFS_KEY_MAX + 1];
        mcpy(k, pb + o + 1, kl);
        k[kl] = 0;
        if (!prefs_key_ok(k)) return -1;
        for (uint32_t v = 0; v < vl; ++v) if (!pb[o + 2U + kl + v]) return -1;   /* values are C strings */
        for (uint32_t q = 0; q < o;) {
            if (pb[q] == kl && meq(pb + q + 1, k, kl)) return -1;
            q += 2U + pb[q] + pb[q + 1U + pb[q]];
        }
        ++n;
        o += 2U + kl + vl;
    }
    return n;
}

int prefs_restore(const uint8_t *half)
{
    char id[24];
    const uint8_t *d = 0;
    int r = 0;
    prefs_reset();
    if (!pb) return -1;
    /* prefs: the pieces in order until one is short (or the next is missing) */
    pid(id, 0);
    int n = state_find(half, id, &d);
    if (n > 0 && d) {
        if (d[0] != PREFS_VER) r = -1;      /* another version's store: started empty */
        else {
            uint32_t at = 0;
            for (uint32_t k = 0; n > 0 && d && k < NPIECES; ++k) {
                uint32_t skip = k ? 0U : 1U, len = (uint32_t)n - skip;
                if (at + len > PREFS_BYTES) { r = -1; break; }
                mcpy(pb + at, d + skip, len);
                at += len;
                if ((uint32_t)n < STATE_REC_MAX) break;
                pid(id, k + 1);
                n = state_find(half, id, &d);
            }
            pn = r ? 0 : at;
            int keys = r ? -1 : pcheck();
            if (keys < 0 || keys > PREFS_KEYS) { pn = 0; r = -1; }
            else nkeys = keys;
        }
    }
    /* blobs: every "blob:<name>" record (not a "#k" piece), then its pieces */
    uint32_t at = 0;
    char rid[24];
    const uint8_t *rd;
    uint32_t rn;
    while (state_next(half, &at, rid, &rd, &rn)) {
        if (!meq(rid, "blob:", 5)) continue;
        const char *name = rid + 5;
        int piece = 0;
        for (const char *c = name; *c; ++c) if (*c == '#') piece = 1;
        if (piece) continue;
        if (!name_ok(name, PREFS_BLOB_NAME) || bfind(name) >= 0 || nblobs >= PREFS_BLOBS) { r = -1; continue; }
        uint32_t start = bn, total = 0;
        int ok = 1;
        const uint8_t *q = rd;
        uint32_t qn = rn;
        for (uint32_t k = 0;; ++k) {
            if (total + qn > PREFS_BLOB_MAX || start + total + qn > PREFS_BLOB_BYTES) { ok = 0; break; }
            mcpy(bb + start + total, q, qn);
            total += qn;
            if (qn < STATE_REC_MAX || k + 1 >= PREFS_BLOB_MAX / STATE_REC_MAX) break;
            bid(id, name, k + 1);
            int m = state_find(half, id, &q);
            if (m < 0 || !q) break;             /* exactly 4 KiB pieces: the last one was full */
            qn = (uint32_t)m;
        }
        if (!ok || !total) { r = -1; continue; }
        struct blob *b = &bt[nblobs++];
        uint32_t k = 0;
        for (; name[k]; ++k) b->name[k] = name[k];
        b->name[k] = 0;
        b->off = start;
        b->n = total;
        bn += total;
    }
    return r;
}
