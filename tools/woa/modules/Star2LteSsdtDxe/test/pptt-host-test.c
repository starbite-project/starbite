/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of PpttBuild (test/run.sh): the release's own PPTT (test/release-pptt.bin, from the
 * woa release DXE volume's platform table file, BSD-2-Clause-Patent) with typical Cortex-A55
 * caches; checks the result's structure and writes it for iasl -d. */
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

int main(int argc, char **argv)
{
    uint8_t in[4096], out[4096];
    FILE *f = fopen(argc > 1 ? argv[1] : "release-pptt.bin", "rb");
    size_t n = fread(in, 1, sizeof in, f);
    fclose(f);
    const PB_CACHE c[] = {
        { 1, PB_DATA, 64, 128, 4 }, { 1, PB_INSTRUCTION, 64, 128, 4 },
        { 2, PB_UNIFIED, 64, 256, 4 }, { 3, PB_UNIFIED, 64, 512, 16 },
    };
    const char *why;
    uint32_t len = PpttAddCaches(in, (uint32_t)n, c, 4, out, sizeof out, &why);
    if (!len) { printf("FAIL build: %s\n", why); return 1; }
    unsigned sum = 0;
    for (uint32_t i = 0; i < len; ++i) sum += out[i];
    check("checksum", sum & 0xff, 0);
    check("length field", rd32(out + 4), len);
    /* walk: leaves 0..3 reference 2 level-1 caches; chains L1 -> L2 (own) -> L3 -> 0 */
    unsigned procs = 0, caches = 0, target = 0, l3 = 0;
    for (uint32_t o = 36; o < len; o += out[o + 1]) {
        if (out[o] == 0) {
            ++procs;
            uint32_t flags = rd32(out + o + 4), id = rd32(out + o + 12), nref = rd32(out + o + 16);
            if ((flags & 8) && id <= 3) {
                ++target;
                check("leaf refs", nref, 2);
                for (uint32_t r = 0; r < nref; ++r) {
                    uint32_t c1 = rd32(out + o + 20 + 4 * r);
                    check("ref is a cache", out[c1], 1);
                    uint32_t c2 = rd32(out + c1 + 8);
                    check("L1 -> L2 cache", out[c2], 1);
                    check("L2 size", rd32(out + c2 + 12), 64 * 256 * 4);
                    uint32_t c3 = rd32(out + c2 + 8);
                    check("L2 -> L3 cache", out[c3], 1);
                    check("L3 size", rd32(out + c3 + 12), 64 * 512 * 16);
                    check("L3 last", rd32(out + c3 + 8), 0);
                    if (!l3) l3 = c3;
                    check("one shared L3", c3, l3);
                }
            } else if (flags & 8) {
                check("M3 leaf untouched", nref, 0);
            }
            uint32_t parent = rd32(out + o + 8);
            if (parent) check("parent is a node", out[parent], 0);
        } else if (out[o] == 1) {
            ++caches;
            check("cache length (rev 3)", out[o + 1], 28);
        }
    }
    check("processor nodes", procs, 11);
    check("A55 leaves", target, 4);
    check("caches", caches, 4 * 3 + 1);
    /* refusals */
    check("refuses its own output", PpttAddCaches(out, len, c, 4, in, sizeof in, &why), 0);
    PB_CACHE bad = { 1, PB_DATA, 0, 128, 4 };
    check("refuses line size 0", PpttAddCaches(in, (uint32_t)n, &bad, 1, out + 2048, 2048, &why), 0);
    if (argc > 2) { f = fopen(argv[2], "wb"); fwrite(out, 1, len, f); fclose(f); }
    printf("%u checks, %u failures, PPTT %zu -> %u bytes\n", checks, fails, n, len);
    return fails ? 1 : 0;
}
