/* Host test of SmbiosBuild.c: writes dmidecode dumps (entry point at 0, table at 32), run.sh decodes them.
 *   smbios-host OUTDIR      -> OUTDIR/ep3-t7.bin ep3-t0.bin ep21-t7.bin ep21-t0.bin
 * Also checks the structure walk, the handles and both checksums itself. SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SB_HOST
#define SB_HOST 1
#endif
#include "../Star2LteSmbiosDxe/SmbiosBuild.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static unsigned sum(const sb_u8 *p, unsigned n) { unsigned s = 0; while (n--) s += *p++; return s & 0xFF; }

static void walk(const sb_u8 *t, sb_u32 len, sb_u16 count, sb_u16 maxsize, int t7)
{
    sb_u32 off = 0;
    unsigned n = 0, max = 0;
    int types[16], nt = 0;
    while (off < len) {
        sb_u32 start = off;
        CHECK(t[off + 1] >= 4);
        CHECK((unsigned)(t[off + 2] | t[off + 3] << 8) == n);           /* handles 0, 1, 2, ... */
        if (nt < 16) types[nt++] = t[off];
        off += t[off + 1];
        while (!(t[off] == 0 && t[off + 1] == 0)) off++;      /* the string set ends with two NULs */
        off += 2;
        if (off - start > max) max = off - start;
        n++;
        if (t[start] == 127) break;
    }
    CHECK(off == len);
    CHECK(n == count);
    CHECK(max == maxsize);
    CHECK(nt == (t7 ? 8 : 5));
    CHECK(types[0] == 0 && types[1] == 1 && types[2] == 2 + 1);
    CHECK(types[nt - 1] == 127 && types[nt - 2] == 4);
}

static void dump(const char *dir, const char *name, const sb_u8 *ep, unsigned eplen, const sb_u8 *t, sb_u32 len)
{
    char path[512];
    sb_u8 buf[4096] = { 0 };
    FILE *f;
    memcpy(buf, ep, eplen);
    memcpy(buf + 32, t, len);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(buf, 1, 32 + len, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    int t7;
    if (argc != 2) { fprintf(stderr, "usage: smbios-host OUTDIR\n"); return 2; }
    for (t7 = 0; t7 <= 1; t7++) {
        sb_u8 t[2048], ep3[SB_EP3_LEN], ep21[SB_EP21_LEN], small[64];
        sb_u16 count = 0, maxsize = 0, c2, m2;
        sb_u32 len = SbSmbiosBuild(t, sizeof t, 0x411FD050u, t7, &count, &maxsize);
        CHECK(len > 0 && len < 1024);
        walk(t, len, count, maxsize, t7);
        CHECK(SbSmbiosBuild(small, sizeof small, 0, t7, &c2, &m2) == 0);   /* too small: refused, no overrun */
        CHECK(SbSmbiosBuild(t, len, 0x411FD050u, t7, &c2, &m2) == len);     /* exactly enough */
        CHECK(SbSmbiosBuild(t, len - 1, 0x411FD050u, t7, &c2, &m2) == 0);
        SbSmbiosBuild(t, sizeof t, 0x411FD050u, t7, &count, &maxsize);
        SbSmbiosEp3(ep3, 32, len);
        SbSmbiosEp21(ep21, 32, (sb_u16)len, count, maxsize);
        CHECK(sum(ep3, SB_EP3_LEN) == 0);
        CHECK(sum(ep21, SB_EP21_LEN) == 0);
        CHECK(sum(ep21 + 0x10, 0x0F) == 0);
        dump(argv[1], t7 ? "ep3-t7.bin" : "ep3-t0.bin", ep3, SB_EP3_LEN, t, len);
        dump(argv[1], t7 ? "ep21-t7.bin" : "ep21-t0.bin", ep21, SB_EP21_LEN, t, len);
        printf("type7=%d: %u bytes, %u structures, largest %u\n", t7, len, count, maxsize);
    }
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("host checks ok\n");
    return 0;
}
