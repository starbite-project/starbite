/* Host preview of the boot intro (Lima VM, aarch64 Linux): the real shim/starlte/anim.c
 * (star_intro_splash, then menu v6's star_reveal through the real menu_v6) frame by frame.
 *
 *   - the panel (STAR_FB_BASE) starts as S-Boot leaves it: black, logo.jpg at (180, 1144)
 *   - hw_ticks() is a fake clock: each call is one frame (FRAME_US), so every frame anim.c
 *     presents is seen; star_sprite_to / star_sprite_rect are ports of xnu_boot.c's
 *     sprite_walk_to (opaque copy, faded), not a blend, so the preview shows what the loader does
 *   - after every frame the panel is compared with anim.c's OFF buffer (what was composed): a
 *     pixel that differs was drawn but never presented (or presented stale) -> "stray"
 *   - frames: every frame at 1/4 size into frames.raw (u32 ms, then 360 x 740 RGB), and the
 *     full panel as PPM at the times given on the command line
 *
 * run.sh builds and runs it. Usage: ./ap ramdisk-v6.bin catalog.sbc logo.rgb ms...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "../../shim/starlte/menu6.c"

#define FRAME_US 16667U
#define OFFBUF ((uint32_t *)0x84000000UL)
#define FBUF ((uint32_t *)STAR_FB_BASE)

static unsigned char *rd;
static size_t rdn;

/* ---- the loader's sprite walker (xnu_boot.c sprite_walk_to), over the ramdisk file ---- */
static uint32_t rd32(size_t off) { uint32_t v; memcpy(&v, rd + off, 4); return v; }
static uint32_t fade_px(uint32_t c, unsigned fade)
{
    return 0xff000000U | ((((c >> 16) & 255U) * fade >> 8) << 16) | ((((c >> 8) & 255U) * fade >> 8) << 8) |
           ((c & 255U) * fade >> 8);
}
int star_sprite_to(unsigned want, uint32_t *dst, int ox, int oy, int at, unsigned fade)
{
    uint32_t *fb = dst ? dst : FBUF;
    uint32_t n = rd32(8);
    size_t q = 12;
    int drawn = 0;
    for (uint32_t r = 0; r < n && r < 64; r++) {
        if (q + 24 > rdn) return -1;
        uint32_t id = rd32(q), x = rd32(q + 4), y = rd32(q + 8), w = rd32(q + 12), ht = rd32(q + 16), runs = rd32(q + 20);
        q += 24;
        if (!w || q + 8ULL * runs > rdn || x + w > W || y + ht > H) return -1;
        if (id == want && at) {
            if (ox < 0 || oy < 0 || (uint32_t)ox + w > W || (uint32_t)oy + ht > H) return -1;
            x = (uint32_t)ox; y = (uint32_t)oy;
        }
        if (id == want) {
            uint64_t left = (uint64_t)w * ht;
            uint32_t px = 0;
            uint32_t *row = fb + (uint64_t)y * W + x;
            for (uint32_t k = 0; k < runs; k++) {
                uint32_t length = rd32(q + 8 * k), c = rd32(q + 8 * k + 4);
                if (length > left) return -1;
                left -= length;
                if (fade < 256) c = fade_px(c, fade);
                for (; length; --length) {
                    row[px] = c;
                    if (++px == w) { px = 0; row += W; }
                }
            }
            drawn++;
        }
        q += 8ULL * runs;
    }
    return drawn;
}
int star_sprite_rect(unsigned id, unsigned *x, unsigned *y, unsigned *w, unsigned *h)
{
    uint32_t n = rd32(8);
    size_t q = 12;
    for (uint32_t r = 0; r < n && r < 64; r++) {
        if (rd32(q) == id) { *x = rd32(q + 4); *y = rd32(q + 8); *w = rd32(q + 12); *h = rd32(q + 16); return 1; }
        q += 24 + 8ULL * rd32(q + 20);
    }
    return 0;
}
int star_font_blob(uint64_t *s, uint64_t *e)
{
    for (size_t i = 0; i + 8 < rdn; i += 4)
        if (!memcmp(rd + i, "SBFONT01", 8)) { *s = (uintptr_t)(rd + i); *e = (uintptr_t)(rd + rdn); return 1; }
    return 0;
}
int star_mmu_enabled(void) { return 1; }

/* ---- the rest of the loader, stubbed ---- */
int em_main(const struct star_info *a, char *b, unsigned c) { (void)a; (void)b; (void)c; return 0; }
static unsigned char cat[65536];
static size_t catn;
int store_find(const char *name, struct store_ent *e) { if (strcmp(name, "catalog")) return 0; e->size = catn; return 1; }
int store_load(const struct store_ent *e, uintptr_t dst, uint64_t cap) { (void)cap; memcpy((void *)dst, cat, e->size); return 0; }
int star_entry_ready(unsigned i, const char **why)
{
    const struct cat_ent *e = cat_get(i);
    int ok = strcmp(e->key, "linux") != 0;
    if (why) *why = ok ? 0 : "NO KERNEL";
    return ok;
}
static struct star_settings S = { .default_key = "ios26", .last_key = "win11", .timeout_s = 0 };
void star_menu_begin(void) {}
struct star_settings *star_menu_settings(int *v) { if (v) *v = 1; return &S; }
int star_menu_settings_save(void) { return 0; }
int star_menu_ready_key(const char *k) { return cat_find(k); }
int star_menu_next(void) { return -1; }
void star_menu_remember(int i) { (void)i; }
int star_menu_failure(const char **a, const char **b) { (void)a; (void)b; return 0; }
void star_fb_row_base(unsigned r) { (void)r; }
void star_fb_fill(uint32_t p) { (void)p; }
void star_fb_text_c(unsigned r, const char *s, uint32_t a, uint32_t b) { (void)r; (void)s; (void)a; (void)b; }
unsigned hw_keys(void) { return 0; }
void hw_wait_ms(unsigned m) { (void)m; }
uint64_t hw_freq(void) { return 1000000; }
void hw_tp_clear(void) {}
void hw_tp_start(void) {}
void hw_power_off(void) {}
void hw_reboot(uint32_t v) { (void)v; }

/* ---- the clock, the frame dumps, the stray check ---- */
static uint64_t clk;            /* us */
static int64_t last_ms = -1;    /* time of the frame now on the panel */
static int *want_ms, nwant, wanted;
static FILE *film;
static int frames, stray_frames;
static double cpu_max, cpu_sum;
static struct timespec ts_ret;
static int in_intro = 1;
static long stray_after_clean;

static void dump_full(const char *f)
{
    FILE *o = fopen(f, "wb");
    fprintf(o, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; ++i) { uint32_t p = FBUF[i]; fputc(p >> 16, o); fputc(p >> 8, o); fputc(p, o); }
    fclose(o);
}

static void frame_done(void)
{
    if (last_ms < 0)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double ms = (now.tv_sec - ts_ret.tv_sec) * 1e3 + (now.tv_nsec - ts_ret.tv_nsec) / 1e6;
    if (ms > cpu_max) cpu_max = ms;
    if (ms > 4.0)
        printf("  frame %4ld ms took %.2f ms to compose and present (host)\n", (long)last_ms, ms);
    cpu_sum += ms;
    frames++;
    /* 1/4 size film frame (box filter) */
    uint32_t t = (uint32_t)last_ms;
    fwrite(&t, 4, 1, film);
    static unsigned char line[(W / 4) * 3];
    for (int y = 0; y < H / 4; ++y) {
        for (int x = 0; x < W / 4; ++x) {
            unsigned r = 0, g = 0, b = 0;
            for (int k = 0; k < 16; ++k) {
                uint32_t p = FBUF[(uint64_t)(y * 4 + k / 4) * W + x * 4 + k % 4];
                r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
            }
            line[3 * x] = (unsigned char)(r / 16); line[3 * x + 1] = (unsigned char)(g / 16); line[3 * x + 2] = (unsigned char)(b / 16);
        }
        fwrite(line, 1, sizeof(line), film);
    }
    /* stray: composed (OFF) but not on the panel. At 0 ms nothing is presented yet: the panel is
     * S-Boot's splash and OFF the prepared copy without its star. */
    long n = 0;
    if (last_ms == 0)
        goto film_done;
    int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
    for (long i = 0; i < (long)W * H; ++i)
        if ((OFFBUF[i] | 0xff000000U) != (FBUF[i] | 0xff000000U)) {
            /* before the splash area is cleaned, OFF lacks S-Boot's faint JPEG noise and its star */
            uint32_t a = OFFBUF[i], b = FBUF[i];
            int d = 0;
            for (int sh = 0; sh < 24; sh += 8) { int e = (int)((a >> sh) & 255) - (int)((b >> sh) & 255); if (e < 0) e = -e; if (e > d) d = e; }
            if (d > 12) {
                int x = (int)(i % W), y = (int)(i / W);
                n++;
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
        }
    if (n) {
        stray_frames++;
        if (last_ms > 420 && in_intro) stray_after_clean += n;
        printf("  frame %4ld ms: %ld pixels composed but not presented, in %d,%d..%d,%d\n", (long)last_ms, n, bx0, by0, bx1, by1);
    }
film_done:
    while (wanted < nwant && last_ms >= want_ms[wanted]) {
        char f[64];
        snprintf(f, sizeof(f), "f%04d.ppm", want_ms[wanted]);
        dump_full(f);
        printf("  %s = the frame of %ld ms\n", f, (long)last_ms);
        wanted++;
    }
}

uint64_t hw_ticks(void)
{
    frame_done();
    last_ms = (int64_t)(clk / 1000);
    uint64_t v = clk;
    clk += FRAME_US;
    clock_gettime(CLOCK_MONOTONIC, &ts_ret);
    return v;
}

/* menu_v6 waits for input once the intro is over: the run ends there. */
int hw_tp_event(int *a, int *b, int *c)
{
    (void)a; (void)b; (void)c;
    frame_done();
    dump_full("final.ppm");
    printf("%d frames, %d with stray pixels (%ld after the splash was cleaned); compose %.2f ms avg, %.2f max (host)\n",
           frames, stray_frames, stray_after_clean, cpu_sum / frames, cpu_max);
    fclose(film);
    exit(0);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s ramdisk catalog logo.rgb [ms...]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); rdn = (size_t)ftell(f); rewind(f);
    rd = malloc(rdn);
    if (fread(rd, 1, rdn, f) != rdn) return 1;
    fclose(f);
    f = fopen(argv[2], "rb"); catn = fread(cat, 1, sizeof(cat), f); fclose(f);
    nwant = argc - 4;
    want_ms = calloc((size_t)nwant + 1, sizeof(int));
    for (int i = 0; i < nwant; ++i) want_ms[i] = atoi(argv[4 + i]);
    if (mmap((void *)0x84000000UL, 0x8000000UL, 3, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED ||
        mmap((void *)0x8c000000UL, 1 << 20, 3, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED ||
        mmap((void *)STAR_FB_BASE, 32 << 20, 3, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    /* S-Boot's splash: black, logo.jpg (1079 x 1747 RGB) at (180, 1144) */
    for (long i = 0; i < (long)W * H; ++i) FBUF[i] = 0xff000000U;
    f = fopen(argv[3], "rb");
    for (int y = 0; y < 1747; ++y)
        for (int x = 0; x < 1079; ++x) {
            unsigned char p[3];
            if (fread(p, 1, 3, f) != 3) return 1;
            FBUF[(uint64_t)(1144 + y) * W + 180 + x] = 0xff000000U | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    fclose(f);
    dump_full("f0000-splash.ppm");
    film = fopen("frames.raw", "wb");
    unsigned x, y, w, h;
    star_sprite_rect(3, &x, &y, &w, &h); printf("splash star marker: %u,%u size %u\n", x, y, w);
    star_sprite_rect(2, &x, &y, &w, &h); printf("header star marker: %u,%u size %u\n", x, y, w);
    star_intro_splash();
    printf("intro over at %ld ms\n", (long)last_ms);
    in_intro = 0;
    clk += 40000;                                   /* the loader's work before the menu */
    struct star_info in;
    memset(&in, 0, sizeof(in));
    char cmd[64] = "";
    menu_v6(&in, cmd, sizeof(cmd));
    puts("menu_v6 returned");
    return 1;
}
