/* Host preview of boot menu v7 (docs/MENU7-PORT.md section 5): the real shim/starlte/glass*.c and
 * sb7.c (and, when built with -DH7_MENU7, menu7*.c, intro7.c and loader_stubs.c) on aarch64 Linux
 * (the Lima VM), with the loader's memory map mmap'ed at its own addresses:
 *   0x84000000 .. 0x88800000  G7_CANVAS, G7_SHARP, G7_BASE, G7_POOL, G7_CACHE
 *   0x89000000                the BOOT ramdisk (menu-v7.sbm), as S-Boot loads it
 *   0xcc000000                the framebuffer
 *
 *   h7 --ramdisk menu-v7.sbm --list scene.dl --out out.ppm [--stats] [--check-full]
 *       a display list (geom2dl.py): one or more frames, each drawn by g7_frame; the panel
 *       (the framebuffer) after the last one is written. --check-full draws the last frame again
 *       with G7_F_FULL and compares: the dirty-rect path must equal a full redraw.
 *   h7 --ramdisk menu-v7.sbm --state <state> [--p 0.4] [--t ms] [loader_stubs options] --out out.ppm
 *       (H7_MENU7 builds) menu7's own frame for a concept state (m7_preview_state), or the intro
 *       at t ms (intro7_frame)
 *
 * Display list lines (numbers are floats unless noted; colours 0xAARRGGBB; # comments):
 *   frame [full] [refreeze]           starts a frame (the previous one is drawn)
 *   dim D | back B | light lx ly | wall sx sy si sr sw rr rf amb | word tm tf ta cx cy cw ch
 *   layer base|over                   following lines go to that layer
 *   clip x0 y0 x1 y1 r                the layer's next clip (index 1, 2, ...)
 *   shape key flags x y w h r a s tr tg tb ta
 *   text key clip flags a color font x y track scale max_w color2 gy0 gy1 gstop |string
 *   image key clip flags a color id x y w h sat
 *   tile key clip flags a tile size x y w sat         (tile "k:<catalog key>:<category>": sb7_tile_for)
 *   rect key clip flags a color x y w h r ring
 *   shadow key clip flags a color x y w h r dx dy blur spread
 *   arc key clip flags a color cx cy r width a0 sweep
 *   stroke key clip flags a color width n x0 y0 x1 y1 ...
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "glass.h"
#include "sb7.h"
#ifdef H7_MENU7
#include "intro7.h"
#include "menu7.h"
#endif

#define FB ((uint32_t *)(0xcc000000UL + G7_HOSTOFF))
#define RD_BASE (0x89000000UL + G7_HOSTOFF)

#ifndef H7_MENU7
/* the loader's timer (loader_stubs.c provides the fake clock in H7_MENU7 builds) */
uint64_t hw_ticks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
uint64_t hw_freq(void) { return 1000000000ULL; }
#endif

static void die(const char *m)
{
    fprintf(stderr, "h7: %s\n", m);
    exit(1);
}
static void map_fixed(uintptr_t a, size_t n)
{
    void *p = mmap((void *)a, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)a) {
        fprintf(stderr, "h7: cannot map 0x%lx (+0x%zx)\n", (unsigned long)a, n);
        exit(1);
    }
}

static size_t load_ramdisk(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die("cannot open the ramdisk");
    struct stat st;
    fstat(fd, &st);
    size_t n = ((size_t)st.st_size + 0xfffff) & ~(size_t)0xfffff;
    map_fixed(RD_BASE, n);
    if (read(fd, (void *)RD_BASE, (size_t)st.st_size) != st.st_size)
        die("short read");
    close(fd);
    return (size_t)st.st_size;
}

static void write_ppm(const char *path, const uint32_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        die("cannot write the output");
    fprintf(f, "P6\n%d %d\n255\n", G7_W, G7_H);
    static unsigned char row[3 * G7_W];
    for (int y = 0; y < G7_H; y++) {
        for (int x = 0; x < G7_W; x++) {
            uint32_t v = px[y * G7_W + x];
            row[3 * x] = (unsigned char)(v >> 16);
            row[3 * x + 1] = (unsigned char)(v >> 8);
            row[3 * x + 2] = (unsigned char)v;
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
}

/* ---- display lists ---- */

static struct g7_layer LBASE, LOVER;
static struct g7_frame F;
static char strpool[1 << 20];
static size_t strtop;
static int stats, nframes;
static double worst_ms;

static void frame_reset(void)
{
    g7_layer_reset(&LBASE);
    g7_layer_reset(&LOVER);
    F.base = &LBASE;
    F.over = &LOVER;
    F.flags = 0;
    strtop = 0;
}

#ifdef H7_TIME
/* --run (H7_TIME builds link with -Wl,--wrap=g7_frame): menu7_main on the fake clock, every g7_frame
 * timed on the host clock and printed as "T7 <fake ms> <host ms> <px> <mode>" */
uint32_t __real_g7_frame(const struct g7_frame *f);
uint64_t stub_now_us(void);
static int run_mode;
uint32_t __wrap_g7_frame(const struct g7_frame *f)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint32_t px = __real_g7_frame(f);
    clock_gettime(CLOCK_MONOTONIC, &b);
    if (run_mode)
        printf("T7 %.0f %.2f %u%s\n", stub_now_us() / 1e3,
               (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6, px,
               (f->flags & G7_F_FULL) ? " full" : "");
    return px;
}
#endif

static void draw_frame(void)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint32_t px = g7_frame(&F);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
    if (ms > worst_ms)
        worst_ms = ms;
    if (stats) {
        struct g7_stats s;
        g7_get_stats(&s);
        fprintf(stderr, "frame %d: %u px, %.2f ms host (x12 ~ %.1f ms on the A55); %u shapes / %u items base, %u / %u over\n",
                nframes, px, ms, ms * 12, LBASE.nshape, LBASE.nitem, LOVER.nshape, LOVER.nitem);
    }
    nframes++;
}

static uint32_t hexv(const char *s) { return (uint32_t)strtoul(s, 0, 0); }

static void parse_list(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        die("cannot open the list");
    static char line[8192];
    struct g7_layer *L = &LBASE;
    int have = 0;
    F.dim = .8f;
    F.back = 0;
    F.wall = (struct g7_wall){ G7_WALL_FINAL_SX, G7_WALL_FINAL_SY, G7_WALL_FINAL_SI, G7_WALL_FINAL_SR, G7_WALL_FINAL_SW,
                               G7_WALL_FINAL_RR, G7_WALL_FINAL_RF, G7_WALL_FINAL_AMB };
    frame_reset();
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *str = strchr(line, '|');
        if (str)
            *str++ = 0;
        char *tok[64];
        int nt = 0;
        for (char *p = strtok(line, " \t"); p && nt < 64; p = strtok(0, " \t"))
            tok[nt++] = p;
        if (!nt || tok[0][0] == '#')
            continue;
        const char *k = tok[0];
#define FV(i) ((float)atof(tok[i]))
        if (!strcmp(k, "frame")) {
            if (have)
                draw_frame();
            frame_reset();
            for (int i = 1; i < nt; i++) {
                if (!strcmp(tok[i], "full")) F.flags |= G7_F_FULL;
                if (!strcmp(tok[i], "refreeze")) F.flags |= G7_F_REFREEZE;
            }
            L = &LBASE;
            have = 1;
        } else if (!strcmp(k, "dim") && nt >= 2) F.dim = FV(1);
        else if (!strcmp(k, "back") && nt >= 2) F.back = FV(1);
        else if (!strcmp(k, "light") && nt >= 3) { F.lx = FV(1); F.ly = FV(2); }
        else if (!strcmp(k, "wall") && nt >= 9)
            F.wall = (struct g7_wall){ FV(1), FV(2), FV(3), FV(4), FV(5), FV(6), FV(7), FV(8) };
        else if (!strcmp(k, "word") && nt >= 8) {
            F.word.tm = FV(1); F.word.tf = FV(2); F.word.ta = FV(3);
            for (int i = 0; i < 4; i++) F.word.cap[i] = FV(4 + i);
        } else if (!strcmp(k, "layer") && nt >= 2) L = !strcmp(tok[1], "over") ? &LOVER : &LBASE;
        else if (!strcmp(k, "clip") && nt >= 6) g7_add_clip(L, FV(1), FV(2), FV(3), FV(4), FV(5));
        else if (!strcmp(k, "shape") && nt >= 14) {
            struct g7_shape *s = g7_add_shape(L);
            if (!s) continue;
            s->key = hexv(tok[1]); s->flags = hexv(tok[2]);
            s->x = FV(3); s->y = FV(4); s->w = FV(5); s->h = FV(6); s->r = FV(7); s->a = FV(8); s->s = FV(9);
            for (int i = 0; i < 4; i++) s->tint[i] = FV(10 + i);
        } else if (nt >= 6) {
            static const char *kinds[] = { "", "text", "image", "tile", "rect", "shadow", "arc", "stroke" };
            int kind = 0;
            for (int i = 1; i < 8; i++)
                if (!strcmp(k, kinds[i])) kind = i;
            if (!kind) { fprintf(stderr, "h7: unknown line '%s'\n", k); continue; }
            struct g7_item *it = g7_add_item(L, (unsigned)kind);
            if (!it) continue;
            it->key = hexv(tok[1]); it->clip = (uint8_t)atoi(tok[2]); it->flags = (uint16_t)hexv(tok[3]); it->a = FV(4);
            int o = 5;
            if (kind != G7_TILE) it->color = hexv(tok[o++]);
            switch (kind) {
            case G7_TEXT: {
                struct g7_text *t = &it->u.text;
                if (nt < o + 10) break;
                t->font = (uint16_t)atoi(tok[o]); t->x = FV(o + 1); t->y = FV(o + 2); t->track = FV(o + 3);
                t->scale = FV(o + 4); t->max_w = FV(o + 5); t->color2 = hexv(tok[o + 6]); t->gy0 = FV(o + 7);
                t->gy1 = FV(o + 8); t->gstop = FV(o + 9);
                size_t n = str ? strlen(str) : 0;
                if (strtop + n + 1 > sizeof strpool) die("string pool full");
                memcpy(strpool + strtop, str ? str : "", n + 1);
                t->s = strpool + strtop;
                t->n = (uint16_t)n;
                strtop += n + 1;
                break;
            }
            case G7_IMAGE:
                it->u.image.id = (uint16_t)atoi(tok[o]); it->u.image.x = FV(o + 1); it->u.image.y = FV(o + 2);
                it->u.image.w = FV(o + 3); it->u.image.h = FV(o + 4); it->u.image.sat = FV(o + 5);
                break;
            case G7_TILE: {
                const char *tk = tok[o];
                unsigned tid;
                if (!strncmp(tk, "k:", 2)) {
                    char key[64];
                    snprintf(key, sizeof key, "%s", tk + 2);
                    char *c = strrchr(key, ':');
                    unsigned cat = 0;
                    if (c) { *c = 0; cat = (unsigned)atoi(c + 1); }
                    tid = sb7_tile_for(key, cat);
                } else
                    tid = (unsigned)atoi(tk);
                it->u.tile.tile = (uint16_t)tid; it->u.tile.size = (uint16_t)atoi(tok[o + 1]);
                it->u.tile.x = FV(o + 2); it->u.tile.y = FV(o + 3); it->u.tile.w = FV(o + 4); it->u.tile.sat = FV(o + 5);
                break;
            }
            case G7_RECT:
                it->u.rect = (struct g7_rect){ FV(o), FV(o + 1), FV(o + 2), FV(o + 3), FV(o + 4), FV(o + 5) };
                break;
            case G7_SHADOW:
                it->u.shadow = (struct g7_boxshadow){ FV(o), FV(o + 1), FV(o + 2), FV(o + 3), FV(o + 4),
                                                      FV(o + 5), FV(o + 6), FV(o + 7), FV(o + 8) };
                break;
            case G7_ARC:
                it->u.arc = (struct g7_arc){ FV(o), FV(o + 1), FV(o + 2), FV(o + 3), FV(o + 4), FV(o + 5) };
                break;
            case G7_STROKE: {
                struct g7_stroke *s = &it->u.stroke;
                s->width = FV(o);
                s->n = (uint8_t)atoi(tok[o + 1]);
                for (int i = 0; i < s->n && i < 4 && o + 3 + 2 * i < nt; i++) {
                    s->x[i] = FV(o + 2 + 2 * i);
                    s->y[i] = FV(o + 3 + 2 * i);
                }
                break;
            }
            }
        }
#undef FV
    }
    fclose(f);
    if (have)
        draw_frame();
}

#ifdef H7_MENU7
__attribute__((weak)) int h7_stubs_args(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
    const char *ramdisk = 0, *list = 0, *out = 0, *state = 0;
    float p = 0, t = -1;
    int check = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ramdisk") && i + 1 < argc) ramdisk = argv[++i];
        else if (!strcmp(argv[i], "--list") && i + 1 < argc) list = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state = argv[++i];
        else if (!strcmp(argv[i], "--p") && i + 1 < argc) p = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--t") && i + 1 < argc) t = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--check-full")) check = 1;
#ifdef H7_TIME
        else if (!strcmp(argv[i], "--run")) { run_mode = 1; state = "run"; }
#endif
    }
    if (!ramdisk || !out || (!list && !state))
        die("usage: h7 --ramdisk menu-v7.sbm (--list scene.dl | --state st) --out out.ppm [--stats] [--check-full]");
    map_fixed(0x84000000UL + G7_HOSTOFF, 0x04800000UL);
    map_fixed((uintptr_t)FB, G7_FRAME_BYTES);
    size_t rn = load_ramdisk(ramdisk);
    if (!sb7_open(RD_BASE, RD_BASE + rn))
        die("no valid SB7ASSET container in the ramdisk");
#ifdef H7_MENU7
    if (h7_stubs_args)
        h7_stubs_args(argc, argv);
#endif
#ifdef H7_TIME
    if (run_mode) {
        static char cmd[1024];
        int r = menu7_main(0, cmd, sizeof cmd);
        printf("menu7_main -> %d\n", r);
        write_ppm(out, FB);
        return 0;
    }
#endif
    if (!g7_init())
        die("g7_init failed (fonts missing?)");
    g7_bake();
    struct g7_stats st;
    g7_get_stats(&st);
    fprintf(stderr, "bake %.1f ms host (x12 ~ %.0f ms on the A55)\n", st.bake_us / 1e3, st.bake_us * 12 / 1e3);
    if (list)
        parse_list(list);
#ifdef H7_MENU7
    else {
        static struct g7_frame f;
        if (!strcmp(state, "intro")) {
            /* the intro as intro7_run plays it, up to T: 30 fps steps of m7_step + intro7_frame, so the
             * springs that run beside it (the list's room for the island) are where they are at T */
            float T = t < 0 ? 0 : t, ti = 0;
            m7_prepare_boot();
            for (;;) {
                float nt = ti + 1000.0f / 30 < T ? ti + 1000.0f / 30 : T;
                m7_step((nt - ti) / 1000);
                ti = nt;
                intro7_frame(&f, ti);
                if (ti >= T)
                    break;
            }
        }
        else {
            if (!m7_preview_state(state, p))
                die("unknown state");
            m7_build(&f, 0);
        }
        F = f;
        draw_frame();
    }
#else
    else {
        (void)p;
        (void)t;
        die("--state needs an H7_MENU7 build (menu7.c, intro7.c, loader_stubs.c)");
    }
#endif
    if (stats)
        fprintf(stderr, "worst frame %.2f ms host (x12 ~ %.1f ms)\n", worst_ms, worst_ms * 12);
    if (check) {
        static uint32_t a[G7_W * G7_H];
        memcpy(a, FB, sizeof a);
        F.flags |= G7_F_FULL;
        g7_frame(&F);
        unsigned bad = 0, worst = 0, bx = 0, by = 0;
        for (int i = 0; i < G7_W * G7_H; i++)
            if (a[i] != FB[i]) {
                unsigned d = 0;
                for (int s = 0; s < 24; s += 8) {
                    int e = (int)((a[i] >> s) & 255) - (int)((FB[i] >> s) & 255);
                    if ((unsigned)abs(e) > d) d = (unsigned)abs(e);
                }
                if (d > worst) { worst = d; bx = (unsigned)(i % G7_W); by = (unsigned)(i / G7_W); }
                bad++;
            }
        fprintf(stderr, "check-full: %u pixels differ between the dirty-rect frames and a full redraw (worst %u at %u,%u)\n",
                bad, worst, bx, by);
        if (bad) {
            write_ppm(out, a);
            return 2;
        }
    }
    write_ppm(out, FB);
    return 0;
}
