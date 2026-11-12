/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/prefs.c (Track A: prefs and blobs) through state.c's slot format: set / get /
 * delete, overwrite, the key and size limits, many keys, a blob over several records, a save and a load through
 * state_begin / state_add / state_finish / state_pick, a broken half falling back to the other, a full half.
 * Run: sh starbite/sbos/kern/host/prefs-test.sh [-v]. Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kern.h"

static int verbose, fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        } else if (verbose) {                                                                                          \
            printf("ok: ");                                                                                            \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static uint8_t *mem, *h0, *h1;

/* settings.c's save in small: other records first, then ours, into half `to` with sequence seq */
static uint32_t save(uint8_t *h, uint64_t seq, int *r)
{
    struct state_buf b;
    state_begin(&b, h, seq);
    state_add(&b, "sbos.settings", "0123456789abcdef", 16);
    *r = prefs_save(&b);
    return state_finish(&b);
}

static void fill(uint8_t *p, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)((i * 2654435761u + seed) >> 13);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    mem = malloc(PREFS_MEM);
    h0 = calloc(1, STATE_HALF);
    h1 = calloc(1, STATE_HALF);
    char v[300];

    /* before prefs_init: every call refuses */
    CHECK(prefs_set("a", "1") == -1 && prefs_get("a", v, sizeof v) == -1 && prefs_blob_put("x", "1", 1) == -1,
          "no store yet: refused");
    prefs_init(mem);

    /* set / get / delete */
    CHECK(prefs_get("display.bright", v, sizeof v) == -1, "a missing key: -1");
    CHECK(prefs_set("display.bright", "3") == 1, "set: changed");
    CHECK(prefs_get("display.bright", v, sizeof v) == 1 && !strcmp(v, "3"), "get: 3");
    CHECK(prefs_set("display.bright", "3") == 0, "the same value: nothing to save");
    CHECK(prefs_set("display.bright", "4") == 1 && prefs_get("display.bright", v, sizeof v) == 1 && !strcmp(v, "4"),
          "overwrite: 4");
    CHECK(prefs_set("notif.phone.allow", "0") == 1, "a second key");
    CHECK(prefs_set("display.bright", "2") == 1, "overwrite the first again (it moves to the end)");
    CHECK(prefs_get("notif.phone.allow", v, sizeof v) == 1 && !strcmp(v, "0") &&
          prefs_get("display.bright", v, sizeof v) == 1 && !strcmp(v, "2"), "both intact after the move");
    CHECK(prefs_set("display.bright", 0) == 1 && prefs_get("display.bright", v, sizeof v) == -1, "delete");
    CHECK(prefs_set("display.bright", 0) == 0, "delete a missing key: nothing");
    CHECK(prefs_set("x", "") == 1 && prefs_get("x", v, sizeof v) == 0 && v[0] == 0, "an empty value is a value");
    CHECK(prefs_get("notif.phone.allow", v, 1) == 1 && v[0] == 0, "cap 1: length, nothing copied");
    {
        char big[256];
        memset(big, 'v', 255);
        big[255] = 0;
        CHECK(prefs_set("long", big) == 1 && prefs_get("long", v, sizeof v) == 255 && !strcmp(v, big), "255 bytes");
        char small[8];
        CHECK(prefs_get("long", small, sizeof small) == 255 && !strcmp(small, "vvvvvvv"), "a short buffer: cut, 0-ended");
        char over[257];
        memset(over, 'w', 256);
        over[256] = 0;
        CHECK(prefs_set("long", over) == -1 && prefs_get("long", v, sizeof v) == 255, "256 bytes refused, old kept");
    }

    /* bad keys */
    CHECK(prefs_set("", "1") == -1, "empty key");
    CHECK(prefs_set(0, "1") == -1, "no key");
    CHECK(prefs_set("Display", "1") == -1, "capitals");
    CHECK(prefs_set("a b", "1") == -1, "a space");
    CHECK(prefs_set("a/b", "1") == -1 && prefs_set("a#1", "1") == -1 && prefs_set("caf\xc3\xa9", "1") == -1, "/, #, UTF-8");
    CHECK(prefs_set("abcdefghijklmnopqrstuvwxyz01234", "1") == 1, "31 characters");
    CHECK(prefs_set("abcdefghijklmnopqrstuvwxyz012345", "1") == -1, "32 characters refused");
    CHECK(prefs_set("a-b_c.d9", "1") == 1, "all the allowed characters");
    CHECK(prefs_get("BAD", v, sizeof v) == -1, "get with a bad key: -1");

    /* many keys: 400, then the 401st refused; values kept */
    prefs_reset();
    int ok = 1;
    for (int i = 0; i < PREFS_KEYS; i++) {
        char k[32], val[32];
        snprintf(k, sizeof k, "k.%d", i);
        snprintf(val, sizeof val, "value %d", i * 7);
        ok &= prefs_set(k, val) == 1;
    }
    CHECK(ok, "400 keys");
    CHECK(prefs_set("k.400", "x") == -1, "the 401st key refused");
    CHECK(prefs_set("k.17", "changed") == 1, "an existing key still changes when the count is full");
    ok = 1;
    for (int i = 0; i < PREFS_KEYS; i++) {
        char k[32], val[32];
        snprintf(k, sizeof k, "k.%d", i);
        snprintf(val, sizeof val, "value %d", i * 7);
        ok &= prefs_get(k, v, sizeof v) > 0 && !strcmp(v, i == 17 ? "changed" : val);
    }
    CHECK(ok, "all 400 read back");

    /* the byte limit: 255-byte values fill PREFS_BYTES before 400 keys */
    prefs_reset();
    {
        char big[256];
        memset(big, 'z', 255);
        big[255] = 0;
        int n = 0;
        char k[32];
        for (;; n++) {
            snprintf(k, sizeof k, "big.%d", n);
            if (prefs_set(k, big) != 1) break;
        }
        int keys;
        uint32_t bytes;
        prefs_counts(&keys, &bytes, 0, 0);
        CHECK(n == keys && bytes <= PREFS_BYTES && bytes + 2 + strlen(k) + 255 > PREFS_BYTES,
              "full by bytes after %d keys (%u bytes)", n, bytes);
    }

    /* blobs */
    prefs_reset();
    static uint8_t a[PREFS_BLOB_MAX + 1], b[PREFS_BLOB_MAX + 1];
    fill(a, sizeof a, 1);
    CHECK(prefs_blob_get("notes", b, sizeof b) == -1, "no blob: -1");
    CHECK(prefs_blob_put("notes", a, 1000) == 1 && prefs_blob_get("notes", b, sizeof b) == 1000 && !memcmp(a, b, 1000),
          "a small blob");
    CHECK(prefs_blob_put("notes", a, 1000) == 0, "the same bytes: nothing to save");
    CHECK(prefs_blob_get("notes", b, 10) == 10, "a smaller buffer: cut");
    CHECK(prefs_blob_put("big", a, PREFS_BLOB_MAX) == 1, "32 KiB");
    CHECK(prefs_blob_put("toobig", a, PREFS_BLOB_MAX + 1) == -1, "32 KiB + 1 refused");
    CHECK(prefs_blob_put("abcdefghijklmnopq", a, 4) == -1 && prefs_blob_put("abcdefghijklmnop", a, 4) == 1,
          "names up to 16");
    CHECK(prefs_blob_put("Bad", a, 4) == -1 && prefs_blob_put("a#1", a, 4) == -1, "bad names");
    fill(a, sizeof a, 2);
    CHECK(prefs_blob_put("notes", a, 2000) == 1 && prefs_blob_get("notes", b, sizeof b) == 2000 && !memcmp(a, b, 2000),
          "replaced, larger");
    fill(a, sizeof a, 1);
    CHECK(prefs_blob_get("big", b, sizeof b) == PREFS_BLOB_MAX && !memcmp(a, b, PREFS_BLOB_MAX), "the other blob intact");
    CHECK(prefs_blob_put("abcdefghijklmnop", 0, 0) == 1 && prefs_blob_get("abcdefghijklmnop", b, 4) == -1, "delete");
    {
        int ok2 = 1, n = 0;
        uint32_t bb;
        for (; n < 40; n++) {
            char nm[16];
            snprintf(nm, sizeof nm, "fill%d", n);
            if (prefs_blob_put(nm, a, PREFS_BLOB_MAX) != 1) break;
        }
        prefs_counts(0, 0, 0, &bb);
        ok2 = bb <= PREFS_BLOB_BYTES;
        CHECK(ok2 && n < 40, "the blob arena fills (%d more, %u bytes) and refuses", n, bb);
        CHECK(prefs_blob_put("notes", a, 100) == 1, "a smaller replacement still fits");
        for (int i = 0; i < n; i++) {
            char nm[16];
            snprintf(nm, sizeof nm, "fill%d", i);
            prefs_blob_put(nm, 0, 0);
        }
    }

    /* save and load: prefs in several records, a blob in nine pieces (32 KiB = 8 full ones), a blob of exactly 4 KiB,
     * one of 4 KiB + 1 */
    prefs_reset();
    char k[32], val[256];
    for (int i = 0; i < 300; i++) {
        snprintf(k, sizeof k, "many.%03d", i);
        memset(val, 'a' + i % 26, 100);
        val[100] = 0;
        prefs_set(k, val);
    }
    uint32_t pbytes;
    prefs_counts(0, &pbytes, 0, 0);
    CHECK(pbytes > 3 * STATE_REC_MAX, "the prefs need several records (%u bytes)", pbytes);
    fill(a, sizeof a, 7);
    prefs_blob_put("big", a, PREFS_BLOB_MAX);
    prefs_blob_put("four", a + 5, STATE_REC_MAX);
    prefs_blob_put("fourplus", a + 9, STATE_REC_MAX + 1);
    prefs_blob_put("notes", "\x01\x00", 2);
    int r;
    save(h0, 1, &r);
    CHECK(r == 0, "saved everything");
    CHECK(state_valid(h0, 0), "a valid half");
    const uint8_t *d;
    CHECK(state_find(h0, "prefs0", &d) == (int)STATE_REC_MAX && d[0] == 1, "prefs0: full, version 1");
    CHECK(state_find(h0, "blob:big", &d) == (int)STATE_REC_MAX && state_find(h0, "blob:big#7", &d) == (int)STATE_REC_MAX,
          "blob:big in 8 pieces");
    CHECK(state_find(h0, "blob:fourplus#1", &d) == 1, "4 KiB + 1: a 1-byte second piece");
    CHECK(state_find(h0, "blob:four#1", &d) == -1, "exactly 4 KiB: one piece");
    prefs_reset();
    CHECK(prefs_get("many.000", v, sizeof v) == -1, "reset: empty");
    CHECK(state_pick(h0, h1) == 0, "pick half 0 (half 1 empty)");
    CHECK(prefs_restore(h0) == 0, "restored without a complaint");
    ok = 1;
    for (int i = 0; i < 300; i++) {
        snprintf(k, sizeof k, "many.%03d", i);
        memset(val, 'a' + i % 26, 100);
        val[100] = 0;
        ok &= prefs_get(k, v, sizeof v) == 100 && !strcmp(v, val);
    }
    CHECK(ok, "300 keys back");
    CHECK(prefs_blob_get("big", b, sizeof b) == PREFS_BLOB_MAX && !memcmp(a, b, PREFS_BLOB_MAX), "32 KiB blob back");
    CHECK(prefs_blob_get("four", b, sizeof b) == STATE_REC_MAX && !memcmp(a + 5, b, STATE_REC_MAX), "4 KiB blob back");
    CHECK(prefs_blob_get("fourplus", b, sizeof b) == STATE_REC_MAX + 1 && !memcmp(a + 9, b, STATE_REC_MAX + 1),
          "4 KiB + 1 back");
    CHECK(prefs_blob_get("notes", b, sizeof b) == 2 && b[0] == 1, "a 2-byte blob back");
    {
        int keys, blobs;
        prefs_counts(&keys, 0, &blobs, 0);
        CHECK(keys == 300 && blobs == 4, "counts after the load: %d keys, %d blobs", keys, blobs);
    }

    /* the next save goes to the other half with a higher sequence; corrupt it: the older half wins */
    prefs_set("many.000", "newer");
    prefs_blob_put("notes", "\x01\x01", 2);
    save(h1, 2, &r);
    CHECK(state_pick(h0, h1) == 1, "the newer half picked");
    prefs_restore(h1);
    CHECK(prefs_get("many.000", v, sizeof v) == 5 && !strcmp(v, "newer"), "the newer value");
    h1[100] ^= 0x40;                        /* inside the records: the CRC fails */
    CHECK(state_pick(h0, h1) == 0, "a broken newer half: the older one picked");
    prefs_restore(h0);
    CHECK(prefs_get("many.000", v, sizeof v) == 100 && v[0] == 'a', "the older value back");
    CHECK(prefs_blob_get("notes", b, sizeof b) == 2 && b[1] == 0, "the older blob back");
    h0[64] ^= 1;
    CHECK(state_pick(h0, h1) == -1, "both broken: nothing (a fresh start)");
    h0[64] ^= 1;
    h1[100] ^= 0x40;

    /* a record with a broken entry (valid CRC): the prefs start empty, blobs still load */
    {
        struct state_buf sb;
        uint8_t bad[8] = { 1, 3, 'a', 'B', 'c', 1, 'x', 0 };   /* version, then a key with a capital */
        state_begin(&sb, h1, 9);
        state_add(&sb, "prefs0", bad, 7);
        state_add(&sb, "blob:ok", "hi", 2);
        state_add(&sb, "blob:ok#1", "zz", 2);      /* a stray piece after a short one: ignored */
        state_finish(&sb);
        CHECK(prefs_restore(h1) == -1, "a broken entry: reported");
        int keys;
        prefs_counts(&keys, 0, 0, 0);
        CHECK(keys == 0, "the prefs empty, not half-read");
        CHECK(prefs_blob_get("ok", b, sizeof b) == 2 && !memcmp(b, "hi", 2), "the blob still back (its short piece ends it)");
        uint8_t other[4] = { 2, 1, 'a', 0 };      /* another version */
        state_begin(&sb, h1, 10);
        state_add(&sb, "prefs0", other, 4);
        state_finish(&sb);
        CHECK(prefs_restore(h1) == -1, "another version: reported, empty");
        uint8_t dup[11] = { 1, 1, 'a', 1, 'x', 1, 'a', 1, 'y' };   /* the same key twice */
        state_begin(&sb, h1, 11);
        state_add(&sb, "prefs0", dup, 9);
        state_finish(&sb);
        CHECK(prefs_restore(h1) == -1, "a key twice: refused");
    }

    /* a half nearly full of other records: the prefs go all or nothing, each blob whole or not at all */
    prefs_restore(h0);
    {
        struct state_buf sb;
        static uint8_t filler[STATE_REC_MAX];
        state_begin(&sb, h1, 20);
        while (sb.len + 28 + STATE_REC_MAX + 60000 <= STATE_HALF) state_add(&sb, "filler", filler, STATE_REC_MAX);
        int rr = prefs_save(&sb);
        state_finish(&sb);
        CHECK(rr == -1, "not everything fits: reported");
        const uint8_t *q;
        int have0 = state_find(h1, "prefs0", &q) > 0, havel = state_find(h1, "prefs7", &q) > 0;
        CHECK(have0 == havel, "the prefs all or nothing (%d %d)", have0, havel);
        int big0 = state_find(h1, "blob:big", &q) > 0, big7 = state_find(h1, "blob:big#7", &q) > 0;
        CHECK(big0 == big7, "the big blob whole or not at all");
        CHECK(state_valid(h1, 0), "the half still valid");
        prefs_restore(h1);
        CHECK(prefs_blob_get("notes", b, sizeof b) == 2, "a small blob after the big one still went in");
    }

    printf("prefs test: %d checks, %d failed\n", checks, fails);
    free(mem);
    free(h0);
    free(h1);
    return fails ? 1 : 0;
}
