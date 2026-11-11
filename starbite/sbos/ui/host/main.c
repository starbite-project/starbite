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

void sh_torch(int lit) { fprintf(stderr, "torch: %s\n", lit ? "on" : "off"); }
void sh_boot_os(const char *key) { fprintf(stderr, "slide to start: would restart into %s\n", key); }
int sh_modem(int on)
{
    if (on) sh_confirm("Start the modem?", "Receive only: its status, no calls, SMS or data.",
                       "A SIM or a network registration parks it.", "The first start is a test: be there.", "Hold to start");
    else fprintf(stderr, "modem: would stop\n");
    return 0;
}
void sh_confirmed(int ok) { fprintf(stderr, "confirm: %s\n", ok ? "held: the modem would start" : "cancelled"); }
/* a pretend network for the previews: on -> searching -> data up on "Operator" LTE, 3 bars */
int sh_cellular(int on)
{
    sh_cell_t c = { on ? 1 : 0, 0, 0, on ? 7 : 0, on ? 3 : -1, on ? SH_DATA_UP : SH_DATA_OFF, "", "" };
    if (on) memcpy(c.oper, "Operator", 9);
    fprintf(stderr, "cellular: %s\n", on ? "on (pretend: data up)" : "off");
    sh_cell(&c);
    return 0;
}
void sh_brightness(int level) { fprintf(stderr, "brightness: level %d\n", level); }
void sh_vibrate(void) { fprintf(stderr, "vibrate: the timer ran out\n"); }
void sh_haptic(void) { }
/* wifi-sbos-next: a pretend Wi-Fi for the previews (made-up network names; nothing is joined): on lists four
 * networks, a join connects, the Internet test answers 200 */
static sh_wifi_t HW;
static void hw_push(void) { sh_wifi(&HW); }
int sh_wifi_power(int on)
{
    static const char *const N[4] = { "Home Wi-Fi", "Guest", "Upstairs 5G", "Cafe Corner" };
    static const int R[4] = { -48, -55, -67, -80 }, S[4] = { 1, 0, 1, 1 }, B[4] = { 1, 0, 1, 0 };
    memset(&HW, 0, sizeof(HW));
    HW.state = on ? SH_WIFI_UP : SH_WIFI_OFF;
    if (on) {
        HW.nnets = 4;
        for (int i = 0; i < 4; i++) {
            strcpy(HW.nets[i].ssid, N[i]);
            HW.nets[i].rssi = R[i], HW.nets[i].secure = S[i], HW.nets[i].band5 = B[i];
        }
    }
    fprintf(stderr, "wifi: %s\n", on ? "on" : "off");
    hw_push();
    return 0;
}
int sh_wifi_scan(void) { fprintf(stderr, "wifi: scan\n"); hw_push(); return 0; }
const char *sh_wifi_why(void) { return ""; }
int sh_wifi_join(const char *ssid, const char *pass)
{
    fprintf(stderr, "wifi: join (a passphrase of %d characters)\n", pass ? (int)strlen(pass) : 0);
    HW.state = SH_WIFI_CONNECTED;
    strncpy(HW.ssid, ssid, 32);
    strcpy(HW.ip, "192.168.1.23");
    hw_push();
    return 0;
}
int sh_wifi_fetch(const char *url)
{
    fprintf(stderr, "wifi: fetch %s\n", url);
    HW.fetch_state = 2, HW.fetch_ok = 1, HW.fetch_status = 200, HW.fetch_bytes = 1256, HW.fetch_ms = 520;
    HW.fetch_tls_ms = 380;
    strcpy(HW.fetch_host, "example.com");
    strcpy(HW.fetch_why, "HTTPS through the phone's own TCP and TLS");
    hw_push();
    return 0;
}
static int screen_off;
void sh_slept(void) { screen_off = 1; fprintf(stderr, "sleep: the screen would go off now\n"); }

/* SBTK_CORES=n: the bands over n threads (the phone's four A55), to check that a split frame is bit for bit the
 * serial one and to time it */
#include <pthread.h>
static struct { void (*fn)(void *, int); void *arg; int n, next; pthread_mutex_t m; } PJ = { 0, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER };
static void *pworker(void *x)
{
    (void)x;
    for (;;) {
        pthread_mutex_lock(&PJ.m);
        int i = PJ.next++;
        pthread_mutex_unlock(&PJ.m);
        if (i >= PJ.n) return 0;
        PJ.fn(PJ.arg, i);
    }
}
static int ncores = 1;
static void host_par(void (*fn)(void *, int), void *arg, int n)
{
    PJ.fn = fn; PJ.arg = arg; PJ.n = n; PJ.next = 0;
    pthread_t t[8];
    for (int k = 0; k < ncores - 1; k++) pthread_create(&t[k], 0, pworker, 0);
    pworker(0);
    for (int k = 0; k < ncores - 1; k++) pthread_join(t[k], 0);
}

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
        else if (!strcmp(w, "power")) { p->kind = 5; p->ms = 20; }      /* the Power key: sleep, or wake if off */
        n++;
    }
    return n;
}

static double memwalk(tk_layer *l, int depth);
static int film(tk_comp *c, const char *out, const char *script, int half)
{
    step st[64];
    int n = parse(script, st, 64);
    float total = 0;
    for (int i = 0; i < n; i++) total += st[i].ms;
    total += 1500;     /* let everything settle */
    int none = !strcmp(out, "none");
    FILE *f = none ? 0 : !strcmp(out, "-") ? stdout : fopen(out, "wb");
    FILE *log = f == stdout ? stderr : stdout;
    tk_bmp h = { 0 };
    if (!none) tk_bmp_new(&h, TK_W / 2, TK_H / 2);
    const float dt = 1000.0f / 60;
    float t = 0, s0 = 0;
    int si = 0, phase = -1, frame = 0, rest_frames = 0;
    tk_comp_frame(c, 0, 1);
    sh_idle();
    while (t < total || sh_moving()) {
        /* the finger: events at 120 Hz between frames (the touch controller's rate) */
        uint64_t sh0 = tk_us();          /* P3i: the shell's own time in the frame (touch handling + tick) */
        for (int sub = 1; sub <= 2; sub++) {
            float te = t + dt * sub / 2;
            while (si < n && te >= s0 + st[si].ms) {
                if (phase >= 0 && st[si].kind && st[si].kind != 5) {
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
            if (p->kind == 5) {
                if (phase < 0) {
                    fprintf(stderr, "power at %.0f ms: %s\n", te, screen_off || sh_sleeping() ? "wake" : "sleep");
                    if (screen_off || sh_sleeping()) { screen_off = 0; sh_wake(); } else sh_sleep(0);
                    phase = 0;
                }
                continue;
            }
            if (phase < 0) { sh_touch(0, p->x0, p->y0, te); phase = 0; continue; }
            if (p->kind == 2) {
                float e = u < 1 ? u : 1;
                e = 1 - (1 - e) * (1 - e) * .3f - (1 - e) * .7f;     /* a finger: fast start, slight ease */
                e = u;
                sh_touch(1, p->x0 + (p->x1 - p->x0) * e, p->y0 + (p->y1 - p->y0) * e, te);
            }
        }
        int mv = sh_tick(dt / 1000, t);
        double shell_ms = (tk_us() - sh0) / 1000.0;
        int motion = mv || sh_moving();
        tk_comp_frame(c, motion, 0);
        fprintf(log, "F %d %.1f %s px %d comp %.2f present %.2f total %.2f", frame, t, c->last_half ? "half" : "full",
               c->last_px, c->us_comp / 1000.0, c->us_present / 1000.0, c->last_us / 1000.0);
        fprintf(log, " idle %.2f halfs %.2f shell %.2f", c->us_idle / 1000.0, c->us_halfs / 1000.0, shell_ms);
        if (getenv("SBTK_HASH")) {             /* what the display shows now (two buffers: the one just presented) */
            uint64_t hh = 1469598103934665603ULL;
            for (size_t i = 0; i < (size_t)TK_W * TK_H; i++) hh = (hh ^ c->panel.px[i]) * 1099511628211ULL;
            fprintf(log, " hash %016llx", (unsigned long long)hh);
        }
        fputc('\n', log);
        if (getenv("SBTK_DUMP") && frame == atoi(getenv("SBTK_DUMP"))) write_ppm(getenv("SBTK_DUMPTO"), &c->panel);
        if (none) ;
        else if (half) {
            tk_bmp_down2(&h, &c->panel);
            fwrite(h.px, 4, (size_t)h.w * h.h, f);
        } else
            fwrite(c->panel.px, 4, (size_t)TK_W * TK_H, f);
        t += dt;
        frame++;
        rest_frames = motion ? 0 : rest_frames + 1;
        if (!motion) sh_idle();     /* between frames: on the phone, the time left in the frame */
        if (t > total + 8000) break;
    }
    (void)rest_frames;
    tk_bmp_free(&h);
    if (none) ;
    else if (f != stdout) fclose(f); else fflush(f);
    if (getenv("SBTK_MEM")) fprintf(stderr, "mem: layers %.1f MB\n", memwalk(c->root, 0) / 1e6);
    fprintf(stderr, "film: %d frames; heap %.1f MB (peak %.1f MB)\n", frame, tk_heap_used() / 1e6, tk_heap_peak() / 1e6);
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
static void up2x_rows(uint8_t *d, const uint8_t *s, uint32_t w, uint32_t y0, uint32_t y1)
{
    uint32_t sw = w / 2, sh = 2960 / 2;
    for (uint32_t y = y0 & ~1U; y < y1; y += 2) {
        uint32_t sy = y / 2, sy1 = sy + 1 < sh ? sy + 1 : sy;
        const uint8_t *r0 = s + (size_t)sy * sw * 4, *r1 = s + (size_t)sy1 * sw * 4;
        uint8_t *o0 = d + (size_t)y * w * 4, *o1 = o0 + (size_t)w * 4;
        for (uint32_t x = 0; x + 4 < sw; x += 4) {
            uint8x16_t a = vld1q_u8(r0 + x * 4), an = vld1q_u8(r0 + x * 4 + 4);
            uint8x16_t b = vld1q_u8(r1 + x * 4), bn = vld1q_u8(r1 + x * 4 + 4);
            uint8x16_t ah = vrhaddq_u8(a, an), bh = vrhaddq_u8(b, bn);
            uint32x4x2_t top = vzipq_u32(vreinterpretq_u32_u8(a), vreinterpretq_u32_u8(ah));
            uint8x16_t va = vrhaddq_u8(a, b), vh = vrhaddq_u8(ah, bh);
            uint32x4x2_t mid = vzipq_u32(vreinterpretq_u32_u8(va), vreinterpretq_u32_u8(vh));
            vst1q_u32((uint32_t *)(o0 + x * 8), top.val[0]);
            vst1q_u32((uint32_t *)(o0 + x * 8 + 16), top.val[1]);
            if (y + 1 < y1) { vst1q_u32((uint32_t *)(o1 + x * 8), mid.val[0]); vst1q_u32((uint32_t *)(o1 + x * 8 + 16), mid.val[1]); }
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
        { "blend, a half-resolution layer over another", 2 }, { "blend, a full-resolution layer over another", 3 },
        { "2x upscale, half frame to a full frame in RAM", 4 } };
    for (int k = 0; k < 5; k++) {
        uint64_t best = ~0ull, bytes = 0;
        for (int r = 0; r < 5; r++) {
            uint64_t t0 = tk_us();
            switch (K[k].k) {
            case 0: { uint32x4_t v = vdupq_n_u32(0xff203040u); for (size_t i = 0; i < (size_t)FW * FH; i += 4) vst1q_u32((uint32_t *)f0 + i, v); bytes = (uint64_t)FW * FH * 4; break; }
            case 1: memcpy(f0, f1, (size_t)FW * FH * 4); bytes = (uint64_t)FW * FH * 8; break;
            case 2: blend_rows(h0, h1, HW, 0, HH); bytes = (uint64_t)HW * HH * 12; break;
            case 3: blend_rows(f0, f1, FW, 0, FH); bytes = (uint64_t)FW * FH * 12; break;
            case 4: up2x_rows(f0, h0, FW, 0, FH); bytes = (uint64_t)FW * FH * 4 + (uint64_t)HW * HH * 4; break;
            }
            uint64_t t = tk_us() - t0;
            if (t < best) best = t;
        }
        printf("hostbench: %s: %llu.%llu ms, %llu MB/s\n", K[k].n, (unsigned long long)(best / 1000), (unsigned long long)(best % 1000 / 100),
               (unsigned long long)(bytes / (best ? best : 1)));
    }
}

static double memwalk(tk_layer *l, int depth)
{
    double t = 0;
    for (; l; l = l->next) {
        double b = (double)l->bmp.w * l->bmp.h * 4, h = (double)l->half.w * l->half.h * 4;
        double g = (double)l->glass.w * l->glass.h + (double)l->ghalf.w * l->ghalf.h;
        if (b + h + g > 2e5) fprintf(stderr, "mem %*s%-14s %6.1f MB (half %.1f, glass %.1f)%s\n", depth * 2, "", l->name, (b + h + g) / 1e6, h / 1e6, g / 1e6, l->hidden ? " hidden" : "");
        t += b + h + g + memwalk(l->child, depth + 1);
    }
    return t;
}

/* SBTK_DBL=2: two buffers in turn; 3: three, as the phone (P2t): the display shows `shown`, the last flip `req`
 * latches unless the frame number says it missed a TE (every 5th, every 7th: a pattern), the free one is the oldest
 * that is neither */
static uint32_t *dbl[3];
static int nbuf, shown, req = 0, bfno[3] = { 0, -1, -1 }, fno_h = 1;
static uint32_t *dbl_acquire(void *ctx, int *age)
{
    (void)ctx;
    if (nbuf == 2) {
        int b = req ^ 1;
        *age = bfno[b] < 0 ? 0 : fno_h - 1 - bfno[b];
        req = b; bfno[b] = fno_h++;
        return dbl[b];
    }
    if (fno_h % 5 && fno_h % 7) shown = req;          /* the last flip latched (else it is still queued) */
    int b = -1;
    for (int i = 0; i < 3; i++) if (i != shown && i != req && (b < 0 || bfno[i] < bfno[b])) b = i;
    *age = bfno[b] < 0 ? 0 : fno_h - 1 - bfno[b];
    req = b; bfno[b] = fno_h++;
    return dbl[b];
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: sbtk-host PAK still|film|bench ...\n"); return 2; }
    uint32_t len;
    uint8_t *pak = readall(argv[1], &len);
    if (!strcmp(argv[2], "bench")) { bench(); return 0; }
    if (!tk_pack_open(pak, len)) { fprintf(stderr, "bad pack\n"); return 1; }
    static tk_comp c;
    /* the phone's frame buffers are not in its UI heap (the old UI's back buffer, the framebuffer) */
    if (!tk_comp_init(&c, getenv("SBTK_HEAP_MB") ? calloc((size_t)TK_W * TK_H, 4) : 0,
                      getenv("SBTK_HEAP_MB") ? calloc((size_t)TK_W * TK_H, 4) : 0)) return 1;
    { extern int getenv_dbg; getenv_dbg = getenv("SBTK_DBG") != 0; }
    if (getenv("SBTK_DBL")) {                  /* the phone's two scanout buffers (P2r): presents alternate */
        nbuf = atoi(getenv("SBTK_DBL")) == 3 ? 3 : 2;
        dbl[0] = c.panel.px;
        dbl[1] = calloc((size_t)TK_W * TK_H, 4);
        dbl[2] = calloc((size_t)TK_W * TK_H, 4);
        c.acquire = dbl_acquire;
    }
    if (getenv("SBTK_CORES")) { ncores = atoi(getenv("SBTK_CORES")); if (ncores > 1) { c.par = host_par; tk_par = host_par; } }
    if (getenv("SBTK_NOLENS")) tk_glass_lens = 0;       /* v6: the concept's baked glass (to compare) */
    uint64_t t0 = tk_us();
    sh_init(&c);
    fprintf(stderr, "bake: %.1f ms; heap %.1f MB (peak %.1f MB)\n", (tk_us() - t0) / 1000.0, tk_heap_used() / 1e6, tk_heap_peak() / 1e6);
    if (getenv("SBTK_MEM")) fprintf(stderr, "mem: layers %.1f MB\n", memwalk(c.root, 0) / 1e6);
    if (!strcmp(argv[2], "still") && argc >= 4) {
        for (int i = 4; i < argc; i++) apply_state(argv[i]);
        tk_comp_frame(&c, 0, 1);
        fprintf(stderr, "frame: %.2f ms; heap %.1f MB (peak %.1f MB)\n", c.last_us / 1000.0, tk_heap_used() / 1e6, tk_heap_peak() / 1e6);
        write_ppm(argv[3], &c.panel);
        return 0;
    }
    if (!strcmp(argv[2], "clocktest")) {
        /* a new minute must reach the panel through an ordinary (damage-only) frame */
        tk_comp_frame(&c, 0, 1);
        uint32_t before = 0, after = 0;
        for (int y = PT(96); y < PT(204); y++) for (int x = 0; x < TK_W; x += 3) before = before * 31 + c.panel.px[y * TK_W + x];
        sh_status(21, 6, 2, 10, 11, 87, 41);
        int px = tk_comp_frame(&c, 0, 0);
        for (int y = PT(96); y < PT(204); y++) for (int x = 0; x < TK_W; x += 3) after = after * 31 + c.panel.px[y * TK_W + x];
        printf("clocktest: %s (%d px redrawn)\n", before != after && px > 0 ? "PASSED: the clock changed on the panel" : "FAILED: the panel kept the old minute", px);
        return before != after && px > 0 ? 0 : 1;
    }
    if (!strcmp(argv[2], "frames") && argc >= 3) {
        /* the cost of one frame of a state with everything damaged: motion (half + present) and rest (full) */
        for (int i = 3; i < argc; i++) apply_state(argv[i]);
        for (int m = 1; m >= 0; m--) {
            double tc = 0, tp = 0;
            int n = 40;
            tk_comp_frame(&c, m, 1);
            for (int i = 0; i < n; i++) { tk_comp_frame(&c, m, 1); tc += c.us_comp; tp += c.us_present; }
            printf("frames %s: %s compose %.2f present %.2f ms\n", argc > 3 ? argv[3] : "lock", m ? "motion" : "rest", tc / n / 1000, tp / n / 1000);
        }
        return 0;
    }
    if (!strcmp(argv[2], "selftest") && argc >= 4) {
        /* the phone's self test: every film of FILMS.txt in one run, each from the reset lock screen; the heap after
         * each (with SBTK_HEAP_MB=160: the phone's heap) */
        FILE *ff = fopen(argv[3], "r");
        char line[2048];
        while (ff && fgets(line, sizeof line, ff)) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = 0;
            sh_set("reset", "", 0);
            film(&c, "none", colon + 1, 1);
            fprintf(stderr, "selftest: %s: heap %zu MiB (peak %zu), largest free %llu MiB\n", line, tk_heap_used() >> 20,
                    tk_heap_peak() >> 20, (unsigned long long)(tk_heap_largest() >> 20));
        }
        return 0;
    }
    if (!strcmp(argv[2], "film") && argc >= 5) return film(&c, argv[3], argv[4], argc > 5 && !strcmp(argv[5], "half"));
    return 2;
}
