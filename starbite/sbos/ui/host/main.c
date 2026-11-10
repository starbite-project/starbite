/* SPDX-License-Identifier: MIT
 * sbtk-host: the toolkit and the lock shell on the host (P1 of the UI port plan).
 *   sbtk-host PAK still OUT.ppm [state ...]      one frame at rest, full resolution; a state is what=key:value
 *                                               (unlock=:0.5, group=apple, focus=win11:1, scroll=:120, hold=t:0.6)
 *   sbtk-host PAK film OUT.bgra SCRIPT [half]   60 fps: the script's touches through the gestures and springs,
 *                                               every frame composed as the phone would (motion frames at half
 *                                               resolution + the 2x present, the first frame at rest full); frames
 *                                               as raw BGRA (720 x 1480 with half), one "F" line per frame on stdout
 *   sbtk-host PAK bench                         uibench's kernels on this host (the A55 calibration)
 * A script: "wait MS", "tap X Y", "drag X0 Y0 X1 Y1 MS", "hold X Y MS" (pt, ms), separated by ';'. */
#include "../sbtk.h"
#include "../shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *readall(const char *p, uint32_t *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = tk_alloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { perror(p); exit(1); }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

static void write_ppm(const char *path, const tk_bmp *b)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", b->w, b->h);
    uint8_t *row = malloc((size_t)b->w * 3);
    for (int j = 0; j < b->h; j++) {
        for (int i = 0; i < b->w; i++) {
            uint32_t p = b->px[j * b->stride + i];
            row[3 * i] = p >> 16 & 255; row[3 * i + 1] = p >> 8 & 255; row[3 * i + 2] = p & 255;
        }
        fwrite(row, 1, (size_t)b->w * 3, f);
    }
    free(row);
    fclose(f);
}

static void apply_state(const char *st)
{
    char what[32] = { 0 }, key[64] = { 0 };
    float v = 1;
    const char *eq = strchr(st, '=');
    if (!eq) return;
    memcpy(what, st, (size_t)(eq - st));
    const char *colon = strchr(eq + 1, ':');
    if (colon) { memcpy(key, eq + 1, (size_t)(colon - eq - 1)); v = (float)atof(colon + 1); }
    else strcpy(key, eq + 1);
    sh_set(what, key, v);
}

/* ---- the film ------------------------------------------------------------------------------------------- */
typedef struct { int kind; float x0, y0, x1, y1, ms; } step;
static int parse(const char *s, step *st, int max)
{
    int n = 0;
    char buf[4096];
    strncpy(buf, s, sizeof buf - 1);
    for (char *tok = strtok(buf, ";"); tok && n < max; tok = strtok(0, ";")) {
        step *p = &st[n];
        memset(p, 0, sizeof *p);
        char w[16];
        if (sscanf(tok, " %15s", w) != 1) continue;
        if (!strcmp(w, "wait")) { p->kind = 0; sscanf(tok, " %*s %f", &p->ms); }
        else if (!strcmp(w, "tap")) { p->kind = 1; sscanf(tok, " %*s %f %f", &p->x0, &p->y0); p->ms = 80; }
        else if (!strcmp(w, "drag")) { p->kind = 2; sscanf(tok, " %*s %f %f %f %f %f", &p->x0, &p->y0, &p->x1, &p->y1, &p->ms); }
        else if (!strcmp(w, "hold")) { p->kind = 3; sscanf(tok, " %*s %f %f %f", &p->x0, &p->y0, &p->ms); }
        else if (!strcmp(w, "state")) { p->kind = 4; }
        n++;
    }
    return n;
}

static int film(tk_comp *c, const char *out, const char *script, int half)
{
    step st[64];
    int n = parse(script, st, 64);
    float total = 0;
    for (int i = 0; i < n; i++) total += st[i].ms;
    total += 1500;     /* let everything settle */
    FILE *f = fopen(out, "wb");
    tk_bmp h;
    tk_bmp_new(&h, TK_W / 2, TK_H / 2);
    const float dt = 1000.0f / 60;
    float t = 0, s0 = 0;
    int si = 0, phase = -1, frame = 0, rest_frames = 0;
    tk_comp_frame(c, 0, 1);
    while (t < total || sh_moving()) {
        /* the finger: events at 120 Hz between frames (the touch controller's rate) */
        for (int sub = 1; sub <= 2; sub++) {
            float te = t + dt * sub / 2;
            while (si < n && te >= s0 + st[si].ms) {
                if (phase >= 0 && st[si].kind) {
                    step *p = &st[si];
                    float x = p->kind == 2 ? p->x1 : p->x0, y = p->kind == 2 ? p->y1 : p->y0;
                    sh_touch(2, x, y, s0 + p->ms);
                }
                phase = -1;
                s0 += st[si].ms;
                si++;
            }
            if (si >= n) break;
            step *p = &st[si];
            if (!p->kind) continue;
            float u = (te - s0) / p->ms;
            if (phase < 0) { sh_touch(0, p->x0, p->y0, te); phase = 0; continue; }
            if (p->kind == 2) {
                float e = u < 1 ? u : 1;
                e = 1 - (1 - e) * (1 - e) * .3f - (1 - e) * .7f;     /* a finger: fast start, slight ease */
                e = u;
                sh_touch(1, p->x0 + (p->x1 - p->x0) * e, p->y0 + (p->y1 - p->y0) * e, te);
            }
        }
        int mv = sh_tick(dt / 1000, t);
        int motion = mv || sh_moving();
        tk_comp_frame(c, motion, 0);
        printf("F %d %.1f %s px %d comp %.2f present %.2f total %.2f\n", frame, t, c->last_half ? "half" : "full",
               c->last_px, c->us_comp / 1000.0, c->us_present / 1000.0, c->last_us / 1000.0);
        if (half) {
            tk_bmp_down2(&h, &c->fb);
            fwrite(h.px, 4, (size_t)h.w * h.h, f);
        } else
            fwrite(c->fb.px, 4, (size_t)TK_W * TK_H, f);
        t += dt;
        frame++;
        rest_frames = motion ? 0 : rest_frames + 1;
        if (t > total + 8000) break;
    }
    (void)rest_frames;
    fclose(f);
    fprintf(stderr, "film: %d frames\n", frame);
    return 0;
}

/* ---- uibench's kernels on the host: the same NEON as kern/uibench.c, one core --------------------------- */
#include <arm_neon.h>
static void blend_rows(uint8_t *d, const uint8_t *s, uint32_t w, uint32_t y0, uint32_t y1)
{
    for (uint32_t y = y0; y < y1; ++y) {
        const uint8_t *a = s + (size_t)y * w * 4;
        uint8_t *b = d + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x += 8) {
            uint8x8x4_t sp = vld4_u8(a + x * 4), dp = vld4_u8(b + x * 4);
            uint8x8_t ia = vmvn_u8(sp.val[3]);
            for (int c = 0; c < 4; ++c) {
                uint16x8_t m = vmull_u8(dp.val[c], ia);
                uint8x8_t r = vrshrn_n_u16(vrsraq_n_u16(m, m, 8), 8);
                dp.val[c] = vqadd_u8(sp.val[c], r);
            }
            vst4_u8(b + x * 4, dp);
        }
    }
}
static void bench(void)
{
    const uint32_t FW = 1440, FH = 2960, HW = 720, HH = 1480;
    uint8_t *f0 = tk_alloc((size_t)FW * FH * 4), *f1 = tk_alloc((size_t)FW * FH * 4);
    uint8_t *h0 = tk_alloc((size_t)HW * HH * 4), *h1 = tk_alloc((size_t)HW * HH * 4);
    for (size_t i = 0; i < (size_t)HW * HH; i++) { uint32_t a = 128 + (uint32_t)(i % 128); ((uint32_t *)h1)[i] = a << 24 | (a / 2) << 16 | (a / 3) << 8 | (a / 4); }
    for (size_t i = 0; i < (size_t)FW * FH; i++) ((uint32_t *)f1)[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
    struct { const char *n; int k; } K[] = { { "fill, a full frame in RAM", 0 }, { "copy, a full frame RAM to RAM", 1 },
        { "blend, a half-resolution layer over another", 2 }, { "blend, a full-resolution layer over another", 3 } };
    for (int k = 0; k < 4; k++) {
        uint64_t best = ~0ull, bytes = 0;
        for (int r = 0; r < 5; r++) {
            uint64_t t0 = tk_us();
            switch (K[k].k) {
            case 0: { uint32x4_t v = vdupq_n_u32(0xff203040u); for (size_t i = 0; i < (size_t)FW * FH; i += 4) vst1q_u32((uint32_t *)f0 + i, v); bytes = (uint64_t)FW * FH * 4; break; }
            case 1: memcpy(f0, f1, (size_t)FW * FH * 4); bytes = (uint64_t)FW * FH * 8; break;
            case 2: blend_rows(h0, h1, HW, 0, HH); bytes = (uint64_t)HW * HH * 12; break;
            case 3: blend_rows(f0, f1, FW, 0, FH); bytes = (uint64_t)FW * FH * 12; break;
            }
            uint64_t t = tk_us() - t0;
            if (t < best) best = t;
        }
        printf("hostbench: %s: %llu.%llu ms, %llu MB/s\n", K[k].n, (unsigned long long)(best / 1000), (unsigned long long)(best % 1000 / 100),
               (unsigned long long)(bytes / (best ? best : 1)));
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: sbtk-host PAK still|film|bench ...\n"); return 2; }
    uint32_t len;
    uint8_t *pak = readall(argv[1], &len);
    if (!strcmp(argv[2], "bench")) { bench(); return 0; }
    if (!tk_pack_open(pak, len)) { fprintf(stderr, "bad pack\n"); return 1; }
    static tk_comp c;
    if (!tk_comp_init(&c)) return 1;
    uint64_t t0 = tk_us();
    sh_init(&c);
    fprintf(stderr, "bake: %.1f ms\n", (tk_us() - t0) / 1000.0);
    if (!strcmp(argv[2], "still") && argc >= 4) {
        for (int i = 4; i < argc; i++) apply_state(argv[i]);
        tk_comp_frame(&c, 0, 1);
        fprintf(stderr, "frame: %.2f ms\n", c.last_us / 1000.0);
        write_ppm(argv[3], &c.fb);
        return 0;
    }
    if (!strcmp(argv[2], "film") && argc >= 5) return film(&c, argv[3], argv[4], argc > 5 && !strcmp(argv[5], "half"));
    return 2;
}
