/* SPDX-License-Identifier: MIT
 * imgtest FILE OUT.ppm W H [t]: Photos' decoders (shell_media_img.inc) on the host: FILE (JPEG or PNG) decoded a step
 * at a time, as the shell does, into at most W x H (t: from the JPEG's EXIF thumbnail if it has one). tools/imgtest.py
 * compares the results with PIL (run.sh imgtest). */
#include "sbtk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shell_media_img.inc"
static FILE *F;
static int rd(void *c, uint64_t off, uint8_t *d, uint32_t n) { (void)c; fseek(F, (long)off, SEEK_SET); return (int)fread(d, 1, n, F); }
static void row(void *c, int y, const uint8_t *rgb) { mr_row(c, y, rgb); }
int main(int argc, char **argv)
{
    F = fopen(argv[1], "rb"); fseek(F, 0, SEEK_END); long n = ftell(F);
    int tw = atoi(argv[3]), th = atoi(argv[4]), usethumb = argc > 5;
    static mj_t j; static mp_t p; static mr_t r; tk_bmp b;
    uint8_t sig[4]; rd(0, 0, sig, 4);
    int steps = 0, rc = 0;
    if (sig[0] == 0xff) {
        j.buf = tk_alloc(MJ_WIN); j.own_buf = 1; j.cap = MJ_WIN; j.fsize = (uint64_t)n; j.rd = rd; j.row = row; j.row_ctx = &r;
        if (!mj_head(&j)) { printf("refused: %s\n", j.err); return 1; }
        int orient = j.orient;
        printf("jpeg %dx%d scale? %d orient %d thumb %u@%llu\n", j.w, j.h_, j.scale, j.orient, j.th_len, (unsigned long long)j.th_off);
        if (usethumb && j.th_len) {
            uint8_t *tb = malloc(j.th_len); rd(0, j.th_off, tb, j.th_len); uint32_t tl = j.th_len;
            mj_free(&j); memset(&j, 0, sizeof j);
            j.buf = tb; j.cap = j.len = tl; j.fsize = tl; j.eof = 1; j.row = row; j.row_ctx = &r;
            if (!mj_head(&j)) { printf("thumb refused: %s\n", j.err); return 1; }
            printf("thumb %dx%d scale %d\n", j.w, j.h_, j.scale);
        }
        if (!mj_start(&j, tw, th)) return 1;
        r.sw = j.ow; r.sh = j.oh; r.cx = r.cy = 0; r.cw = j.ow; r.ch = j.oh; r.tw = tw < j.ow ? tw : j.ow; r.th = th < j.oh ? th : j.oh; r.orient = orient;
        int W, H; mr_size(&r, &W, &H); tk_bmp_new(&b, W, H); r.dst = &b; mr_init(&r);
        while (!(rc = mj_step(&j))) steps++;
        if (rc < 0) printf("failed: %s\n", j.err);
    } else {
        uint8_t *all = malloc((size_t)n); rd(0, 0, all, (uint32_t)n);
        p.in = all; p.n = (uint32_t)n; p.row = row; p.row_ctx = &r;
        if (!mp_head(&p)) { printf("refused: %s\n", p.err); return 1; }
        printf("png %dx%d depth %d type %d\n", p.w, p.h, p.depth, p.ctype);
        r.sw = p.w; r.sh = p.h; r.cw = p.w; r.ch = p.h; r.tw = tw < p.w ? tw : p.w; r.th = th < p.h ? th : p.h; r.orient = 1;
        tk_bmp_new(&b, r.tw, r.th); r.dst = &b; mr_init(&r);
        while (!(rc = mp_step(&p))) steps++;
        if (rc < 0) printf("failed: %s\n", p.err);
    }
    mr_flush(&r);
    printf("steps %d\n", steps);
    FILE *o = fopen(argv[2], "wb"); fprintf(o, "P6\n%d %d\n255\n", b.w, b.h);
    for (int i = 0; i < b.w * b.h; i++) { uint32_t v = b.px[i]; fputc(v >> 16 & 255, o); fputc(v >> 8 & 255, o); fputc(v & 255, o); }
    fclose(o);
    return 0;
}
