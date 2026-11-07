/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of the Utility OS QR encoder (starbite/utility/qr.c). Encodes named strings (text, a URL, a Wi-Fi
 * login, binary with 0x00 and 0xff) and every length 0..max at each ECC level, checks each symbol's structure
 * against the spec (size, finders and separators, timing, the dark module, alignment, format bits read back
 * through their BCH code, version info) and that the version is the lowest that holds the data, then writes
 * each symbol as a PBM (4-module quiet zone, 8 px per module) into the given directory with manifest.txt:
 *   <file> <payload as hex, "-" if empty> <version> <ecc 0..3> <mask>
 * for starbite/tools/qr-check.py to decode with zbarimg.
 *   gcc -O1 -Wall -Wextra -Werror -Istarbite/utility -o qr-test starbite/tools/qr-test.c starbite/utility/qr.c
 *   ./qr-test starbite/out/qr */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qr.h"

#define SCALE 8
#define QUIET 4

/* byte-mode capacities from the spec's table 7, [ecc][version 1..10]: independent of qr.c's EC tables */
static const unsigned cap_tab[4][QR_MAX_VER] = {
    { 17, 32, 53, 78, 106, 134, 154, 192, 230, 271 },
    { 14, 26, 42, 62, 84, 106, 122, 152, 180, 213 },
    { 11, 20, 32, 46, 60, 74, 86, 108, 130, 151 },
    { 7, 14, 24, 34, 44, 58, 64, 84, 98, 119 },
};
static const char ecc_name[4] = { 'L', 'M', 'Q', 'H' };

static int fails, nsym, mask_seen[8], ver_seen[4][QR_MAX_VER + 1];
static FILE *manifest;
static const char *outdir;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); return; } } while (0)

/* remainder of v (with the generator's degree of zero bits appended) mod g, over GF(2) */
static unsigned gf2_mod(unsigned v, int vbits, unsigned g, int gdeg)
{
    v <<= gdeg;
    for (int i = vbits + gdeg - 1; i >= gdeg; i--)
        if (v >> i & 1)
            v ^= g << (i - gdeg);
    return v;
}

/* the format bits as read from copy 1 (around the top-left finder) or copy 2 (split top-right / bottom-left) */
static unsigned read_format(const struct qr *q, int copy)
{
    int n = q->size;
    unsigned f = 0;
    for (int i = 0; i < 15; i++) {
        int x, y;
        if (copy == 1) {
            if (i <= 5) { x = 8; y = i; }
            else if (i == 6) { x = 8; y = 7; }
            else if (i == 7) { x = 8; y = 8; }
            else if (i == 8) { x = 7; y = 8; }
            else { x = 14 - i; y = 8; }
        } else {
            if (i < 8) { x = n - 1 - i; y = 8; }
            else { x = 8; y = n - 15 + i; }
        }
        f |= (unsigned)q->m[y][x] << i;
    }
    return f;
}

/* a finder and its separator (clipped at the edges); (x0, y0) is the top-left corner of the 7x7 */
static int finder_ok(const struct qr *q, int x0, int y0)
{
    for (int dy = -1; dy <= 7; dy++)
        for (int dx = -1; dx <= 7; dx++) {
            int x = x0 + dx, y = y0 + dy;
            if (x < 0 || y < 0 || x >= q->size || y >= q->size)
                continue;
            int ex = dx < 3 ? 3 - dx : dx - 3, ey = dy < 3 ? 3 - dy : dy - 3, r = ex > ey ? ex : ey;
            int want = r <= 1 || r == 3;     /* r 2 and 4 (the separator) light */
            if (q->m[y][x] != want)
                return 0;
        }
    return 1;
}

static void write_pbm(const struct qr *q, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    int w = (q->size + 2 * QUIET) * SCALE, rowbytes = (w + 7) / 8;
    fprintf(f, "P4\n%d %d\n", w, w);
    unsigned char row[(QR_MAX_SIZE + 2 * QUIET) * SCALE / 8 + 1];
    for (int py = 0; py < w; py++) {
        memset(row, 0, sizeof(row));
        int my = py / SCALE - QUIET;
        for (int px = 0; px < w; px++) {
            int mx = px / SCALE - QUIET;
            if (mx >= 0 && my >= 0 && mx < q->size && my < q->size && q->m[my][mx])
                row[px >> 3] |= (unsigned char)(0x80 >> (px & 7));
        }
        fwrite(row, 1, (size_t)rowbytes, f);
    }
    fclose(f);
}

static void check_symbol(const char *name, const uint8_t *d, unsigned len, int ecc)
{
    static struct qr q;
    nsym++;
    int ok = qr_encode(d, len, ecc, &q);
    CHECK(ok, "%s: encode failed (len %u ecc %c)", name, len, ecc_name[ecc]);
    int n = q.size, v = q.ver;
    CHECK(v >= 1 && v <= QR_MAX_VER && n == 17 + 4 * v, "%s: ver %d size %d", name, v, n);
    CHECK(q.ecc == ecc, "%s: ecc %d, asked %d", name, q.ecc, ecc);
    CHECK(len <= cap_tab[ecc][v - 1] && (v == 1 || len > cap_tab[ecc][v - 2]), "%s: len %u at version %d is not the "
          "lowest", name, len, v);
    CHECK(q.mask >= 0 && q.mask < 8, "%s: mask %d", name, q.mask);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            CHECK(q.m[y][x] <= 1, "%s: module (%d,%d) = %d", name, x, y, q.m[y][x]);
    CHECK(finder_ok(&q, 0, 0) && finder_ok(&q, n - 7, 0) && finder_ok(&q, 0, n - 7), "%s: finders", name);
    for (int i = 8; i < n - 8; i++)
        CHECK(q.m[6][i] == (i % 2 == 0) && q.m[i][6] == (i % 2 == 0), "%s: timing at %d", name, i);
    CHECK(q.m[n - 8][8] == 1, "%s: dark module", name);
    if (v >= 2) {                           /* the bottom-right alignment pattern, centred at (n-7, n-7) */
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                int r = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                CHECK(q.m[n - 7 + dy][n - 7 + dx] == (r != 1), "%s: alignment", name);
            }
    }
    unsigned f1 = read_format(&q, 1), f2 = read_format(&q, 2);
    CHECK(f1 == f2, "%s: format copies differ %04x %04x", name, f1, f2);
    unsigned fd = f1 ^ 0x5412;
    CHECK(gf2_mod(fd >> 10, 5, 0x537, 10) == (fd & 0x3ff), "%s: format BCH %04x", name, f1);
    static const int ind_to_ecc[4] = { QR_ECC_M, QR_ECC_L, QR_ECC_H, QR_ECC_Q };
    CHECK(ind_to_ecc[fd >> 13] == ecc && (int)(fd >> 10 & 7) == q.mask, "%s: format says ecc %d mask %u", name,
          ind_to_ecc[fd >> 13], fd >> 10 & 7);
    if (v >= 7) {
        unsigned a = 0, b = 0;
        for (int i = 0; i < 18; i++) {
            a |= (unsigned)q.m[i / 3][n - 11 + i % 3] << i;
            b |= (unsigned)q.m[n - 11 + i % 3][i / 3] << i;
        }
        CHECK(a == b && (int)(a >> 12) == v && gf2_mod(a >> 12, 6, 0x1f25, 12) == (a & 0xfff),
              "%s: version info %05x %05x", name, a, b);
    }
    mask_seen[q.mask]++;
    ver_seen[ecc][v]++;

    char path[512], file[64];
    snprintf(file, sizeof(file), "qr%04d-v%02d%c-m%d.pbm", nsym, v, ecc_name[ecc], q.mask);
    snprintf(path, sizeof(path), "%s/%s", outdir, file);
    write_pbm(&q, path);
    fprintf(manifest, "%s ", file);
    if (!len)
        fputc('-', manifest);
    for (unsigned i = 0; i < len; i++)
        fprintf(manifest, "%02x", d[i]);
    fprintf(manifest, " %d %d %d %s\n", v, ecc, q.mask, name);
}

static void check_str(const char *name, const char *s, int ecc)
{
    check_symbol(name, (const uint8_t *)s, (unsigned)strlen(s), ecc);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
        return 2;
    }
    outdir = argv[1];
    char mpath[512];
    snprintf(mpath, sizeof(mpath), "%s/manifest.txt", outdir);
    manifest = fopen(mpath, "w");
    if (!manifest) {
        perror(mpath);
        return 2;
    }

    for (int e = 0; e < 4; e++) {
        check_str("hello", "HELLO", e);
        check_str("url", "https://example.com/starbite/utility?page=qr&v=1", e);
        check_str("wifi", "WIFI:T:WPA;S:name;P:pass;;", e);
        check_str("wifi-escaped", "WIFI:T:WPA;S:Starbite\\;Lab;P:p\\:a\\\\ss w0rd;H:false;;", e);
        check_str("utf8", "Hyv\xc3\xa4\xc3\xa4 p\xc3\xa4iv\xc3\xa4\xc3\xa4 \xe2\x80\x94 \xe2\x9c\x93", e);
        static const uint8_t bin[] = { 0x00, 0xff, 0x00, 0x80, 0x7f, 0x01, 0xfe, 0x00, 0x00, 0xff, 0xff, 0x0a, 0x0d };
        check_symbol("binary", bin, sizeof(bin), e);
        uint8_t all[256];
        for (int i = 0; i < 256; i++)
            all[i] = (uint8_t)(255 - i);
        check_symbol("bytes-ff-to-08", all, cap_tab[e][QR_MAX_VER - 1] < 248 ? cap_tab[e][QR_MAX_VER - 1] : 248, e);
    }

    /* every length 0..max at each ECC level, pseudo-random bytes over 0x00..0xff (xorshift, fixed seed) */
    uint32_t seed = 0x5eed1234;
    for (int e = 0; e < 4; e++) {
        unsigned cap = cap_tab[e][QR_MAX_VER - 1];
        if (qr_capacity(e) != cap) {
            fails++;
            printf("FAIL qr_capacity(%c) = %u, want %u\n", ecc_name[e], qr_capacity(e), cap);
        }
        uint8_t buf[300];
        for (unsigned len = 0; len <= cap; len++) {
            for (unsigned i = 0; i < len; i++) {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                buf[i] = (uint8_t)(seed >> 7);
            }
            char name[32];
            snprintf(name, sizeof(name), "sweep-%c-%u", ecc_name[e], len);
            check_symbol(name, buf, len, e);
        }
        static struct qr q;
        if (qr_encode(buf, cap + 1, e, &q)) {
            fails++;
            printf("FAIL %c: %u bytes encoded, capacity %u\n", ecc_name[e], cap + 1, cap);
        }
    }
    static struct qr q;
    if (qr_encode((const uint8_t *)"x", 1, 4, &q) || qr_encode((const uint8_t *)"x", 1, -1, &q)) {
        fails++;
        printf("FAIL bad ecc accepted\n");
    }
    fclose(manifest);

    for (int m = 0; m < 8; m++)
        if (!mask_seen[m]) {
            fails++;
            printf("FAIL mask %d never chosen\n", m);
        }
    for (int e = 0; e < 4; e++)
        for (int v = 1; v <= QR_MAX_VER; v++)
            if (!ver_seen[e][v]) {
                fails++;
                printf("FAIL version %d%c never made\n", v, ecc_name[e]);
            }
    printf("qr-test: %d symbols, masks", nsym);
    for (int m = 0; m < 8; m++)
        printf(" %d:%d", m, mask_seen[m]);
    printf(", %d failures\n", fails);
    return fails != 0;
}
