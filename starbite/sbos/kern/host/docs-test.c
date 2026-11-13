/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/docs.c through kern/state.c (Track A, 8a: Notes' and Contacts' documents). -v lists every check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kern.h"

static int nchk, nfail, verbose;
static void chk(int ok, const char *what)
{
    nchk++;
    if (!ok) { nfail++; printf("FAIL: %s\n", what); }
    else if (verbose) printf("ok: %s\n", what);
}

static uint8_t *mem, *half;

static void fill(uint8_t *p, uint32_t n, uint32_t seed)
{
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seed * 131u + i * 7u + (i >> 8));
}

/* save everything into a fresh half and restore it into the store again */
static int roundtrip(uint32_t cap)
{
    struct state_buf b;
    state_begin(&b, half, 7);
    int r = docs_save(&b);
    if (cap && b.len > cap) return -2;
    (void)state_finish(&b);
    docs_reset();
    return r < 0 ? r : docs_restore(half);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    mem = malloc(DOCS_MEM);
    half = malloc(STATE_HALF);
    static uint8_t a[DOC_MAX + 16], g[DOC_MAX + 16];

    chk(docs_put(DOC_NOTES, 1, "x", 1) < 0, "before docs_init every call refuses");
    docs_init(mem);
    chk(docs_count(DOC_NOTES) == 0 && docs_count(DOC_CONTACTS) == 0, "empty at the start");
    chk(docs_put(DOC_NOTES, 0, "x", 1) < 0, "id 0 refused");
    chk(docs_put(2, 1, "x", 1) < 0, "a third collection refused");
    chk(docs_put(DOC_NOTES, 1, a, DOC_MAX + 1) < 0, "over DOC_MAX refused");

    fill(a, 100, 1);
    chk(docs_put(DOC_NOTES, 5, a, 100) == 1, "put a note");
    chk(docs_put(DOC_NOTES, 5, a, 100) == 0, "the same again: unchanged");
    chk(docs_get(DOC_NOTES, 5, g, sizeof g) == 100 && !memcmp(a, g, 100), "get it back");
    chk(docs_get(DOC_CONTACTS, 5, g, sizeof g) < 0, "collections are apart");
    fill(a, 300, 2);
    chk(docs_put(DOC_CONTACTS, 3, a, 300) == 1 && docs_put(DOC_CONTACTS, 9, a, 30) == 1, "two contacts");
    chk(docs_id_at(DOC_CONTACTS, 0) == 3 && docs_id_at(DOC_CONTACTS, 1) == 9 && docs_id_at(DOC_CONTACTS, 2) == 0, "ids in order");
    chk(docs_new_id(DOC_CONTACTS) == 10 && docs_new_id(DOC_NOTES) == 6, "new ids");

    /* replacing a document in the middle keeps the others intact */
    fill(a, 5000, 3);
    chk(docs_put(DOC_CONTACTS, 3, a, 5000) == 1, "replace with a bigger one");
    chk(docs_get(DOC_CONTACTS, 9, g, sizeof g) == 30, "the other one kept");
    fill(a, 300, 2);
    chk(!memcmp(g, a, 30), "the other one's bytes kept");
    uint32_t used = 0, budget = 0;
    docs_usage(DOC_CONTACTS, &used, &budget);
    chk(used == 5030 && budget == DOC_BUDGET_CONTACTS, "usage counts the bytes");

    /* the budget: the old document stays when a put would not fit */
    docs_reset();
    uint32_t k = 0;
    for (; k < 10; k++) { fill(a, DOC_MAX, k); if (docs_put(DOC_NOTES, k + 1, a, DOC_MAX) < 0) break; }
    chk(k == DOC_BUDGET_NOTES / DOC_MAX, "notes fill up to the budget");
    chk(docs_count(DOC_NOTES) == (int)k, "the refused one is not there");
    fill(a, 10, 99);
    chk(docs_put(DOC_NOTES, 1, a, 10) == 1, "a smaller replacement still fits");
    chk(docs_put(DOC_NOTES, 1, 0, 0) == 1 && docs_get(DOC_NOTES, 1, g, 1) < 0, "delete");

    /* the slot: a round trip keeps every document, big ones in pieces */
    docs_reset();
    fill(a, DOC_MAX, 11);
    chk(docs_put(DOC_NOTES, 7, a, DOC_MAX) == 1, "a full-size note");
    fill(g, 4091, 12);
    chk(docs_put(DOC_NOTES, 8, g, 4091) == 1, "a note that fills its first record exactly");
    chk(docs_put(DOC_CONTACTS, 2, "Ada", 3) == 1, "a small contact");
    chk(roundtrip(0) == 0, "save and restore");
    chk(docs_count(DOC_NOTES) == 2 && docs_count(DOC_CONTACTS) == 1, "all documents back");
    static uint8_t h[DOC_MAX];
    chk(docs_get(DOC_NOTES, 7, h, sizeof h) == (int)DOC_MAX && !memcmp(h, a, DOC_MAX), "the big note byte for byte");
    chk(docs_get(DOC_NOTES, 8, h, sizeof h) == 4091 && !memcmp(h, g, 4091), "the exact-fit note");
    chk(docs_get(DOC_CONTACTS, 2, h, sizeof h) == 3 && !memcmp(h, "Ada", 3), "the contact");

    /* a broken document (a piece missing) is dropped, never half-read */
    {
        struct state_buf b;
        state_begin(&b, half, 8);
        uint8_t first[5 + 10] = { 1, 0, 0x20, 0, 0 };   /* says 8192 bytes, carries 10, and no pieces follow */
        (void)state_add(&b, "d0:00000004", first, sizeof first);
        (void)state_add(&b, "d1:00000002", (const uint8_t[]){ 1, 3, 0, 0, 0, 'B', 'o', 'b' }, 8);
        (void)state_finish(&b);
        chk(docs_restore(half) < 0, "a broken document reported");
        chk(docs_get(DOC_NOTES, 4, h, sizeof h) < 0, "the broken one left out");
        chk(docs_get(DOC_CONTACTS, 2, h, sizeof h) == 3 && !memcmp(h, "Bob", 3), "the good one taken");
    }

    /* a nearly full half: a document that does not fit is left out whole */
    {
        docs_reset();
        fill(a, DOC_MAX, 21);
        (void)docs_put(DOC_NOTES, 1, a, DOC_MAX);
        struct state_buf b;
        state_begin(&b, half, 9);
        static uint8_t pad[STATE_REC_MAX];
        while (b.len + STATE_REC_MAX + 64 < STATE_HALF - 20000) (void)state_add(&b, "pad", pad, sizeof pad);
        chk(docs_save(&b) < 0, "a save that does not fit says so");
        uint32_t at = 0, n;
        char id[24];
        const uint8_t *d;
        int pieces = 0;
        (void)state_finish(&b);
        while (state_next(half, &at, id, &d, &n)) if (id[0] == 'd' && id[2] == ':') pieces++;
        chk(pieces == 0, "no piece of the cut document in the half");
    }

    printf("docs-test: %d checks, %d failed\n", nchk, nfail);
    free(mem);
    free(half);
    return nfail != 0;
}
