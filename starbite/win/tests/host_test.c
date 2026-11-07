/* SPDX-License-Identifier: GPL-2.0-only
 * Host test for the portable cores of the stage 2 Windows drivers (sbth_core.c, sbdc_core.c).
 * Build and run: starbite/win/tests/run.sh (cc on the Mac, no WDK needed).
 *
 * The fake register files fail the test on any access outside what docs/WIN11-GPU.md allows:
 * the TMU is never written; DECON/DMA/DSIM are written only at the listed offsets; no IDMA
 * channel other than 0 is ever read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sbthermal/sbth_core.h"
#include "../sbdisplay/sbdc_core.h"

static int fails, checks;
#define CHECK(c, ...)                                                                    \
    do {                                                                                 \
        ++checks;                                                                        \
        if (!(c)) {                                                                      \
            ++fails;                                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                  \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

/* ---- fake TMU ------------------------------------------------------------------------------ */

struct ftmu {
    uint32_t reg[2][0x400];
    int glitch;                           /* return code 0 for the next n CURRENT_TEMP reads */
    int writes;
};

static uint32_t ftmu_rd(void *ctx, unsigned b, unsigned off)
{
    struct ftmu *t = ctx;
    if (b > 1 || off >= 0x1000 || (off & 3)) {
        printf("FAIL: TMU read out of range %u:%#x\n", b, off);
        ++fails;
        return 0;
    }
    if (off >= 0x40 && off <= 0x48 && t->glitch > 0) {
        --t->glitch;
        return 0;
    }
    return t->reg[b][off / 4];
}

/* Probe p's code into CURRENT_TEMP. */
static void ftmu_set_code(struct ftmu *t, unsigned b, unsigned p, unsigned code)
{
    unsigned off = p < 2 ? 0x40 : 0x44 + 4 * ((p - 2) / 3), sh = p < 2 ? 9 * p : 9 * ((p - 2) % 3);
    t->reg[b][off / 4] = (t->reg[b][off / 4] & ~(0x1ffu << sh)) | (code & 0x1ff) << sh;
}

/* One-point trim te1 = 50 for every probe: code = C - 25 + 50. */
static void ftmu_init(struct ftmu *t)
{
    memset(t, 0, sizeof(*t));
    for (unsigned b = 0; b < 2; ++b) {
        for (unsigned p = 0; p < 8; ++p)
            t->reg[b][p] = 50;
        t->reg[b][0x20 / 4] = 0x1001;      /* CORE_EN + THERM_TRIP_EN */
        t->reg[b][0x50 / 4] = (50 + 90) << 16; /* trip at 115 C */
        for (unsigned p = 0; p < 8; ++p)
            ftmu_set_code(t, b, p, 50 + 40 - 25);  /* 40 C */
    }
}

static void ftmu_set_c(struct ftmu *t, unsigned slot, int c)
{
    ftmu_set_code(t, slot < 8 ? 0 : 1, slot < 8 ? slot : slot - 8, (unsigned)(c - 25 + 50));
}

static void test_thermal(void)
{
    struct ftmu t;
    struct sbth_io io = { &t, ftmu_rd, NULL };
    struct sbth_reading r;
    struct sbth_levels lv;

    /* conversion */
    CHECK(sbth_code_to_mc(75, 50, 0, 0) == 50000, "one-point 75/50 -> 50 C");
    CHECK(sbth_code_to_mc(110, 50, 110, 1) == 85000, "two-point at te2 -> 85 C");
    CHECK(sbth_code_to_mc(80, 50, 110, 1) == 55000, "two-point midway -> 55 C");
    CHECK(sbth_code_to_mc(80, 0, 0, 0) == 50000, "te1 0 falls back to 55");

    ftmu_init(&t);
    sbth_read_all(&io, &r);
    CHECK(r.gpu_mc == 40000, "GPU 40 C, got %d", r.gpu_mc);
    CHECK(r.hottest_mc == 40000, "hottest 40 C, got %d", r.hottest_mc);
    CHECK(sbth_trip_armed(&io, 0) && sbth_trip_armed(&io, 1), "trip armed");
    CHECK(sbth_trip_mc(&io, 0) == 115000, "trip 115 C, got %d", sbth_trip_mc(&io, 0));
    CHECK(sbth_block_live(&io, 0) && sbth_block_live(&io, 1), "blocks live");

    /* probe 7 and SUB 0 (unknown places) do not count for the hottest */
    ftmu_set_c(&t, 7, 120);
    ftmu_set_c(&t, 8, 120);
    sbth_read_all(&io, &r);
    CHECK(r.hottest_mc == 40000, "unplaced probes ignored, got %d", r.hottest_mc);
    ftmu_init(&t);

    /* the code-0 glitch: two bad samples are filtered, the median survives */
    t.glitch = 2;
    {
        unsigned rej = 0;
        int mc = sbth_probe_mc(&io, 1, 1, &rej);
        CHECK(mc == 40000 && rej == 2, "glitch filtered: %d, %u rejected", mc, rej);
    }
    /* four bad samples: no temperature */
    t.glitch = 4;
    CHECK(sbth_probe_mc(&io, 1, 1, NULL) == SBTH_NO_TEMP, "4 glitches -> no temperature");
    t.glitch = 0;

    /* emulation mode or core off: block not live, no temperatures */
    t.reg[1][0x160 / 4] = 1;
    sbth_read_all(&io, &r);
    CHECK(r.gpu_mc == SBTH_NO_TEMP, "emulated SUB gives no GPU temperature");
    t.reg[1][0x160 / 4] = 0;

    /* levels */
    sbth_levels_init(&lv);
    CHECK(lv.level == SBTH_NORMAL, "starts normal");
    CHECK(sbth_levels_step(&lv, 75000, 75000) == 0 && lv.level == SBTH_NORMAL, "one reading is not enough");
    CHECK(sbth_levels_step(&lv, 75000, 75000) == 1 && lv.level == SBTH_WARM, "two readings -> warm");
    /* a run 85, 95 -> the lower (hot), not stop */
    sbth_levels_step(&lv, 85000, 85000);
    sbth_levels_step(&lv, 95000, 95000);
    CHECK(lv.level == SBTH_HOT, "run 85/95 -> hot, got %d", lv.level);
    sbth_levels_step(&lv, 95000, 95000);
    sbth_levels_step(&lv, 95000, 95000);
    CHECK(lv.level == SBTH_STOP, "stop at 95, got %d", lv.level);
    /* hysteresis: 86 C still holds stop (90 - 5) */
    for (int i = 0; i < 20; ++i)
        sbth_levels_step(&lv, 86000, 86000);
    CHECK(lv.level == SBTH_STOP, "86 C holds stop, got %d", lv.level);
    /* 84 C for 7 readings: not yet; the 8th: hot */
    for (int i = 0; i < 7; ++i)
        sbth_levels_step(&lv, 84000, 84000);
    CHECK(lv.level == SBTH_STOP, "7 readings below: still stop");
    sbth_levels_step(&lv, 84000, 84000);
    CHECK(lv.level == SBTH_HOT, "8 readings below -> hot, got %d", lv.level);
    /* a hot spike in the falling run keeps the level */
    for (int i = 0; i < 7; ++i)
        sbth_levels_step(&lv, 40000, 40000);
    sbth_levels_step(&lv, 81000, 81000);
    CHECK(lv.level == SBTH_HOT, "spike resets the fall");
    for (int i = 0; i < 8; ++i)
        sbth_levels_step(&lv, 40000, 40000);
    CHECK(lv.level == SBTH_NORMAL, "cool -> normal, got %d", lv.level);
    /* any probe >= 105 -> critical even with a cool GPU */
    sbth_levels_step(&lv, 50000, 106000);
    sbth_levels_step(&lv, 50000, 106000);
    CHECK(lv.level == SBTH_CRITICAL, "CPU 106 C -> critical, got %d", lv.level);
    CHECK(lv.max_any_mc == 106000 && lv.max_gpu_mc == 95000, "maxima %d %d", lv.max_any_mc, lv.max_gpu_mc);

    /* blind: 3 missing GPU readings change nothing, the 4th -> stop at once */
    sbth_levels_init(&lv);
    for (int i = 0; i < 3; ++i)
        sbth_levels_step(&lv, SBTH_NO_TEMP, 40000);
    CHECK(lv.level == SBTH_NORMAL, "3 blind readings tolerated");
    CHECK(sbth_levels_step(&lv, SBTH_NO_TEMP, 40000) == 1 && lv.level == SBTH_STOP, "4th blind -> stop");
    /* readings come back: falls after 8 */
    for (int i = 0; i < 8; ++i)
        sbth_levels_step(&lv, 40000, 40000);
    CHECK(lv.level == SBTH_NORMAL, "sight back -> normal, got %d", lv.level);

    /* the driver starts at Stop: needs 2 s of cool readings to release the GPU */
    sbth_levels_init(&lv);
    lv.level = SBTH_STOP;
    for (int i = 0; i < 7; ++i)
        sbth_levels_step(&lv, 40000, 40000);
    CHECK(lv.level == SBTH_STOP, "start: still stop after 7");
    sbth_levels_step(&lv, 40000, 40000);
    CHECK(lv.level == SBTH_NORMAL, "start: normal after 8");

    /* IOCTL code = CTL_CODE(0x8253, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS) */
    CHECK(SBTH_IOCTL_GET_STATE == 0x82536004u, "ioctl %#x", SBTH_IOCTL_GET_STATE);
}

/* ---- fake DECON / DMA / DSIM ---------------------------------------------------------------- */

struct fdisp {
    uint32_t decon[0x4000], dma[0x1c00], dsim[0x40];
    char log[8192];
    int latch_after;                      /* shadow request clears after n reads */
    int dsim_stuck;
    unsigned payloads, headers;
    uint8_t sent[64][16];                 /* DCS packets as the panel would see them */
    unsigned sent_len[64], nsent;
    uint8_t cur[16];
    unsigned curn;
};

static uint32_t *freg(struct fdisp *f, unsigned b, unsigned off)
{
    if (b == SBDC_DECON && off < sizeof(f->decon))
        return &f->decon[off / 4];
    if (b == SBDC_DMA && off < sizeof(f->dma))
        return &f->dma[off / 4];
    if (b == SBDC_DSIM && off < sizeof(f->dsim))
        return &f->dsim[off / 4];
    return NULL;
}

static uint32_t fd_rd(void *ctx, unsigned b, unsigned off)
{
    struct fdisp *f = ctx;
    uint32_t *r = freg(f, b, off);
    if (!r || (off & 3)) {
        printf("FAIL: display read out of range %u:%#x\n", b, off);
        ++fails;
        return 0;
    }
    if (b == SBDC_DMA && (off < SBDC_CH0 || off >= SBDC_CH0 + 0x1000)) {
        printf("FAIL: read of an IDMA channel other than 0 (%#x)\n", off);
        ++fails;
    }
    if (b == SBDC_DECON && off == SBDC_SHADOW_REQ && *r && f->latch_after >= 0 && f->latch_after-- == 0)
        *r = 0;
    if (b == SBDC_DSIM && off == SBDC_DSIM_INTSRC && !f->dsim_stuck)
        return *r | SBDC_DSIM_PH_EMPTY;   /* the link drains each packet at once */
    return *r;
}

static void fd_wr(void *ctx, unsigned b, unsigned off, uint32_t v)
{
    struct fdisp *f = ctx;
    uint32_t *r = freg(f, b, off);
    char line[64];
    int ok = (b == SBDC_DECON && (off == SBDC_SHADOW_REQ || off == SBDC_TRIG || off == SBDC_INT_EN || off == SBDC_INT_PEND)) ||
             (b == SBDC_DMA && (off == SBDC_CH0 + SBDC_IDMA_BASE_Y || off == SBDC_CH0 + SBDC_IDMA_BASE_Y + 4)) ||
             (b == SBDC_DSIM && (off == SBDC_DSIM_INTSRC || off == SBDC_DSIM_PKTHDR || off == SBDC_DSIM_PAYLOAD));
    if (!ok || !r) {
        printf("FAIL: write to a forbidden register %u:%#x = %#x\n", b, off, v);
        ++fails;
        return;
    }
    snprintf(line, sizeof(line), "%u:%x=%x ", b, off, v);
    strncat(f->log, line, sizeof(f->log) - strlen(f->log) - 1);
    if (b == SBDC_DSIM && off == SBDC_DSIM_PAYLOAD) {
        for (unsigned k = 0; k < 4 && f->curn < 16; ++k)
            f->cur[f->curn++] = (uint8_t)(v >> (8 * k));
        ++f->payloads;
        return;
    }
    if (b == SBDC_DSIM && off == SBDC_DSIM_PKTHDR) {
        unsigned n = (v >> 8 & 0xff) | (v >> 16 & 0xff) << 8;
        if ((v & 0xff) == 0x39 && f->nsent < 64) {
            memcpy(f->sent[f->nsent], f->cur, n < 16 ? n : 16);
            f->sent_len[f->nsent++] = n;
        }
        f->curn = 0;
        ++f->headers;
        *r = v;
        return;
    }
    if (b == SBDC_DSIM && off == SBDC_DSIM_INTSRC) {
        *r &= ~v;                         /* W1C */
        return;
    }
    if (b == SBDC_DECON && off == SBDC_INT_PEND) {
        *r &= ~v;
        return;
    }
    *r = v;
}

static void fd_init(struct fdisp *f)
{
    memset(f, 0, sizeof(*f));
    f->decon[SBDC_PATH0 / 4] = 1u << 20;   /* window 5 enabled */
    f->decon[SBDC_PATH1 / 4] = 0x500000;
    f->decon[SBDC_WIN_START(5) / 4] = 0;
    f->decon[SBDC_WIN_END(5) / 4] = (2959u << 16) | 1439u;
    f->decon[SBDC_TRIG / 4] = 0x1281;      /* the loader's value: HW trigger enabled, unmasked */
    f->dma[(SBDC_CH0 + SBDC_IDMA_BASE_Y) / 4] = 0xcc000000;
    f->dma[(SBDC_CH0 + SBDC_IDMA_SHADOW + SBDC_IDMA_BASE_Y) / 4] = 0xcc000000;
    f->dma[(SBDC_CH0 + SBDC_IDMA_IMG_SIZE) / 4] = (2960u << 16) | 1440u;
    f->dsim[SBDC_DSIM_CLK_CTRL / 4] = 1u << 20;
    f->dsim[SBDC_DSIM_FIFOCTRL / 4] = 0x00000407;   /* header FIFO empty, idle */
    f->latch_after = 3;
}

static void test_display(void)
{
    struct fdisp f;
    struct sbdc_io io = { &f, fd_rd, fd_wr, NULL };
    struct sbdc d;

    fd_init(&f);
    CHECK(sbdc_identify(&d, &io, 0xcc000000, 1440, 2960) == 0, "identify S-Boot's pipeline");
    CHECK(d.win == 5, "window 5, got %u", d.win);
    CHECK(f.log[0] == 0, "identify writes nothing: %s", f.log);

    /* not as expected -> refuse */
    fd_init(&f);
    f.dma[(SBDC_CH0 + SBDC_IDMA_SHADOW + SBDC_IDMA_BASE_Y) / 4] = 0xdd000000;
    CHECK(sbdc_identify(&d, &io, 0xcc000000, 1440, 2960) == -3, "channel 0 not on the framebuffer");
    fd_init(&f);
    f.decon[SBDC_WIN_END(5) / 4] = (1479u << 16) | 719u;
    CHECK(sbdc_identify(&d, &io, 0xcc000000, 1440, 2960) == -2, "no full-screen window");
    fd_init(&f);
    f.dma[(SBDC_CH0 + SBDC_IDMA_IMG_SIZE) / 4] = (1480u << 16) | 720u;
    CHECK(sbdc_identify(&d, &io, 0xcc000000, 1440, 2960) == -4, "image size differs");

    /* a flip: base (+0x40, +0x44), shadow request for window 5, trigger untouched (already on) */
    fd_init(&f);
    sbdc_identify(&d, &io, 0xcc000000, 1440, 2960);
    CHECK(sbdc_flip(&d, 0x91000000) == 0, "flip");
    CHECK(strcmp(f.log, "1:1040=91000000 1:1044=91000000 0:60=20 ") == 0, "flip writes: %s", f.log);
    CHECK(sbdc_flip_pending(&d), "pending until the TE latch");
    CHECK(sbdc_flip(&d, 0xcc000000) == -1, "no second flip while pending");
    CHECK(sbdc_wait_latched(&d, 1000, 50) == 0, "latches");
    CHECK(sbdc_flip(&d, 0x91000001) == -1, "unaligned refused");
    CHECK(sbdc_flip(&d, 0) == -1, "zero refused");

    /* masked trigger (S-Boot's 0x3070): the flip unmasks it, keeping the other bits */
    fd_init(&f);
    f.decon[SBDC_TRIG / 4] = 0x3070;
    sbdc_identify(&d, &io, 0xcc000000, 1440, 2960);
    sbdc_flip(&d, 0x91000000);
    CHECK(f.decon[SBDC_TRIG / 4] == 0x3061, "trigger 0x3070 -> 0x3061, got %#x", f.decon[SBDC_TRIG / 4]);

    /* a flip that never latches: timeout counted */
    fd_init(&f);
    f.latch_after = -1;
    sbdc_identify(&d, &io, 0xcc000000, 1440, 2960);
    sbdc_flip(&d, 0x91000000);
    CHECK(sbdc_wait_latched(&d, 500, 50) == -1 && d.latch_timeouts == 1, "latch timeout");

    /* interrupts: enable is RMW, ack is W1C of the three known bits only */
    fd_init(&f);
    sbdc_identify(&d, &io, 0xcc000000, 1440, 2960);
    f.decon[SBDC_INT_EN / 4] = 0x100;
    sbdc_frame_irq(&d, 1);
    CHECK(f.decon[SBDC_INT_EN / 4] == 0x1101, "irq enable RMW %#x", f.decon[SBDC_INT_EN / 4]);
    f.decon[SBDC_INT_PEND / 4] = 0x80001000;
    CHECK(sbdc_irq_ack(&d) == 0x1000 && f.decon[SBDC_INT_PEND / 4] == 0x80000000, "ack only known bits");

    /* rectangles */
    {
        struct sbdc_rects s;
        struct sbdc_rect r1 = { -5, -5, 10, 10 }, r2 = { 2, 2, 8, 8 }, r3 = { 1430, 2950, 1500, 3000 };
        sbdc_rects_clear(&s);
        sbdc_rects_add(&s, r1, 1440, 2960);
        sbdc_rects_add(&s, r2, 1440, 2960);
        sbdc_rects_add(&s, r3, 1440, 2960);
        CHECK(s.n == 2 && s.r[0].l == 0 && s.r[0].r == 10 && s.r[1].r == 1440 && s.r[1].b == 2960,
              "clip and containment: n=%u", s.n);
        for (int i = 0; i < 40; ++i) {
            struct sbdc_rect r = { i * 10, i * 10, i * 10 + 5, i * 10 + 5 };
            sbdc_rects_add(&s, r, 1440, 2960);
        }
        CHECK(s.n <= SBDC_MAX_RECTS, "bounded");
        struct sbdc_rect e = { 5, 5, 5, 9 };
        unsigned before = s.n;
        sbdc_rects_add(&s, e, 1440, 2960);
        CHECK(s.n == before, "empty rect ignored");
    }

    /* swap: present 1 -> back = ours (full copy), present 2 -> back = fb (both presents' rects) */
    {
        struct sbdc_swap sw;
        struct sbdc_rect a = { 0, 0, 100, 100 }, b = { 500, 500, 600, 600 };
        sbdc_swap_init(&sw, 0xcc000000, 0x91000000, 1440, 2960);
        sbdc_swap_damage(&sw, &a, 1, 1440, 2960);
        CHECK(sbdc_swap_back(&sw) == 1, "first back = ours");
        CHECK(sbdc_rects_area(&sw.stale[1]) == 1440ull * 2960, "ours needs the full frame");
        sbdc_swap_flipped(&sw);
        CHECK(sw.front == 1 && sw.stale[1].n == 0, "ours is front, clean");
        sbdc_swap_damage(&sw, &b, 1, 1440, 2960);
        CHECK(sbdc_swap_back(&sw) == 0, "second back = framebuffer");
        CHECK(sbdc_rects_area(&sw.stale[0]) == 2 * 100 * 100, "fb needs both presents: %llu",
              sbdc_rects_area(&sw.stale[0]));
        sbdc_swap_flipped(&sw);
        CHECK(sbdc_rects_area(&sw.stale[1]) == 100 * 100, "ours needs only present 2");
    }

    /* brightness */
    CHECK(sbdc_level_to_aor(100) == 0x000c, "100 -> 0x000c");
    CHECK(sbdc_level_to_aor(64) == 0x0440, "64 -> S-Boot normal");
    CHECK(sbdc_level_to_aor(4) == 0x0b7c, "4 -> 0x0b7c");
    CHECK(sbdc_level_to_aor(0) == 0x0ba3, "0 -> 0x0ba3");
    CHECK(sbdc_level_to_aor(200) == 0x000c, "clamped");
    {
        sbdc_u32 last = 0xffff;
        int mono = 1;
        for (unsigned l = 0; l <= 100; ++l) {
            sbdc_u32 a = sbdc_level_to_aor(l);
            if (a > last || a < SBDC_AOR_MIN || a > SBDC_AOR_MAX)
                mono = 0;
            last = a;
        }
        CHECK(mono, "AOR falls monotonically with the level, inside the clamp");
    }

    fd_init(&f);
    sbdc_identify(&d, &io, 0xcc000000, 1440, 2960);
    CHECK(sbdc_set_aor(&d, 0x0440) == 0, "set AOR");
    CHECK(f.nsent == 4, "4 packets, got %u", f.nsent);
    CHECK(f.sent_len[0] == 3 && !memcmp(f.sent[0], "\xf0\x5a\x5a", 3), "key on");
    CHECK(f.sent_len[1] == 3 && !memcmp(f.sent[1], "\xb1\x04\x40", 3), "B1 04 40");
    CHECK(f.sent_len[2] == 2 && !memcmp(f.sent[2], "\xf7\x03", 2), "commit");
    CHECK(f.sent_len[3] == 3 && !memcmp(f.sent[3], "\xf0\xa5\xa5", 3), "key off");
    fd_init(&f);
    sbdc_identify(&d, &io, 0xcc000000, 1440, 2960);
    sbdc_set_aor(&d, 0xffff);
    CHECK(f.nsent == 4 && !memcmp(f.sent[1], "\xb1\x0b\xa3", 3), "AOR clamped to 0x0ba3");
    /* link not in HS / video mode: nothing sent */
    fd_init(&f);
    sbdc_identify(&d, &io, 0xcc000000, 1440, 2960);
    f.dsim[SBDC_DSIM_CLK_CTRL / 4] = 0;
    CHECK(sbdc_set_aor(&d, 0x0440) == -2 && f.nsent == 0, "link down: nothing sent");
    /* a link that never drains: fails, the key is still locked (key off sent), retried once */
    fd_init(&f);
    sbdc_identify(&d, &io, 0xcc000000, 1440, 2960);
    f.dsim_stuck = 1;
    f.dsim[SBDC_DSIM_FIFOCTRL / 4] = 0x00010007;   /* one header queued, never empties */
    CHECK(sbdc_set_aor(&d, 0x0440) != 0, "stuck link fails");
    CHECK(f.nsent == 0, "stuck link: no packet pushed into a full queue (%u)", f.nsent);
}

int main(void)
{
    test_thermal();
    test_display();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
