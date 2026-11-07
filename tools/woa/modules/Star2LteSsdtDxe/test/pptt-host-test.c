/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of PpttBuild (test/run.sh): the release's own PPTT (test/release-pptt.bin, from the
 * woa release DXE volume's platform table file, BSD-2-Clause-Patent) with
 *   1. the S9's A55 caches as measured on the phone (L1 I+D per core, one 512 KiB L2 shared by the
 *      cluster, no L3; PrivateLevels 1), written for iasl -d;
 *   2. a deeper layout (private L1 + L2, shared L3; PrivateLevels 2) for the chaining.
 * Checks the result's structure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../PpttBuild.h"

static unsigned fails, checks;
static void check(const char *w, long got, long want)
{
    ++checks;
    if (got != want) { ++fails; printf("FAIL %-40s got %ld want %ld\n", w, got, want); }
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Walk a built table; Levels = number of cache levels, Private = per-core ones. */
static void walk(const char *name, const uint8_t *out, uint32_t len, unsigned levels, unsigned priv,
                 const uint32_t *sizes)
{
    unsigned sum = 0, procs = 0, caches = 0, target = 0, clusters = 0;
    uint32_t shared = 0, clusteroff = 0;
    printf("-- %s\n", name);
    for (uint32_t i = 0; i < len; ++i) sum += out[i];
    check("checksum", sum & 0xff, 0);
    check("length field", rd32(out + 4), len);
    for (uint32_t o = 36; o < len; o += out[o + 1]) {
        if (out[o] == 0) {
            ++procs;
            uint32_t flags = rd32(out + o + 4), id = rd32(out + o + 12), nref = rd32(out + o + 16);
            check("node length", out[o + 1], 20 + 4 * nref);
            if ((flags & 8) && id <= 3) {
                ++target;
                if (!clusteroff) clusteroff = rd32(out + o + 8);
                check("A55 leaves share one cluster", rd32(out + o + 8), clusteroff);
                check("leaf refs (L1 I+D)", nref, 2);
                for (uint32_t r = 0; r < nref; ++r) {
                    uint32_t c = rd32(out + o + 20 + 4 * r);
                    check("ref is a cache", out[c], 1);
                    check("L1 size", rd32(out + c + 12), sizes[0]);
                    for (unsigned lv = 2; lv <= levels; ++lv) {
                        c = rd32(out + c + 8);
                        check("next level is a cache", out[c], 1);
                        check("level size", rd32(out + c + 12), sizes[lv - 1]);
                        if (lv == priv + 1) {
                            if (!shared) shared = c;
                            check("one shared cache for the cluster", c, shared);
                        }
                    }
                    check("last level ends the chain", rd32(out + c + 8), 0);
                }
            } else if (flags & 8) {
                check("M3 leaf untouched", nref, 0);
            }
            uint32_t parent = rd32(out + o + 8);
            if (parent) check("parent is a node", out[parent], 0);
        } else if (out[o] == 1) {
            ++caches;
            check("cache length (rev 3)", out[o + 1], 28);
            check("cache flags (all valid)", rd32(out + o + 4), 0xff);
        }
    }
    /* the cluster node lists the shared cache(s) */
    check("cluster is a node", out[clusteroff], 0);
    uint32_t cref = rd32(out + clusteroff + 16);
    check("cluster refs = shared levels", cref, levels - priv);
    if (cref) check("cluster ref = the shared cache", rd32(out + clusteroff + 20), shared);
    for (uint32_t o = 36; o < len; o += out[o + 1])
        if (out[o] == 0 && o != clusteroff && !(rd32(out + o + 4) & 8)) check("other non-leaf untouched", rd32(out + o + 16), 0);
    (void)clusters;
    check("processor nodes", procs, 11);
    check("A55 leaves", target, 4);
    check("caches", caches, 4 * (priv == 1 ? 2 : 3) + (levels - priv));
}

int main(int argc, char **argv)
{
    uint8_t in[4096], out[4096], out2[4096];
    FILE *f = fopen(argc > 1 ? argv[1] : "release-pptt.bin", "rb");
    size_t n = fread(in, 1, sizeof in, f);
    fclose(f);
    const char *why;

    /* 1. measured on the phone (zone 0 of combo1, 2026-11-07) */
    const PB_CACHE s9[] = {
        { 1, PB_DATA, 64, 128, 4 }, { 1, PB_INSTRUCTION, 64, 128, 4 }, { 2, PB_UNIFIED, 64, 512, 16 },
    };
    const uint32_t s9sizes[] = { 32768, 524288 };
    uint32_t len = PpttAddCaches(in, (uint32_t)n, s9, 3, 1, out, sizeof out, &why);
    if (!len) { printf("FAIL build: %s\n", why); return 1; }
    walk("S9 A55: L1 per core, L2 shared", out, len, 2, 1, s9sizes);

    /* 2. private L1 + L2, shared L3 */
    const PB_CACHE deep[] = {
        { 1, PB_DATA, 64, 128, 4 }, { 1, PB_INSTRUCTION, 64, 128, 4 },
        { 2, PB_UNIFIED, 64, 256, 4 }, { 3, PB_UNIFIED, 64, 512, 16 },
    };
    const uint32_t deepsizes[] = { 32768, 65536, 524288 };
    uint32_t len2 = PpttAddCaches(in, (uint32_t)n, deep, 4, 2, out2, sizeof out2, &why);
    if (!len2) { printf("FAIL build 2: %s\n", why); return 1; }
    walk("private L1+L2, shared L3", out2, len2, 3, 2, deepsizes);

    /* refusals */
    uint8_t scratch[4096];
    check("refuses its own output", PpttAddCaches(out, len, s9, 3, 1, scratch, sizeof scratch, &why), 0);
    PB_CACHE bad = { 1, PB_DATA, 0, 128, 4 };
    check("refuses line size 0", PpttAddCaches(in, (uint32_t)n, &bad, 1, 1, scratch, sizeof scratch, &why), 0);
    check("refuses PrivateLevels 0", PpttAddCaches(in, (uint32_t)n, s9, 3, 0, scratch, sizeof scratch, &why), 0);
    check("refuses a small buffer", PpttAddCaches(in, (uint32_t)n, s9, 3, 1, scratch, 300, &why), 0);
    if (argc > 2) { f = fopen(argv[2], "wb"); fwrite(out, 1, len, f); fclose(f); }
    printf("%u checks, %u failures, PPTT %zu -> %u bytes (S9), %u (deep)\n", checks, fails, n, len, len2);
    return fails ? 1 : 0;
}
