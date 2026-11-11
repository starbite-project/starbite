/* SPDX-License-Identifier: MIT
 * Starbite OS's GPU compositor (P4 of the UI port plan; starbite-os-ui-port-plan.md): the Mali-G72 composites the
 * UI's cached layers into the scanout with compute jobs (the job kind Survival's GPU heat already runs on this phone:
 * no tiler, no fragment jobs, no texture units). The shaders are Mesa's Bifrost binaries from
 * starbite/tools/mesa-compjob.c (gpucomp.h): FILL, COMP (textured quads: scale, alpha, a rounded clip, bilinear,
 * src-over), DOWN and UP (the dual filter's blur passes). Parameters live in fixed blocks of the GPU's data window.
 *
 * Normal clocks only: the bring-up never writes vdd_g3d (Survival's 950 mV is the heater's); the GPU runs at the
 * clock S-Boot left, or at a table step ACPM sets with its own voltage. Its safety checks are gpuheat's (the domain
 * on, a G72, PLL_G3D running, the GPU zone's hardware trip armed).
 *
 * gpucomp_selftest_step drives the silent test variant (SBOS_SELFTEST_GPUCOMP): fill, composite, blur, a 60-frame
 * motion run, each checked against the CPU's pixels and timed, then the GPU off and a restart into recovery. */
#include "sb.h"
#include "kern.h"
#include "gpucomp.h"

#define VA(o) (GPUCOMP_VA_BASE + (uint64_t)(o))
#define CPU(o) ((volatile uint32_t *)(uintptr_t)(SB_GPU_DATA + (uint64_t)(o)))
#define SCAN_B 0xcd200000UL                 /* the second scanout buffer (Starbite OS Next's flips) */
#define VA_SCAN_B 0x06000000ULL
#define VA_POOL 0x08000000ULL
#define JS 1U                               /* the compute slot, as Panfrost and gpuheat */

static struct sb_auth au;
static uint32_t cores;
static int up, fails;
static unsigned mhz_boot;

static void put(uint64_t off, const uint32_t *w, unsigned n)
{
    volatile uint32_t *d = CPU(off);
    for (unsigned i = 0; i < n; ++i)
        d[i] = w[i];
}

static void put64(volatile uint32_t *d, uint64_t v)
{
    d[0] = (uint32_t)v;
    d[1] = (uint32_t)(v >> 32);
}

static uint32_t fbits(float f)
{
    union { float f; uint32_t u; } x = { f };
    return x.u;
}

static void log_us(const char *what, uint64_t us)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "gpucomp: ");
    ub_s(&u, what);
    ub_s(&u, ": ");
    ub_dec(&u, us / 1000U);
    ub_c(&u, '.');
    ub_dec2(&u, (unsigned)(us % 1000U / 10U));
    ub_s(&u, " ms");
    sb_log(u.b);
}

static void fail(const char *why)
{
    ++fails;
    sb_logf("gpucomp: FAILED: ", why);
}

/* the GPU up at its normal clock; 1 ready */
static int gc_up(void)
{
    if (!sb_gpu_domain()) {
        fail("the G3D power domain is off");
        return 0;
    }
    uint32_t id = 0;
    if (!sb_probe32(SB_GPU, &id) || (id >> 16) != 0x6221U) {
        fail("GPU_ID is not a Mali-G72");
        return 0;
    }
    int unl = 0, off = 0;
    uint64_t hz = sb_gpu_hz(&off);
    (void)sb_pll_hz(0x17400140UL, &unl);
    if (off || !hz || unl) {
        fail("PLL_G3D is not running");
        return 0;
    }
    struct sb_temps t;
    sb_temps_read(&t);
    if (t.sub != 1 || !(t.trip_armed & 2)) {
        fail("the GPU zone's hardware heat trip (TMU_SUB) is not armed");
        return 0;
    }
    /* the session's gate is the heater's authorisation (it names vdd_g3d); this path never writes it */
    sb_auth_init(&au, SB_SP_PMIC, 0, 1U << 8 | 0x2bU, 1, SB_TIER1, 0, "gpucomp: the GPU at its normal clock (no voltage write)");
    sb_auth_step(&au, SB_STEP_WARN);
    sb_auth_step(&au, SB_STEP_HELD);
    if (!sb_gpu_open(&au)) {
        fail("the GPU session was refused");
        return 0;
    }
    sb_gpu_tables();
    if (!sb_gpu_map(VA_SCAN_B, SCAN_B, (uint64_t)SB_FB_W * SB_FB_H * 4U) ||
        !sb_gpu_map(VA_POOL, SB_GPU_POOL, SB_GPU_POOL_SIZE)) {
        sb_gpu_close();
        fail("the GPU's extra mappings (the second scanout buffer, the pool) did not fit");
        return 0;
    }
    cores = sb_gpu_rd(0x100);
    if (!cores)
        cores = 0x5771777U;
    unsigned us[4];
    if (!sb_gpu_up(cores, us)) {
        sb_gpu_power_off();
        sb_gpu_close();
        fail("the GPU did not power up");
        return 0;
    }
    put(GPUCOMP_O_SH_fill, gpucomp_sh_fill, sizeof gpucomp_sh_fill / 4U);
    put(GPUCOMP_O_SH_comp, gpucomp_sh_comp, sizeof gpucomp_sh_comp / 4U);
    put(GPUCOMP_O_SH_down, gpucomp_sh_down, sizeof gpucomp_sh_down / 4U);
    put(GPUCOMP_O_SH_up, gpucomp_sh_up, sizeof gpucomp_sh_up / 4U);
    put(GPUCOMP_O_RSD_fill, gpucomp_rsd_fill, sizeof gpucomp_rsd_fill / 4U);
    put(GPUCOMP_O_RSD_comp, gpucomp_rsd_comp, sizeof gpucomp_rsd_comp / 4U);
    put(GPUCOMP_O_RSD_down, gpucomp_rsd_down, sizeof gpucomp_rsd_down / 4U);
    put(GPUCOMP_O_RSD_up, gpucomp_rsd_up, sizeof gpucomp_rsd_up / 4U);
    put(GPUCOMP_O_TLS, gpucomp_tls, sizeof gpucomp_tls / 4U);
    mhz_boot = (unsigned)(hz / 1000000U);
    up = 1;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "gpucomp: GPU up at ");
    ub_dec(&u, mhz_boot);
    ub_s(&u, " MHz (the boot clock, no voltage write), cores ");
    ub_hex(&u, cores, 8);
    ub_s(&u, ", reset+power ");
    ub_dec(&u, us[0] + us[1] + us[2] + us[3]);
    ub_s(&u, " us");
    sb_log(u.b);
    return 1;
}

static void gc_down(const char *why)
{
    if (!up)
        return;
    sb_gpu_power_off();
    sb_gpu_close();
    up = 0;
    sb_logf("gpucomp: GPU off: ", why);
}

/* one job (its descriptor written again: the GPU writes its status into the header); GPU us, or 0 failed */
static uint64_t run(uint64_t job_off, const uint32_t *job, const char *what)
{
    put(job_off, job, GPUCOMP_JOB_BYTES / 4U);
    struct sb_gpu_result r;
    hw_wdt_feed();
    if (!sb_gpu_run(JS, VA(job_off), cores, 1000000U, &r)) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "gpucomp: FAILED: ");
        ub_s(&u, what);
        ub_s(&u, r.timeout ? ": no end in 1 s" : ": a fault, ");
        if (!r.timeout) {
            ub_s(&u, sb_gpu_exc_name(r.js_status & 0xffU));
            ub_s(&u, " js ");
            ub_hex(&u, r.js_status, 8);
            ub_s(&u, " mmu ");
            ub_hex(&u, r.as_fault, 8);
            ub_s(&u, " at ");
            ub_hex(&u, r.as_addr, 10);
        }
        sb_log(u.b);
        ++fails;
        return 0;
    }
    return r.us ? r.us : 1;
}

/* ---- parameters ------------------------------------------------------------------------------------------- */
static void par_fill(uint64_t dst_va, uint32_t stride, uint32_t w, uint32_t h, uint32_t argb)
{
    volatile uint32_t *p = CPU(GPUCOMP_O_PAR_fill);
    put64(p, dst_va);
    p[2] = stride;
    p[3] = w;
    p[4] = h;
    p[5] = argb;
}

struct quad {
    uint64_t src;
    uint32_t stride, w, h, flags, solid;
    float alpha, dx0, dy0, isx, isy, cx0, cy0, cx1, cy1, radius;
};
static struct quad Q[64];
static unsigned nq;

static void par_comp(uint64_t dst_va, uint32_t bg)
{
    volatile uint32_t *p = CPU(GPUCOMP_O_PAR_comp);
    put64(p, dst_va);
    p[2] = SB_FB_W;
    p[3] = SB_FB_W;
    p[4] = SB_FB_H;
    p[5] = nq;
    p[6] = bg;
    for (unsigned i = 0; i < nq; ++i) {
        volatile uint32_t *q = p + 16 + i * 20;
        const struct quad *s = &Q[i];
        put64(q, s->src);
        q[2] = s->stride;
        q[3] = s->w;
        q[4] = s->h;
        q[5] = s->flags;
        q[6] = s->solid;
        q[7] = fbits(s->alpha);
        q[8] = fbits(s->dx0);
        q[9] = fbits(s->dy0);
        q[10] = fbits(s->isx);
        q[11] = fbits(s->isy);
        q[12] = fbits(s->cx0);
        q[13] = fbits(s->cy0);
        q[14] = fbits(s->cx1);
        q[15] = fbits(s->cy1);
        q[16] = fbits(s->radius);
    }
}

static void par_blur(uint32_t par_off, uint64_t src, uint32_t sw, uint32_t sh, uint64_t dst, uint32_t dw, uint32_t dh)
{
    volatile uint32_t *p = CPU(par_off);
    put64(p, src);
    p[2] = sw;
    p[3] = sw;
    p[4] = sh;
    put64(p + 6, dst);
    p[8] = dw;
    p[9] = dw;
    p[10] = dh;
}

/* ---- the test's textures, in the GPU pool (CPU-written, non-cacheable) ------------------------------------- */
#define POOL_CPU(va) ((uint32_t *)(uintptr_t)(SB_GPU_POOL + ((va) - VA_POOL)))
#define TEX_BG (VA_POOL)                                    /* 720 x 1480 opaque gradient: the wallpaper */
#define TEX_CARD (VA_POOL + 0x420000ULL)                    /* 600 x 300 translucent premultiplied: a card */
#define BUF_HALF (VA_POOL + 0x520000ULL)                    /* 720 x 1480: the blur's half */
#define BUF_QUARTER (VA_POOL + 0x940000ULL)                 /* 360 x 740 */

static uint32_t bg_px(unsigned x, unsigned y)
{
    uint32_t r = 8U + y * 40U / 1480U, g = 17U + y * 50U / 1480U, b = 49U + x * 120U / 720U;
    return 0xff000000U | r << 16 | g << 8 | b;
}

static uint32_t card_px(unsigned x, unsigned y)
{
    uint32_t a = 150U + (x + y) % 100U;                     /* alpha .59 .. .98, premultiplied channels */
    return a << 24 | (a * 70U / 255U) << 16 | (a * 110U / 255U) << 8 | (a * 200U / 255U);
}

static void make_textures(void)
{
    uint32_t *bg = POOL_CPU(TEX_BG), *cd = POOL_CPU(TEX_CARD);
    for (unsigned y = 0; y < 1480U; ++y)
        for (unsigned x = 0; x < 720U; ++x)
            bg[y * 720U + x] = bg_px(x, y);
    for (unsigned y = 0; y < 300U; ++y)
        for (unsigned x = 0; x < 600U; ++x)
            cd[y * 600U + x] = card_px(x, y);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* the scene: the wallpaper (bilinear 2x), n cards (a rounded clip of 60 px, alpha .92) at dy, a solid quad */
static void scene(unsigned ncards, float dy)
{
    nq = 0;
    Q[nq++] = (struct quad){ TEX_BG, 720, 720, 1480, 2, 0, 1.0f, 0, 0, .5f, .5f, 0, 0, 1440, 2960, 0 };
    for (unsigned i = 0; i < ncards && nq < 62; ++i) {
        float x = 120.0f + (float)(i % 2) * 40.0f, y = 600.0f + (float)i * 260.0f + dy;
        Q[nq++] = (struct quad){ TEX_CARD, 600, 600, 300, 0, 0, .92f, x, y, 1, 1, x, y, x + 600.0f * 2.0f, y + 300.0f, 60 };
        Q[nq - 1].isx = .5f;               /* drawn 1200 px wide: the card stretched x2 horizontally (bilinear) */
        Q[nq - 1].flags = 2;
    }
    Q[nq++] = (struct quad){ 0, 0, 0, 0, 1, 0xccffffffU, 1.0f, 0, 0, 1, 1, 520, 2880, 920, 2898, 9 };   /* the home bar */
}

/* the CPU's own result for pixel (x, y) of the scene (the same maths, float), to check the GPU */
static void unpack_f(uint32_t p, float c[4])
{
    c[0] = (float)((p >> 16) & 255U) / 255.0f;
    c[1] = (float)((p >> 8) & 255U) / 255.0f;
    c[2] = (float)(p & 255U) / 255.0f;
    c[3] = (float)(p >> 24) / 255.0f;
}

static float fl(float v) { return (float)(int)(v < 0 ? v - 0.999999f : v); }

static void sample(const struct quad *q, float u, float v, float out[4])
{
    const uint32_t *t = POOL_CPU(q->src);
    if (!(q->flags & 2)) {
        unpack_f(t[(unsigned)v * q->stride + (unsigned)u], out);
        return;
    }
    float uu = u - .5f, vv = v - .5f, u0 = fl(uu), v0 = fl(vv), tu = uu - u0, tv = vv - v0, c[4][4];
    int iu = (int)u0, iv = (int)v0;
    for (int k = 0; k < 4; ++k) {
        int x = iu + (k & 1), y = iv + (k >> 1);
        x = x < 0 ? 0 : x >= (int)q->w ? (int)q->w - 1 : x;
        y = y < 0 ? 0 : y >= (int)q->h ? (int)q->h - 1 : y;
        unpack_f(t[(unsigned)y * q->stride + (unsigned)x], c[k]);
    }
    for (int i = 0; i < 4; ++i) {
        float top = c[0][i] + (c[1][i] - c[0][i]) * tu, bot = c[2][i] + (c[3][i] - c[2][i]) * tu;
        out[i] = top + (bot - top) * tv;
    }
}

static float fsqrt(float v)
{
    float r = v > 1 ? v : 1;
    for (int i = 0; i < 20; ++i)
        r = .5f * (r + v / r);
    return r;
}

static uint32_t cpu_px(unsigned x, unsigned y, uint32_t bg)
{
    float acc[4], fx = (float)x + .5f, fy = (float)y + .5f;
    unpack_f(bg, acc);
    for (unsigned i = 0; i < nq; ++i) {
        const struct quad *q = &Q[i];
        if (fx < q->cx0 || fx >= q->cx1 || fy < q->cy0 || fy >= q->cy1)
            continue;
        float cov = 1;
        if (q->radius >= .01f) {
            float hx = (q->cx1 - q->cx0) * .5f, hy = (q->cy1 - q->cy0) * .5f;
            float px = fx - (q->cx0 + hx), py = fy - (q->cy0 + hy);
            px = px < 0 ? -px : px;
            py = py < 0 ? -py : py;
            float qx = px - (hx - q->radius), qy = py - (hy - q->radius);
            float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0, mm = qx > qy ? qx : qy;
            float d = fsqrt(mx * mx + my * my) + (mm < 0 ? mm : 0) - q->radius;
            cov = .5f - d;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
        }
        float s[4];
        if (q->flags & 1) {
            unpack_f(q->solid, s);
        } else {
            float u = (fx - q->dx0) * q->isx, v = (fy - q->dy0) * q->isy;
            if (u < 0 || v < 0 || u >= (float)q->w || v >= (float)q->h)
                continue;
            sample(q, u, v, s);
        }
        float a = q->alpha * cov, k = 1 - s[3] * a;
        for (int c = 0; c < 4; ++c)
            acc[c] = s[c] * a + acc[c] * k;
    }
    uint32_t o = 0;
    for (int c = 0; c < 4; ++c) {
        float v = acc[c] < 0 ? 0 : acc[c] > 1 ? 1 : acc[c];
        uint32_t u = (uint32_t)(v * 255.0f + .5f);
        o |= u << (c == 3 ? 24 : c == 0 ? 16 : c == 1 ? 8 : 0);
    }
    return o;
}

static int near(uint32_t a, uint32_t b, unsigned tol)
{
    for (int s = 0; s < 32; s += 8) {
        int d = (int)((a >> s) & 255U) - (int)((b >> s) & 255U);
        if (d > (int)tol || d < -(int)tol)
            return 0;
    }
    return 1;
}

/* check the scanout at a few points against the CPU; the number of mismatches */
static unsigned check_scene(const volatile uint32_t *fb, uint32_t bg, const char *what)
{
    static const unsigned pts[][2] = { { 10, 10 }, { 700, 300 }, { 180, 640 }, { 125, 605 }, { 1300, 700 }, { 719, 1480 },
                                        { 400, 1150 }, { 720, 2889 }, { 523, 2881 }, { 1439, 2959 }, { 181, 601 }, { 60, 2000 } };
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; ++i) {
        uint32_t want = cpu_px(pts[i][0], pts[i][1], bg), got = fb[pts[i][1] * SB_FB_W + pts[i][0]];
        if (!near(want, got, 3)) {
            if (bad < 4) {
                struct ub u;
                ub_init(&u);
                ub_s(&u, "gpucomp: ");
                ub_s(&u, what);
                ub_s(&u, ": pixel ");
                ub_dec(&u, pts[i][0]);
                ub_c(&u, ',');
                ub_dec(&u, pts[i][1]);
                ub_s(&u, " GPU ");
                ub_hex(&u, got, 8);
                ub_s(&u, " CPU ");
                ub_hex(&u, want, 8);
                sb_log(u.b);
            }
            ++bad;
        }
    }
    return bad;
}

/* ---- the self test's steps ---------------------------------------------------------------------------------- */
static int st;
static uint64_t frame_sum, frame_max;

static unsigned gpu_mhz(void)
{
    int off = 0;
    uint64_t hz = sb_gpu_hz(&off);
    return off ? 0 : (unsigned)(hz / 1000000U);
}

int gpucomp_selftest_step(void)
{
    const uint32_t bg = 0xff081131U;
    volatile uint32_t *fb = (volatile uint32_t *)SB_FB;
    switch (st) {
    case 0:
        sb_log("gpucomp: start: the GPU compositor's test (compute jobs: fill, composite, blur; normal clocks)");
        if (!gc_up()) {
            st = 9;
            return 0;
        }
        make_textures();
        st = 1;
        return 0;
    case 1: {
        par_fill(SB_GPU_PANEL_VA, SB_FB_W, SB_FB_W, SB_FB_H, 0xff203a6cU);
        uint64_t us = run(GPUCOMP_O_JOB_fill_full, gpucomp_job_fill_full, "fill");
        if (us) {
            log_us("fill, the full scanout (1440 x 2960)", us);
            unsigned bad = 0;
            for (unsigned i = 0; i < 64; ++i)
                bad += fb[(i * 46U % SB_FB_H) * SB_FB_W + i * 22U % SB_FB_W] != 0xff203a6cU;
            if (bad) {
                sb_logx("gpucomp: FAILED: fill: pixels not the colour, of 64: ", bad);
                ++fails;
            } else {
                sb_log("gpucomp: fill: the scanout holds the colour (64 points)");
            }
        }
        st = us ? 2 : 8;
        return 0;
    }
    case 2: {
        scene(6, 0);
        par_comp(SB_GPU_PANEL_VA, bg);
        uint64_t us = run(GPUCOMP_O_JOB_comp_full, gpucomp_job_comp_full, "composite");
        if (us) {
            log_us("composite, the wallpaper (bilinear 2x) + 6 rounded cards + the home bar, full scanout", us);
            unsigned bad = check_scene(fb, bg, "composite");
            if (bad) {
                sb_logx("gpucomp: FAILED: composite: points off by more than 3 levels, of 12: ", bad);
                ++fails;
            } else {
                sb_log("gpucomp: composite: 12 points within 3 levels of the CPU's");
            }
        }
        st = us ? 3 : 8;
        return 0;
    }
    case 3: {
        /* the blur (dual filter, two levels) of what the scanout shows, back into the scanout */
        uint64_t t = 0, us;
        par_blur(GPUCOMP_O_PAR_down, SB_GPU_PANEL_VA, SB_FB_W, SB_FB_H, BUF_HALF, 720, 1480);
        us = run(GPUCOMP_O_JOB_down_half, gpucomp_job_down_half, "blur down 1");
        t += us;
        if (us) {
            par_blur(GPUCOMP_O_PAR_down, BUF_HALF, 720, 1480, BUF_QUARTER, 360, 740);
            us = run(GPUCOMP_O_JOB_down_quarter, gpucomp_job_down_quarter, "blur down 2");
            t += us;
        }
        if (us) {
            par_blur(GPUCOMP_O_PAR_up, BUF_QUARTER, 360, 740, BUF_HALF, 720, 1480);
            us = run(GPUCOMP_O_JOB_up_half, gpucomp_job_up_half, "blur up 1");
            t += us;
        }
        if (us) {
            par_blur(GPUCOMP_O_PAR_up, BUF_HALF, 720, 1480, SB_GPU_PANEL_VA, SB_FB_W, SB_FB_H);
            us = run(GPUCOMP_O_JOB_up_full, gpucomp_job_up_full, "blur up 2");
            t += us;
        }
        if (us) {
            log_us("blur, the full scanout (dual filter: 2 down + 2 up passes)", t);
            /* in the wallpaper's flat middle the blur keeps the colour (within the gradient's change over 16 px) */
            uint32_t got = fb[2400U * SB_FB_W + 30U], want = cpu_px(30, 2400, bg);
            if (!near(got, want, 6)) {
                struct ub u;
                ub_init(&u);
                ub_s(&u, "gpucomp: FAILED: blur: a flat area changed: GPU ");
                ub_hex(&u, got, 8);
                ub_s(&u, " CPU (unblurred) ");
                ub_hex(&u, want, 8);
                sb_log(u.b);
                ++fails;
            } else {
                sb_log("gpucomp: blur: a flat area keeps its colour");
            }
        }
        st = us ? 4 : 8;
        frame_sum = frame_max = 0;
        return 0;
    }
    case 4:
    case 5: {
        /* 60 motion frames: the cards slide up as an unlock does (16 cards: 18 quads), into the second buffer
         * (VA_SCAN_B: not on screen) and the scanout in turn */
        unsigned frames = 0;
        for (unsigned f = 0; f < 60; ++f) {
            scene(16, -(float)f * 25.0f);
            par_comp(f & 1 ? VA_SCAN_B : SB_GPU_PANEL_VA, bg);
            uint64_t us = run(GPUCOMP_O_JOB_comp_full, gpucomp_job_comp_full, "a motion frame");
            if (!us)
                break;
            frame_sum += us;
            if (us > frame_max)
                frame_max = us;
            ++frames;
        }
        if (frames == 60) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "gpucomp: 60 motion frames (18 quads, full resolution) at ");
            ub_dec(&u, gpu_mhz());
            ub_s(&u, " MHz: mean ");
            ub_dec(&u, frame_sum / 60U / 1000U);
            ub_c(&u, '.');
            ub_dec2(&u, (unsigned)(frame_sum / 60U % 1000U / 10U));
            ub_s(&u, " ms, worst ");
            ub_dec(&u, frame_max / 1000U);
            ub_c(&u, '.');
            ub_dec2(&u, (unsigned)(frame_max % 1000U / 10U));
            ub_s(&u, frame_max <= 16700U ? " ms: fits 60 fps" : " ms: over 16.7 ms");
            sb_log(u.b);
        }
        if (frames != 60) {
            st = 8;
            return 0;
        }
        if (st == 4) {
            /* the same at 572 MHz: a table step ACPM sets with its own voltage (no write of ours) */
            frame_sum = frame_max = 0;
            if (hw_dvfs(DVFS_GPU, 572000U)) {
                sb_mdelay(2);
                st = 5;
            } else {
                sb_log("gpucomp: 572 MHz not taken (ACPM): that run is skipped");
                st = 6;
            }
            return 0;
        }
        st = 6;
        return 0;
    }
    case 6:
        if (mhz_boot && gpu_mhz() != mhz_boot)
            (void)hw_dvfs(DVFS_GPU, mhz_boot * 1000U);
        st = 8;
        return 0;
    case 8:
        gc_down("the test is over");
        sb_log(fails ? "selftest: FAILED: gpucomp (the GPU compositor's test): see the gpucomp lines" :
                       "selftest: passed: gpucomp (fill, composite, blur, 120 motion frames: the GPU's pixels match the CPU's)");
        st = 10;
        return 1;
    case 9:
        sb_log("selftest: FAILED: gpucomp: the GPU did not come up (nothing else run)");
        st = 10;
        return 1;
    default:
        return 1;
    }
}
