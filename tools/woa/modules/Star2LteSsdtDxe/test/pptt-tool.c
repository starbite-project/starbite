/* SPDX-License-Identifier: GPL-2.0-only
 * pptt-tool: PpttBuild on the host, for tools/woa/pptt-patch.py (build-time PPTT fix,
 * docs/WIN11-USB.md section 10).
 *   pptt-tool IN.aml OUT.aml PRIVATE_LEVELS LEVEL:KIND:LINE:SETS:WAYS ...
 * KIND: d (data), i (instruction), u (unified). Exit 1 with the reason when PpttBuild refuses. */
#include <stdio.h>
#include <stdlib.h>
#include "../PpttBuild.h"

int main(int argc, char **argv)
{
    static uint8_t in[65536], out[65536];
    PB_CACHE c[PB_MAX_CACHES];
    unsigned nc = 0;
    if (argc < 5 || argc - 4 > (int)PB_MAX_CACHES) {
        fprintf(stderr, "usage: pptt-tool IN OUT PRIVATE_LEVELS LEVEL:KIND:LINE:SETS:WAYS ...\n");
        return 2;
    }
    for (int a = 4; a < argc; ++a) {
        unsigned lv, line, sets, ways;
        char k;
        if (sscanf(argv[a], "%u:%c:%u:%u:%u", &lv, &k, &line, &sets, &ways) != 5 || (k != 'd' && k != 'i' && k != 'u')) {
            fprintf(stderr, "bad cache %s\n", argv[a]);
            return 2;
        }
        c[nc].Level = (pb_u8)lv;
        c[nc].Kind = k == 'd' ? PB_DATA : k == 'i' ? PB_INSTRUCTION : PB_UNIFIED;
        c[nc].LineSize = (pb_u16)line;
        c[nc].Sets = sets;
        c[nc].Ways = ways;
        ++nc;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    size_t n = fread(in, 1, sizeof in, f);
    fclose(f);
    const char *why = 0;
    uint32_t len = PpttAddCaches(in, (uint32_t)n, c, nc, (pb_u32)atoi(argv[3]), out, sizeof out, &why);
    if (!len) { fprintf(stderr, "pptt-tool: %s\n", why ? why : "refused"); return 1; }
    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, len, f) != len || fclose(f)) { perror(argv[2]); return 2; }
    printf("PPTT %zu -> %u bytes\n", n, len);
    return 0;
}
