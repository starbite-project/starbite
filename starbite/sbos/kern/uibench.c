/* SPDX-License-Identifier: MIT
 * 1.2-alpha26 P0 (docs: the UI porting plan, starbite-os-ui-port-plan.md): what the CPU can composite. The new UI
 * moves cached layers (translate / scale / opacity) and presents half-resolution motion frames scaled 2x; this
 * measures the four kernels that costs (fill, copy, premultiplied blend, the 2x upscale) on 1 and 4 A55 at the
 * screen-on 1053 MHz and the top 1794 MHz (the M3s later, through the heater's FlexPMU path), into RAM and into the panel's write-combined
 * framebuffer. Only the uibench self-test variant calls it (uitest.c SBOS_SELFTEST_UIBENCH); read only apart from
 * its own buffers and the framebuffer (the screen shows test patterns). */
#include "sb.h"
#include "kern.h"
#include <arm_neon.h>

#define FW SB_FB_W
#define FH SB_FB_H
#define HW (FW / 2)
#define HH (FH / 2)

enum { K_FILL, K_COPY, K_BLEND, K_UP2X };
static struct {
    volatile uint32_t gen, done[8];
    int kind;
    uint8_t *dst;
    const uint8_t *src;
    uint32_t w, h;                  /* the destination's size in px (K_UP2X: the source is w/2 x h/2) */
    unsigned nslice, slice_of[8];   /* worker core -> its slice index */
} J;

static uint8_t *b_full0, *b_full1, *b_half0, *b_half1;

static void fill_rows(uint8_t *d, uint32_t w, uint32_t y0, uint32_t y1)
{
    uint32x4_t v = vdupq_n_u32(0xff203040U);
    for (uint32_t y = y0; y < y1; ++y) {
        uint32_t *p = (uint32_t *)(d + (size_t)y * w * 4);
        for (uint32_t x = 0; x < w; x += 4)
            vst1q_u32(p + x, v);
    }
}

static void copy_rows(uint8_t *d, const uint8_t *s, uint32_t w, uint32_t y0, uint32_t y1)
{
    for (uint32_t y = y0; y < y1; ++y) {
        const uint8_t *a = s + (size_t)y * w * 4;
        uint8_t *b = d + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w * 4; x += 64) {
            uint8x16x4_t q = vld1q_u8_x4(a + x);
            vst1q_u8_x4(b + x, q);
        }
    }
}

/* premultiplied src over dst: d = s + d * (255 - sa) / 255 */
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

/* a half-resolution frame into the panel's size: each source row makes two (the second the average with the next
 * row), each pixel two (the second the average with its right neighbour); rows y0..y1 of the destination */
static void up2x_rows(uint8_t *d, const uint8_t *s, uint32_t w, uint32_t y0, uint32_t y1)
{
    uint32_t sw = w / 2, sh = FH / 2;
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
            if (y + 1 < y1) {
                vst1q_u32((uint32_t *)(o1 + x * 8), mid.val[0]);
                vst1q_u32((uint32_t *)(o1 + x * 8 + 16), mid.val[1]);
            }
        }
    }
}

static void run_slice(unsigned i)
{
    uint32_t rows = J.h / J.nslice, y0 = i * rows, y1 = i + 1 == J.nslice ? J.h : y0 + rows;
    y0 &= ~1U;
    if (i + 1 != J.nslice)
        y1 &= ~1U;
    switch (J.kind) {
    case K_FILL: fill_rows(J.dst, J.w, y0, y1); break;
    case K_COPY: copy_rows(J.dst, J.src, J.w, y0, y1); break;
    case K_BLEND: blend_rows(J.dst, J.src, J.w, y0, y1); break;
    default: up2x_rows(J.dst, J.src, J.w, y0, y1); break;
    }
}

static void worker(unsigned w, void *arg)
{
    (void)arg;
    uint32_t seen = __atomic_load_n(&J.gen, __ATOMIC_ACQUIRE);
    while (!sb_worker_stop()) {
        uint32_t g = __atomic_load_n(&J.gen, __ATOMIC_ACQUIRE);
        if (g == seen)
            continue;
        seen = g;
        run_slice(J.slice_of[w]);
        __atomic_store_n(&J.done[w], g, __ATOMIC_RELEASE);
    }
}

static unsigned nw, wcore[8];       /* the worker cores in use (besides the boot core unless only_worker) */
static int only_worker;             /* the M3 run: the boot core waits, the worker does it all */

/* one run of a kernel over the whole destination, split over the boot core and the workers: ms */
static uint64_t run_us(int kind, uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t h)
{
    J.kind = kind;
    J.dst = dst;
    J.src = src;
    J.w = w;
    J.h = h;
    J.nslice = nw + (only_worker ? 0 : 1);
    for (unsigned i = 0; i < nw; ++i)
        J.slice_of[wcore[i]] = i + (only_worker ? 0 : 1);
    uint64_t t0 = sb_ticks();
    uint32_t g = __atomic_add_fetch(&J.gen, 1, __ATOMIC_RELEASE);
    if (!only_worker)
        run_slice(0);
    for (unsigned i = 0; i < nw; ++i)
        while (__atomic_load_n(&J.done[wcore[i]], __ATOMIC_ACQUIRE) != g)
            ;
    uint64_t t1 = sb_ticks();
    return (t1 - t0) * 1000000U / sb_freq();
}

static uint64_t best_us(int kind, uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t h)
{
    uint64_t b = ~0ULL;
    for (int r = 0; r < 3; ++r) {
        hw_wdt_feed();
        uint64_t t = run_us(kind, dst, src, w, h);
        if (t < b)
            b = t;
    }
    return b;
}

static void line(const char *what, uint64_t us, uint64_t bytes)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "uibench: ");
    ub_s(&u, what);
    ub_s(&u, ": ");
    ub_dec(&u, us / 1000U);
    ub_c(&u, '.');
    ub_dec(&u, us % 1000U / 100U);
    ub_s(&u, " ms, ");
    ub_dec(&u, us ? bytes / us : 0);
    ub_s(&u, " MB/s");
    sb_log(u.b);
}

int uibench_setup(void)
{
    b_full0 = sb_alloc((uint64_t)FW * FH * 4);
    b_full1 = sb_alloc((uint64_t)FW * FH * 4);
    b_half0 = sb_alloc((uint64_t)HW * HH * 4);
    b_half1 = sb_alloc((uint64_t)HW * HH * 4);
    if (!b_full0 || !b_full1 || !b_half0 || !b_half1) {
        sb_log("uibench: not enough memory for the buffers (43 MB)");
        return 0;
    }
    for (uint64_t i = 0; i < (uint64_t)HW * HH; ++i) {   /* a translucent layer: premultiplied, alpha .5 .. 1 */
        uint32_t a = 128U + (uint32_t)(i % 128U);
        ((uint32_t *)b_half1)[i] = a << 24 | (a / 2) << 16 | (a / 3) << 8 | (a / 4);
    }
    for (uint64_t i = 0; i < (uint64_t)FW * FH; ++i)
        ((uint32_t *)b_full1)[i] = 0xff000000U | (uint32_t)(i * 2654435761U >> 8);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "uibench: buffers ready; A55 at ");
    ub_dec(&u, hw_a55_khz() / 1000U);
    ub_s(&u, " MHz; free RAM ");
    ub_size(&u, sb_arena_free());
    sb_log(u.b);
    return 1;
}

/* one set: the label says which cores and clock */
void uibench_set(const char *label, unsigned ncores, const unsigned *cores, int worker_only)
{
    nw = 0;
    only_worker = worker_only;
    for (unsigned i = 0; i < ncores; ++i) {
        int64_t r = sb_smp_start(cores[i], worker, 0);
        if (r) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "uibench: core ");
            ub_dec(&u, cores[i]);
            ub_s(&u, " did not start: ");
            ub_sdec(&u, r);
            sb_log(u.b);
            continue;
        }
        wcore[nw++] = cores[i];
    }
    if (worker_only && !nw) {
        sb_log("uibench: no worker core: this set is skipped");
        return;
    }
    sb_logf("uibench: set: ", label);
    uint64_t full = (uint64_t)FW * FH * 4, half = (uint64_t)HW * HH * 4;
    uint64_t f = best_us(K_FILL, b_full0, 0, FW, FH);
    line("fill, a full frame in RAM", f, full);
    uint64_t c = best_us(K_COPY, b_full0, b_full1, FW, FH);
    line("copy, a full frame RAM to RAM", c, full * 2);
    uint64_t bh = best_us(K_BLEND, b_half0, b_half1, HW, HH);
    line("blend, a half-resolution layer over another", bh, half * 3);
    uint64_t bf = best_us(K_BLEND, b_full0, b_full1, FW, FH);
    line("blend, a full-resolution layer over another", bf, full * 3);
    uint64_t ur = best_us(K_UP2X, b_full0, b_half0, FW, FH);
    line("2x upscale, half frame to a full frame in RAM", ur, full + half);
    uint64_t ff = best_us(K_FILL, (uint8_t *)SB_FB, 0, FW, FH);
    line("fill, the panel's framebuffer (write-combined)", ff, full);
    uint64_t cf = best_us(K_COPY, (uint8_t *)SB_FB, b_full1, FW, FH);
    line("copy, RAM to the panel's framebuffer", cf, full * 2);
    uint64_t uf = best_us(K_UP2X, (uint8_t *)SB_FB, b_half0, FW, FH);
    line("2x upscale, half frame into the panel's framebuffer", uf, full + half);
    /* the plan's motion frame: three half-resolution layers blended, then the 2x present into the panel */
    uint64_t frame = 3 * bh + uf;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "uibench: a motion frame (3 half-res layers + the 2x present): ");
    ub_dec(&u, frame / 1000U);
    ub_c(&u, '.');
    ub_dec(&u, frame % 1000U / 100U);
    ub_s(&u, frame <= 16700U ? " ms: fits 60 fps" : frame <= 33300U ? " ms: 30 fps, not 60" : " ms: under 30 fps");
    sb_log(u.b);
    ub_init(&u);
    ub_s(&u, "uibench: a full-resolution rest frame (3 layers + the copy into the panel): ");
    ub_dec(&u, (3 * bf + cf) / 1000U);
    ub_s(&u, " ms");
    sb_log(u.b);
    unsigned left = sb_smp_stop(500);
    if (left)
        sb_logx("uibench: workers that did not stop: ", left);
}
