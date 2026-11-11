/* SPDX-License-Identifier: GPL-2.0-only
 * gwin-host: L2's kernel glue (starbite/guest/next/gwin.c) and its unattended check on the host, with the real Next
 * shell (starbite/sbos/ui: shell_lock.c, the toolkit, host/plat.c, host/main.c's other platform fakes) and a fake
 * guest in place of kern/guest.c + sbghost: it comes up 1.5 s after the start, publishes foot's frame (GWIN_PPM, a
 * 720 x 916 P6 PPM from the QEMU run; else a dark surface), draws each typed character as a block (a new frame per
 * key), sets the marker when a typed line holds STARBITE-TERM-OK, counts the touches, and is off 0.7 s after the stop.
 * A fake clock steps 100 ms per turn. Writes OUT/gwin-typed.ppm (the window after the command) and prints the self
 * test's log; exit 0 when it passed.
 *   starbite/guest/next/host/run.sh UI_TREE PAK OUT     (UI_TREE: a checkout of the toolkit with the L2 patch)
 * GWIN_STORE=none: the store has no guest-wl files (the check must fail with that reason).
 */
#include "sb.h"
#include "kern.h"
#include "guest.h"
#include "sbtk.h"
#include "shell.h"
#include "../../host/sbghost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gwin_now(void);
int gwin_selftest_step(void);

/* ---- the fake clock and log ---- */
static uint64_t now_ms = 1000;
uint64_t sb_ms(void) { return now_ms; }
void sb_log(const char *s) { printf("[%6llu ms] %s\n", (unsigned long long)now_ms, s); }
void sb_logf(const char *a, const char *b) { printf("[%6llu ms] %s%s\n", (unsigned long long)now_ms, a, b); }
void ub_init(struct ub *u) { u->n = 0; u->b[0] = 0; }
void ub_s(struct ub *u, const char *s) { while (*s && u->n + 1 < sizeof u->b) u->b[u->n++] = *s++; u->b[u->n] = 0; }
void ub_dec(struct ub *u, uint64_t v)
{
    char t[24];
    snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    ub_s(u, t);
}

/* ---- the fake guest (kern/guest.c's API as gwin.c uses it) ---- */
static int st = GUEST_OFF, shown, mode = GUEST_APP, term_ok;
static uint64_t t_state;
static uint32_t seq, drawn;
static char line[128], typed[512];
static int nline, ntyped;
static uint8_t *rgb;
static struct sbg_stats stats;
static const char *why = "";

int guest_available_mode(int m) { return m == GUEST_WL && !(getenv("GWIN_STORE") && !strcmp(getenv("GWIN_STORE"), "none")); }
int guest_start_mode(int m)
{
    if (st != GUEST_OFF && st != GUEST_FAILED) return -1;
    if (!guest_available_mode(m)) { why = "the Linux apps' files are not in the store"; return -4; }
    mode = m;
    st = GUEST_STARTING;
    t_state = now_ms;
    term_ok = 0;
    seq = drawn = 0;
    ntyped = nline = 0;
    why = "";
    return 0;
}
int guest_state(void) { return st; }
int guest_mode(void) { return mode; }
const char *guest_why(void) { return why; }
int guest_stop(void) { if (st == GUEST_RUNNING || st == GUEST_STARTING) { st = GUEST_STOPPING; t_state = now_ms; } return 0; }
int guest_owns_m3(void) { return st != GUEST_OFF && st != GUEST_FAILED; }
int hw_m3_on(void) { return guest_owns_m3(); }
void guest_show(int on) { shown = on; }
int guest_shown(void) { return shown && st == GUEST_RUNNING; }
void guest_full_next(void) { drawn = 0; }
int guest_frame_new(void) { return guest_shown() && seq != drawn; }
int guest_term_ok(void) { return term_ok; }
int guest_type(char c)
{
    if (!guest_shown()) return 0;
    if (c == '\n') {
        line[nline] = 0;
        if (strstr(line, "STARBITE-TERM-OK")) { term_ok = 1; sb_log("linux| STARBITE-TERM-OK (fake)"); }
        nline = 0;
    } else if (nline + 1 < (int)sizeof line)
        line[nline++] = c;
    if (ntyped + 1 < (int)sizeof typed) typed[ntyped++] = c;
    seq++;
    return 1;
}
int sbg_touch(uint32_t slot, uint32_t phase, int32_t x, int32_t y)
{
    (void)slot;
    printf("[%6llu ms] fake guest: touch phase %u at %d,%d\n", (unsigned long long)now_ms, phase, x, y);
    stats.touches_sent++;
    return 1;
}
const struct sbg_stats *sbg_stats(void) { return &stats; }
/* the surface at 2x, opaque: foot's frame, the typed characters as 16 x 28 blocks from row 260 (45 a row) */
int guest_blit(uint32_t *back, unsigned stride_px, unsigned x0, unsigned y0, int *py0, int *py1)
{
    *py0 = *py1 = -1;
    if (!guest_shown()) return 0;
    drawn = seq;
    for (int y = 0; y < SH_LINUX_H; y++)
        for (int x = 0; x < SH_LINUX_W; x++) {
            uint32_t c = 0xff242424u;
            if (rgb) {
                const uint8_t *p = rgb + ((size_t)y * SH_LINUX_W + x) * 3;
                c = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            }
            int cell = y >= 260 ? (y - 260) / 32 * 45 + x / 16 : -1;
            if (cell >= 0 && cell < ntyped && typed[cell] != ' ' && x % 16 < 12 && (y - 260) % 32 > 4 && (y - 260) % 32 < 28)
                c = 0xffddddddu;
            uint32_t *d = back + (size_t)(y0 + 2 * y) * stride_px + x0 + 2 * x;
            d[0] = d[1] = d[stride_px] = d[stride_px + 1] = c;
        }
    *py0 = (int)y0;
    *py1 = (int)y0 + 2 * SH_LINUX_H - 1;
    return SH_LINUX_H;
}
static void guest_tick(void)
{
    if (st == GUEST_STARTING && now_ms - t_state >= 1500) { st = GUEST_RUNNING; seq = 1; sb_log("fake guest: HELLO, foot's first frame"); }
    if (st == GUEST_STOPPING && now_ms - t_state >= 700) { st = GUEST_OFF; shown = 0; sb_log("fake guest: off"); }
}

/* ---- the harness ---- */
static uint8_t *readall(const char *p, uint32_t *len)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *len = (uint32_t)n;
    return b;
}
static void write_ppm(const char *name, const tk_bmp *b)
{
    FILE *f = fopen(name, "wb");
    if (!f) { perror(name); return; }
    fprintf(f, "P6\n%d %d\n255\n", b->w, b->h);
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            uint32_t c = b->px[(size_t)y * b->stride + x];
            fputc(c >> 16 & 255, f); fputc(c >> 8 & 255, f); fputc(c & 255, f);
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: gwin-host PAK OUTDIR\n"); return 2; }
    uint32_t len;
    uint8_t *pak = readall(argv[1], &len);
    const char *pp = getenv("GWIN_PPM");
    if (pp) {
        uint32_t n;
        uint8_t *d = readall(pp, &n);
        const char *want = "P6\n720 916\n255\n";
        if (n == strlen(want) + (uint32_t)SH_LINUX_W * SH_LINUX_H * 3 && !memcmp(d, want, strlen(want))) rgb = d + strlen(want);
        else fprintf(stderr, "gwin-host: %s is not a 720x916 P6 PPM: a dark surface\n", pp);
    }
    if (!tk_pack_open(pak, len)) { fprintf(stderr, "bad pack\n"); return 1; }
    static tk_comp c;
    if (!tk_comp_init(&c, 0, 0)) return 1;
    sh_init(&c);
    int r = 0, shot = 0, draws = 0;
    for (int i = 0; i < 1500 && !r; i++) {
        now_ms += 100;
        guest_tick();
        if (gwin_now()) { tk_comp_frame(&c, 0, 0); draws++; }
        r = gwin_selftest_step();
        if (!shot && stats.touches_sent >= 2) {             /* the window after the typed command, as shown */
            char out[512];
            snprintf(out, sizeof out, "%s/gwin-typed.ppm", argv[2]);
            tk_comp_frame(&c, 0, 1);
            write_ppm(out, &c.panel);
            shot = 1;
        }
    }
    printf("gwin-host: %s after %llu ms (fake clock); frames drawn for the window %d; the guest's state %d, M3 %d\n",
           r > 0 ? "PASSED" : r < 0 ? "FAILED" : "NOT DONE", (unsigned long long)(now_ms - 1000), draws, st, hw_m3_on());
    return r > 0 ? 0 : 1;
}
