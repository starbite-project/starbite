/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: media playback through Calliope (abox_media.h has the what, the wiring and the sources).
 *
 * The stream (cal_play_start), in Samsung's order (abox_rdma.c / abox_if.c / abox.c, as One UI ran it on this phone):
 *   1  the mixer controls: SPUS chain 0 = IN RDMA, ASRC off (abox_try_to_asrc_off), OUT SIFS0 (SPUS_CTRL0 [3:0] 0x2);
 *      UAIFn SPK = SIFS0 (ROUTE_CTRL0 [4n+3:4n] 1); AUDIF 73.728 MHz, UAIFn BCLK 1.536 MHz (abox_register_bclk_usage);
 *      UAIFn I2S master 48 kHz 2 x 16 (CTRL1 0x0984F000; abox_uaif_set_fmt / hw_params);
 *   2  the ring zeroed (PLATFORM_NORMAL prepare clears the buffer);
 *   3  IPC_PCMPLAYBACK channel 0: OPEN, SET_BUFFER (IOVA 0x91000000, period bytes, periods), HW_PARAMS (48000, 16, 2),
 *      PREPARE (abox_rdma_open / hw_params / prepare);
 *   4  MIXP flush (abox_flush_mixp, SIFS0 PRE_PMU), DRAM on (abox_request_dram_on: SYSPOWER_CTRL bit 0), UAIFn SPK_EN
 *      (abox_uaif_trigger, first for playback), TRIGGER 1 (abox_rdma_trigger).
 * Stop (cal_play_stop): TRIGGER 0, wait for RDMA0 CTRL0 ENABLE to clear (40 ms, abox_rdma_disable_barrier), 1 ms
 * for the UAIF FIFO, UAIFn SPK_EN off, DRAM off, HW_FREE, CLOSE, the routes and the UAIF as found, MIXP flush (POST_PMD).
 * While a call has ABOX (abox_state() CALL) the call's settings are left alone: UAIF1, its route and DRAM on.
 *
 * Writes only: ABOX SFR's SYSPOWER_CTRL, ROUTE_CTRL0, SPUS_CTRL0 / CTRL2, UAIF0/1 CTRL0 / CTRL1; CMU_AUD's AUDIF and
 * UAIF0/1 dividers; the PMU's AUD_CONF (only to switch the domain back on when the probe found it on); abox_rmem's ring
 * window. Reads only registers Samsung's driver or our earlier phone runs read (RDMA0 CTRL0 / BUF_STR / STATUS, UAIF
 * STATUS, the PMU AUD / CPU status, IP_INDEX, SysMMU CTRL). Everything else goes through abox.c's own calls. */
#include "abox_media.h"
#include "drvprobe.h"

#ifdef SBOS_KERNEL
#include "sb.h"
#include "amp.h"
#include "sb/bcmw_sb.h"
#else
void *memset(void *d, int c, size_t n);
#endif

#define SFR_SYSPOWER_CTRL  0x0010U
#define SFR_IP_INDEX       0x0000U
#define SFR_ROUTE_CTRL0    0x0040U
#define SFR_SPUS_CTRL0     0x0200U
#define SFR_SPUS_CTRL2     0x0208U
#define SFR_UAIF_CTRL0(n)  (0x0500U + 0x10U * (unsigned)(n))
#define SFR_UAIF_CTRL1(n)  (0x0504U + 0x10U * (unsigned)(n))
#define SFR_UAIF_STATUS(n) (0x050CU + 0x10U * (unsigned)(n))
#define SFR_RDMA0          0x1000U
#define RDMA_CTRL0         0x00U
#define RDMA_BUF_STR       0x08U
#define RDMA_STATUS        0x30U
#define RDMA_ENABLE        (1U << 0)
#define RDMA_PROGRESS      (1U << 31)
#define UAIF_SPK_EN        (1U << 0)
#define UAIF_MASTER        (1U << 2)
#define UAIF_IRQ_MODE      (1U << 3)
#define UAIF_DATA_MODE     (1U << 4)
#define UAIF_CTRL1_MASK    0x1FFFFFFFU
#define UAIF_CTRL1_VAL     0x0984F000U
#define CHAIN0_MASK        0xFU
#define CHAIN0_SIFS0       0x2U             /* IN 0 (RDMA), OUT 1 (SIFS0) at [2:1], ASRC 0 */
#define CMU_DIV_AUDIF      0x1800U
#define CMU_DIV_UAIF(n)    (0x1824U + 4U * (unsigned)(n))
#define DIV_BUSY           (1U << 16)
#define DIV_AUDIF_UAIF     15U              /* 1179.648 / 16 = 73.728 MHz */
#define DIV_UAIF_BCLK      47U              /* 73.728 / 48 = 1.536 MHz */
#define MMU_CTRL           0x000U
#define STOP_US            40000U
#define FLUSH_US           1000U

static struct abox_hal H;
static int have_hal;
static struct cal_probe_ops O;
static int ops_set;

static struct {
    int playing, opened, triggered, uaif, call;
    uint32_t period, periods, ring;
    uint8_t *win;
    uint32_t spus0, route0, uc0, uc1, sysp;
} E;
static char why_buf[160] = "off";

/* ---- text: "cal: " + %s %u %x ------------------------------------------------------------------------------- */
struct tb {
    char b[200];
    unsigned n;
};
static void tc(struct tb *t, char c)
{
    if (t->n + 1 < sizeof t->b) {
        t->b[t->n++] = c;
        t->b[t->n] = 0;
    }
}
static void ts(struct tb *t, const char *s) { while (s && *s) tc(t, *s++); }
static void tu(struct tb *t, uint64_t v)
{
    char d[24];
    unsigned i = 0;
    do {
        d[i++] = (char)('0' + v % 10U);
        v /= 10U;
    } while (v && i < sizeof d);
    while (i)
        tc(t, d[--i]);
}
static void tx(struct tb *t, uint64_t v)
{
    static const char hx[] = "0123456789abcdef";
    int on = 0;
    ts(t, "0x");
    for (int s = 60; s >= 0; s -= 4) {
        unsigned d = (unsigned)(v >> s) & 15U;
        if (d || on || !s) {
            tc(t, hx[d]);
            on = 1;
        }
    }
}
static void tinit(struct tb *t, const char *pre)
{
    t->n = 0;
    t->b[0] = 0;
    ts(t, pre);
}
static void out(struct tb *t)
{
    if (have_hal && H.log)
        H.log(H.ctx, t->b);
}
static void say(const char *s)
{
    struct tb t;
    tinit(&t, "cal: ");
    ts(&t, s);
    out(&t);
}
static void set_why(const char *s)
{
    unsigned i = 0;
    for (; s && s[i] && i + 1 < sizeof why_buf; ++i)
        why_buf[i] = s[i];
    why_buf[i] = 0;
}
const char *cal_why(void) { return why_buf; }

/* ---- guarded access -------------------------------------------------------------------------------------------- */
static int reg_ok(uint64_t pa)
{
    uint64_t s = pa - ABOX_SFR;
    if (pa == ABOX_PMU + ABOX_PMU_AUD_CONF || pa == ABOX_CMU + CMU_DIV_AUDIF || pa == ABOX_CMU + CMU_DIV_UAIF(0) ||
        pa == ABOX_CMU + CMU_DIV_UAIF(1))
        return 1;
    return pa >= ABOX_SFR && (s == SFR_SYSPOWER_CTRL || s == SFR_ROUTE_CTRL0 || s == SFR_SPUS_CTRL0 ||
                              s == SFR_SPUS_CTRL2 || s == SFR_UAIF_CTRL0(0) || s == SFR_UAIF_CTRL1(0) ||
                              s == SFR_UAIF_CTRL0(1) || s == SFR_UAIF_CTRL1(1));
}
static uint32_t r32(uint64_t pa) { return H.rd32(H.ctx, pa); }
static void w32(uint64_t pa, uint32_t v)
{
    if (!reg_ok(pa)) {
        struct tb t;
        tinit(&t, "cal: REFUSED a write outside this file's registers (");
        tx(&t, pa);
        ts(&t, "): a bug, nothing written");
        out(&t);
        return;
    }
    H.wr32(H.ctx, pa, v);
}
static void m32(uint64_t pa, uint32_t mask, uint32_t v) { w32(pa, (r32(pa) & ~mask) | (v & mask)); }
static uint64_t now_us(void) { return H.now_us ? H.now_us(H.ctx) : 0; }
static void sync_ram(void)
{
    if (H.sync)
        H.sync(H.ctx);
}

static int cmu_div(uint32_t off, uint32_t mask, uint32_t v)
{
    m32(ABOX_CMU + off, mask, v);
    for (uint32_t t = 0; t <= 1000U; t += 10U) {
        if (!(r32(ABOX_CMU + off) & DIV_BUSY))
            return (r32(ABOX_CMU + off) & mask) == v;
        H.delay_us(H.ctx, 10);
    }
    return 0;
}

static uint64_t rmem_base(void)
{
    struct abox_cfg c;
    abox_cfg_default(&c);                       /* nobody changes abox_rmem (the DT's); abox.c refuses other places */
    return c.rmem_base;
}

int cal_hal_set(const struct abox_hal *h)
{
    if (!h || !h->rd32 || !h->wr32 || !h->delay_us || !h->log || !h->ram || !h->smc) {
        have_hal = 0;
        return ABOX_ESTATE;
    }
    H = *h;
    have_hal = 1;
    return 0;
}

/* the window is shared by its users (snd.c keeps it for any boot, the probe adds and removes its own): counted */
static int map_users;
int cal_map(void)
{
    int r = abox_map_extra(CAL_IOVA, rmem_base() + CAL_RM_PLAY, CAL_RM_WINDOW);
    if (!r)
        map_users++;
    return r;
}
void cal_unmap(void)
{
    if (map_users > 0 && --map_users == 0)
        (void)abox_map_extra(CAL_IOVA, 0, 0);
}
void cal_sync(void) { sync_ram(); }

/* our own tables (abox.c writes them at boot into abox_rmem): is RDMA0's IOVA a page table onto the ring? */
static int ring_mapped(uint32_t bytes)
{
    uint64_t base = rmem_base();
    const uint32_t *l1 = (const uint32_t *)H.ram(H.ctx, base + ABOX_RM_PT, 0x4000);
    if (!l1)
        return 0;
    uint32_t e = l1[CAL_IOVA >> 20];
    uint64_t l2pa = (uint64_t)(e & ~0x3FU) << 4;
    if ((e & 3U) != 1U || l2pa < base + ABOX_RM_PT || l2pa >= base + ABOX_RM_PT + 0x10000U)
        return 0;
    const uint32_t *l2 = (const uint32_t *)H.ram(H.ctx, l2pa, 0x400);
    for (uint32_t k = 0; l2 && k < (bytes + 0xFFFU) >> 12; ++k)
        if ((l2[k] & 3U) != 2U || (uint64_t)(l2[k] & ~0xFFU) << 4 != base + CAL_RM_PLAY + 0x1000ULL * k)
            return 0;
    return l2 != 0;
}
int cal_playing(void) { return E.playing; }
uint8_t *cal_play_buf(void) { return E.playing ? E.win : 0; }
uint32_t cal_ring_bytes(void) { return E.playing ? E.ring : 0; }

static int abox_up(void) { return abox_state() == ABOX_ST_READY || abox_state() == ABOX_ST_CALL; }

/* ---- the stream ---------------------------------------------------------------------------------------------- */
static int trace_ipc;                           /* cal_trace(): every PCM message we send, with its fields */
void cal_trace(int on) { trace_ipc = on; }

/* one IPC_PCMPLAYBACK message for RDMA0 (abox.c: msgtype, channel_id = task_id = 0, three param words) */
static int pcm(uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    static const char *const nm[] = { "OPEN", "CLOSE", "?13", "HW_PARAMS", "HW_FREE", "PREPARE", "TRIGGER", "POINTER",
                                      "?19", "SET_BUFFER" };
    int r = abox_pcm_msg(0, CAL_ID, type, a, b, c);
    if (trace_ipc) {
        struct tb t;
        tinit(&t, "cal: ipc -> PCMPLAYBACK (2) task 0 ch 0 ");
        ts(&t, type >= 11U && type <= 20U ? nm[type - 11U] : "?");
        ts(&t, " (msgtype ");
        tu(&t, type);
        ts(&t, "; ");
        tx(&t, a);
        ts(&t, ", ");
        tx(&t, b);
        ts(&t, ", ");
        tx(&t, c);
        ts(&t, r ? "): NOT taken (" : "): taken");
        if (r) {
            tu(&t, (uint64_t)(-r));
            ts(&t, ")");
        }
        out(&t);
    }
    return r;
}

void cal_dump(const char *tag)
{
    struct tb t;
    if (!have_hal || !abox_up())                /* never inside the domain when Calliope is not up */
        return;
    static const uint8_t ro[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x20, 0x30 };
    tinit(&t, "cal: dump ");
    ts(&t, tag);
    ts(&t, ": RDMA0");
    for (unsigned i = 0; i < sizeof ro; ++i) {
        tc(&t, ' ');
        tx(&t, r32(ABOX_SFR + SFR_RDMA0 + ro[i]));
    }
    ts(&t, " | SYSPOWER ");
    tx(&t, r32(ABOX_SFR + SFR_SYSPOWER_CTRL));
    ts(&t, " ROUTE0 ");
    tx(&t, r32(ABOX_SFR + SFR_ROUTE_CTRL0));
    ts(&t, " SPUS0 ");
    tx(&t, r32(ABOX_SFR + SFR_SPUS_CTRL0));
    ts(&t, " UAIF");
    tu(&t, (uint64_t)E.uaif);
    tc(&t, ' ');
    tx(&t, r32(ABOX_SFR + SFR_UAIF_CTRL0(E.uaif)));
    tc(&t, ' ');
    tx(&t, r32(ABOX_SFR + SFR_UAIF_CTRL1(E.uaif)));
    ts(&t, " st ");
    tx(&t, r32(ABOX_SFR + SFR_UAIF_STATUS(E.uaif)));
    out(&t);
}

static int fail_start(const char *what, int r)
{
    struct tb t;
    tinit(&t, "cal: start: ");
    ts(&t, what);
    out(&t);
    set_why(t.b + 5);
    cal_play_stop();
    set_why(t.b + 5);
    return r;
}

int cal_play_start(int uaif, uint32_t period, uint32_t periods)
{
    struct tb t;
    int r;
    if (!have_hal)
        return ABOX_ESTATE;
    if (E.playing || E.opened)
        return fail_start("already playing", ABOX_ESTATE);
    if (!abox_up())
        return fail_start("Calliope is not running (abox_boot first)", ABOX_ESTATE);
    if ((uaif != CAL_UAIF0 && uaif != CAL_UAIF1) || period < 128U || period % 16U || periods < 2U ||
        periods > CAL_RING_MAX / 128U || period * periods > CAL_RING_MAX)
        return fail_start("bad geometry (period >= 128 bytes in 16-byte steps, ring <= 64 KiB)", ABOX_ECFG);
    if (!ring_mapped(period * periods))
        return fail_start("RDMA0's ring is not in ABOX's SysMMU tables (Calliope was booted before cal_map())",
                          ABOX_EMMU);
    E.win = (uint8_t *)H.ram(H.ctx, rmem_base() + CAL_RM_PLAY, CAL_RM_WINDOW);
    if (!E.win)
        return fail_start("the ring's window in abox_rmem is not mappable", ABOX_EMMU);
    E.uaif = uaif, E.period = period, E.periods = periods, E.ring = period * periods;
    E.call = abox_state() == ABOX_ST_CALL;

    /* 1: mixer controls, clocks, UAIF (the found values kept to put back) */
    E.spus0 = r32(ABOX_SFR + SFR_SPUS_CTRL0);
    E.route0 = r32(ABOX_SFR + SFR_ROUTE_CTRL0);
    E.uc0 = r32(ABOX_SFR + SFR_UAIF_CTRL0(uaif));
    E.uc1 = r32(ABOX_SFR + SFR_UAIF_CTRL1(uaif));
    E.sysp = r32(ABOX_SFR + SFR_SYSPOWER_CTRL);
    E.opened = 1;                               /* from here cal_play_stop() puts things back */
    if (!cmu_div(CMU_DIV_AUDIF, 0x1FFU, DIV_AUDIF_UAIF) || !cmu_div(CMU_DIV_UAIF(uaif), 0x1FFU, DIV_UAIF_BCLK))
        return fail_start("a UAIF clock divider did not take its value", ABOX_ECLOCK);
    m32(ABOX_SFR + SFR_SPUS_CTRL0, CHAIN0_MASK, CHAIN0_SIFS0);
    m32(ABOX_SFR + SFR_ROUTE_CTRL0, 0xFU << (4 * uaif), 1U << (4 * uaif));
    if (!(E.call && uaif == CAL_UAIF1)) {
        uint32_t mask = UAIF_MASTER | (uaif == CAL_UAIF0 ? UAIF_DATA_MODE | UAIF_IRQ_MODE : 0U);
        m32(ABOX_SFR + SFR_UAIF_CTRL0(uaif), mask, UAIF_MASTER | (uaif == CAL_UAIF0 ? UAIF_DATA_MODE : 0U));
        m32(ABOX_SFR + SFR_UAIF_CTRL1(uaif), UAIF_CTRL1_MASK, UAIF_CTRL1_VAL);
    }
    /* 2: the ring and its guard zeroes */
    memset(E.win, 0, CAL_RM_WINDOW);
    sync_ram();
    /* 3: the PCM task */
    if ((r = pcm(ABOX_PCM_OPEN, 0, 0, 0)) ||
        (r = pcm(ABOX_PCM_SET_BUFFER, CAL_IOVA, period, periods)) ||
        (r = pcm(ABOX_PCM_HW_PARAMS, CAL_RATE, 16, 2)) ||
        (r = pcm(ABOX_PCM_PREPARE, 0, 0, 0)))
        return fail_start("Calliope did not take the PCM set-up (OPEN / SET_BUFFER / HW_PARAMS / PREPARE)", r);
    /* 4: start */
    m32(ABOX_SFR + SFR_SPUS_CTRL2, 1U, 1U);
    m32(ABOX_SFR + SFR_SYSPOWER_CTRL, 1U, 1U);
    m32(ABOX_SFR + SFR_UAIF_CTRL0(uaif), UAIF_SPK_EN, UAIF_SPK_EN);
    E.triggered = 1;
    if ((r = pcm(ABOX_PCM_TRIGGER, 1, 0, 0)))
        return fail_start("Calliope did not take TRIGGER start", r);
    E.playing = 1;
    tinit(&t, "cal: start: RDMA0 ");
    tu(&t, periods);
    ts(&t, " x ");
    tu(&t, period);
    ts(&t, " bytes at IOVA ");
    tx(&t, CAL_IOVA);
    ts(&t, " -> SPUS 0 -> SIFS0 -> UAIF");
    tu(&t, (uint64_t)uaif);
    ts(&t, E.call ? " (mixed into the call)" : "");
    ts(&t, "; RDMA0 CTRL0 ");
    tx(&t, r32(ABOX_SFR + SFR_RDMA0 + RDMA_CTRL0));
    ts(&t, " BUF_STR ");
    tx(&t, r32(ABOX_SFR + SFR_RDMA0 + RDMA_BUF_STR));
    ts(&t, " STATUS ");
    tx(&t, r32(ABOX_SFR + SFR_RDMA0 + RDMA_STATUS));
    out(&t);
    set_why(E.uaif ? "playing through Calliope to the amplifiers" : "playing through Calliope to the codec");
    return 0;
}

uint32_t cal_play_pos(uint32_t *status)
{
    if (!E.playing || !abox_up())
        return CAL_NOPOS;
    uint32_t st = r32(ABOX_SFR + SFR_RDMA0 + RDMA_STATUS);
    if (status)
        *status = st;
    if (!(st & RDMA_PROGRESS))
        return CAL_NOPOS;
    return ((((st >> 16) & 0x1FFFU) << 4) + (st & 0x1FFFU)) % E.ring;
}

uint32_t cal_play_ptr(uint32_t *count)
{
    uint32_t p = abox_pcm_pointer(0, CAL_ID, count);
    if (!E.playing || p < CAL_IOVA || p - CAL_IOVA >= E.ring)
        return CAL_NOPOS;
    return p - CAL_IOVA;
}

void cal_play_stop(void)
{
    if (!E.opened)
        return;
    int up = have_hal && abox_up(), call = up && abox_state() == ABOX_ST_CALL;
    if (up) {
        if (E.triggered)
            (void)pcm(ABOX_PCM_TRIGGER, 0, 0, 0);
        uint32_t t = 0;
        while ((r32(ABOX_SFR + SFR_RDMA0 + RDMA_CTRL0) & RDMA_ENABLE) && t < STOP_US) {
            H.delay_us(H.ctx, 100);
            t += 100;
        }
        if (t >= STOP_US)
            say("stop: RDMA0 still enabled after 40 ms (Calliope's barrier): going on");
        H.delay_us(H.ctx, FLUSH_US);
        if (!(call && E.uaif == CAL_UAIF1)) {
            m32(ABOX_SFR + SFR_UAIF_CTRL0(E.uaif), UAIF_SPK_EN, 0);
            m32(ABOX_SFR + SFR_UAIF_CTRL0(E.uaif), ~UAIF_SPK_EN, E.uc0);
            m32(ABOX_SFR + SFR_UAIF_CTRL1(E.uaif), UAIF_CTRL1_MASK, E.uc1);
            m32(ABOX_SFR + SFR_ROUTE_CTRL0, 0xFU << (4 * E.uaif), E.route0);
        }
        if (!call)
            m32(ABOX_SFR + SFR_SYSPOWER_CTRL, 1U, E.sysp);
        (void)pcm(ABOX_PCM_HW_FREE, 0, 0, 0);
        (void)pcm(ABOX_PCM_CLOSE, 0, 0, 0);
        m32(ABOX_SFR + SFR_SPUS_CTRL0, CHAIN0_MASK, E.spus0);
        m32(ABOX_SFR + SFR_SPUS_CTRL2, 1U, 1U);
        say(call ? "stop: RDMA0 stopped and closed; the call's UAIF1, route and DRAM left on"
                 : "stop: RDMA0 stopped and closed, UAIF and routes as found");
    } else {
        say("stop: Calliope is no longer running: the stream is just forgotten");
    }
    E.playing = E.opened = E.triggered = 0;
    set_why("off");
}

/* ======================================== the driver probe ======================================================== */
static struct {
    int level, step, active, booted, mapped, cfg, amp, found_pd, phase;
    uint64_t t0, tm0, boot_ms;
    uint64_t moved, sampled_us;
    uint32_t last, st_first, st_last, ptr0, ptr1, polls, slices;
    uint32_t rate[3], prate[3];
    int ok[3], ran[3];
} P;

/* the stream phases: 0 One UI's ring to UAIF1, 1 snd's ring to UAIF1 (only after 0 moved), 2 One UI's ring to UAIF0
 * (One UI's own boot route, the codec in reset: only after 0 did not move) */
static const struct { int uaif; uint32_t period, periods; const char *name; } phs[3] = {
    { CAL_UAIF1, CAL_ONEUI_PERIOD, CAL_ONEUI_PERIODS, "One UI's ring (4 x 3840) -> UAIF1" },
    { CAL_UAIF1, CAL_SND_PERIOD, CAL_SND_PERIODS, "snd's ring (32 x 2048) -> UAIF1" },
    { CAL_UAIF0, CAL_ONEUI_PERIOD, CAL_ONEUI_PERIODS, "One UI's ring (4 x 3840) -> UAIF0 (One UI's boot route)" },
};
#define SETTLE_US 300000U                       /* Calliope's post-boot messages (log ring, dumps, gear) taken first */

static void dline(struct tb *t) { out(t); }
static void dsay(const char *s)
{
    struct tb t;
    tinit(&t, "drvprobe: calliope: ");
    ts(&t, s);
    dline(&t);
}

#ifdef SBOS_KERNEL
static long k_file(void *ctx, const char *path, void *dst, uint32_t cap)
{
    (void)ctx;
    return bcmw_sb_vendor_read(path, dst, cap);
}
#endif

void cal_probe_setup(const struct abox_hal *h, const struct cal_probe_ops *ops)
{
    if (h)
        (void)cal_hal_set(h);
    if (ops)
        O = *ops;
    ops_set = 1;
}

static int setup(void)
{
#ifdef SBOS_KERNEL
    if (!ops_set) {
        static struct abox_hal kh;
        O.file = k_file;
        O.amp_probe = amp_probe;
        O.amp_receiver = amp_receiver;
        O.amp_off = amp_off;
        abox_hal_libsb(&kh, k_file, 0);
        (void)cal_hal_set(&kh);
        ops_set = 1;
    }
    amp_hal_set(audio_hal_libsb());
#endif
    if (!have_hal || !H.now_us || !H.file)
        return -1;
    abox_hal_set(&H);
    return 0;
}

static int pd_on(void) { return (r32(ABOX_PMU + ABOX_PMU_AUD_STAT) & 0xFU) == 0xFU; }

static int finish(int pass, const char *a, struct tb *rest)
{
    struct tb t;
    tinit(&t, pass ? "drvprobe: calliope: PASS " : "drvprobe: calliope: FAIL ");
    ts(&t, a);
    if (rest)
        ts(&t, rest->b);
    dline(&t);
    P.active = 0;
    P.step = 0;
    P.found_pd = -1;                            /* a later abort never powers the domain from a stale finding */
    return 1;
}

/* everything back: the amp, the stream, Calliope (abox_off), the extra window, abox.c's cfg, the traces */
static void put_back(void)
{
    if (P.amp && O.amp_off)
        O.amp_off();
    P.amp = 0;
    cal_play_stop();
    if (P.booted)
        abox_off();
    P.booted = 0;
    if (P.mapped)
        cal_unmap();
    P.mapped = 0;
    if (P.cfg) {
        struct abox_cfg c;
        abox_cfg_default(&c);
        (void)abox_cfg_set(&c);
        P.cfg = 0;
    }
    /* The AUD domain stays as abox_off() leaves it: off, as Android's runtime suspend does (SUSPEND / WFI, CPU off,
     * SysMMU off, pad retention, the TZPC save SMC, aud_off), even when the boot had left it on. modem-core owns the
     * power sequence (abox_pd_on / abox_pd_off later); nothing here powers it or reads inside it once it is off. */
    cal_trace(0);
    abox_trace_rx(0);
}

static int fail_out(const char *what, const char *why)
{
    struct tb t;
    tinit(&t, "");
    if (why) {
        ts(&t, ": ");
        ts(&t, why);
    }
    char keep[200];
    unsigned i = 0;
    for (; t.b[i] && i + 1 < sizeof keep; ++i)
        keep[i] = t.b[i];
    keep[i] = 0;
    put_back();
    tinit(&t, keep);
    ts(&t, "; everything off");
    return finish(0, what, &t);
}

/* Level 0: reads only */
static int probe0(void)
{
    struct tb t;
    uint32_t aud = r32(ABOX_PMU + ABOX_PMU_AUD_STAT), cpu = r32(ABOX_PMU + ABOX_PMU_CPU_STAT);
    int on = (aud & 0xFU) == 0xFU;
    tinit(&t, "drvprobe: calliope: level 0: AUD domain ");
    ts(&t, on ? "on (the boot's)" : "off");
    ts(&t, ", ABOX CPU ");
    ts(&t, (cpu & 1U) ? "ON" : "off");
    ts(&t, ", abox.c state ");
    tu(&t, (uint64_t)abox_state());
    if (on) {
        ts(&t, ", IP_INDEX ");
        tx(&t, r32(ABOX_SFR + SFR_IP_INDEX));
        ts(&t, ", SysMMU CTRL ");
        tx(&t, r32(ABOX_SYSMMU + MMU_CTRL));
    }
    dline(&t);
    if ((cpu & 1U) || abox_state() != ABOX_ST_OFF)
        return finish(1, "level 0: ABOX's CPU runs (or abox.c has it): files not read, state above", 0);
    static const struct { const char *path; uint32_t off, cap; } f[] = {
        { "firmware/calliope_sram.bin", ABOX_RM_RDMA4, ABOX_SRAM_SIZE },
        { "firmware/calliope_dram.bin", ABOX_RM_DRAM, ABOX_RM_DRAM_SZ },
        { "firmware/calliope_iva.bin", ABOX_RM_IVA, 0x80000U },
    };
    struct abox_cfg c;
    abox_cfg_default(&c);
    uint8_t *rm = (uint8_t *)H.ram(H.ctx, c.rmem_base, c.rmem_size);
    if (!rm)
        return finish(0, "level 0: abox_rmem is not mappable as RAM here (libsb map kind 3)", 0);
    long sz[3];
    uint64_t t0 = now_us();
    for (unsigned i = 0; i < 3; ++i)
        sz[i] = H.file(H.ctx, f[i].path, rm + f[i].off, f[i].cap);
    uint64_t ms = (now_us() - t0) / 1000U;
    tinit(&t, "files on VENDOR: calliope_sram.bin ");
    for (unsigned i = 0; i < 3; ++i) {
        if (i)
            ts(&t, i == 1 ? ", calliope_dram.bin " : ", calliope_iva.bin ");
        if (sz[i] > 0)
            tu(&t, (uint64_t)sz[i]);
        else
            ts(&t, "MISSING");
    }
    ts(&t, " bytes (read in ");
    tu(&t, ms);
    ts(&t, " ms)");
    if (sz[0] <= 0 || sz[1] <= 0)
        return finish(0, "level 0: Calliope's firmware is not on VENDOR: ", &t);
    return finish(1, "level 0: AUD domain state read, ", &t);
}

/* -24 dBFS 1 kHz: 48 samples a cycle, so One UI's 3840-frame ring holds exactly 80 cycles and loops seamlessly */
static void tone_fill(void)
{
    static const uint16_t q[13] = { 0, 4277, 8481, 12540, 16384, 19948, 23170, 25997, 28378, 30274, 31651, 32488, 32767 };
    int16_t *s = (int16_t *)(void *)cal_play_buf();
    uint32_t frames = cal_ring_bytes() / CAL_FRAME_BYTES;
    for (uint32_t i = 0; s && i < frames; ++i) {
        unsigned k = i % 48U, qk = k % 24U;
        int32_t v = q[qk <= 12U ? qk : 24U - qk];
        if (k >= 24U)
            v = -v;
        v = v * 2064 / 32767;
        s[2 * i] = s[2 * i + 1] = (int16_t)v;
    }
    sync_ram();
}

/* up to ~100 ms of 1 ms samples of RDMA0's position (and Calliope's messages every 10 ms); 0 ok, <0 Calliope fell */
static int measure_slice(void)
{
    uint64_t t0 = now_us(), t = t0;
    uint32_t last = cal_play_pos(&P.st_last);
    unsigned n = 0;
    if (!P.st_first)
        P.st_first = P.st_last;
    while (t - t0 < 100000U) {
        if (!(n++ % 10U)) {
            P.polls++;
            if (abox_poll() < 0)
                return -1;
        }
        H.delay_us(H.ctx, 1000);
        uint32_t pos = cal_play_pos(&P.st_last);
        if (pos != CAL_NOPOS && last != CAL_NOPOS)
            P.moved += (pos + cal_ring_bytes() - last) % cal_ring_bytes();
        last = pos;
        t = now_us();
    }
    P.sampled_us += t - t0;
    return 0;
}

static void phase_begin(void)
{
    P.moved = P.sampled_us = 0;
    P.st_first = P.st_last = 0;
    P.slices = 0;
    (void)cal_play_ptr(&P.ptr0);
    P.tm0 = now_us();
}

static void phase_end(unsigned ph)
{
    struct tb t;
    uint64_t wall = now_us() - P.tm0;
    (void)cal_play_ptr(&P.ptr1);
    P.ran[ph] = 1;
    P.rate[ph] = P.sampled_us ? (uint32_t)(P.moved * 1000000U / P.sampled_us) : 0;
    P.prate[ph] = wall ? (uint32_t)((uint64_t)(P.ptr1 - P.ptr0) * E.period * 1000000U / wall) : 0;
    P.ok[ph] = (P.rate[ph] >= 144000U && P.rate[ph] <= 240000U) || (P.prate[ph] >= 144000U && P.prate[ph] <= 240000U);
    tinit(&t, "drvprobe: calliope: ");
    ts(&t, phs[ph].name);
    ts(&t, ": RDMA0 STATUS moved ");
    tu(&t, P.rate[ph]);
    ts(&t, " B/s over ");
    tu(&t, P.sampled_us / 1000U);
    ts(&t, " ms (STATUS ");
    tx(&t, P.st_first);
    ts(&t, " .. ");
    tx(&t, P.st_last);
    ts(&t, "); Calliope sent ");
    tu(&t, P.ptr1 - P.ptr0);
    ts(&t, " period pointers = ");
    tu(&t, P.prate[ph]);
    ts(&t, " B/s; UAIF");
    tu(&t, (uint64_t)E.uaif);
    ts(&t, " STATUS ");
    tx(&t, r32(ABOX_SFR + SFR_UAIF_STATUS(E.uaif)));
    dline(&t);
}

static void put_ver(struct tb *t)
{
    uint32_t v = abox_version();
    for (int s = 24; s >= 0; s -= 8) {
        char c = (char)(v >> s);
        tc(t, c >= 0x20 && c < 0x7f ? c : '?');
    }
}

/* the rate of phase ph for the last line: "N B/s (STATUS)" / "N B/s (pointers)" */
static void put_rate(struct tb *t, unsigned ph)
{
    tu(t, P.rate[ph] ? P.rate[ph] : P.prate[ph]);
    ts(t, P.rate[ph] ? " B/s (STATUS)" : " B/s (pointers)");
}

/* Steps: 0 set-up, 1 boot, 2 settle (300 ms of Calliope's post-boot messages), 3 start a phase, 4 measure it,
 * 5 off and the last line. */
static int probe_active(unsigned *wait_ms)
{
    struct tb t;
    int r;
    *wait_ms = 0;
    switch (P.step) {
    case 0:
        P.active = 1;
        P.booted = P.mapped = P.cfg = P.amp = 0;
        for (unsigned i = 0; i < 3; ++i)
            P.ok[i] = P.ran[i] = 0, P.rate[i] = P.prate[i] = 0;
        dsay(P.level == 1 ? "level 1 (silent): Calliope booted from VENDOR, 300 ms for its post-boot messages, then "
                            "RDMA0 zeroes -> SIFS0 -> UAIF1 (amps off, codec in reset) through Calliope's PCM IPC: 1 s "
                            "One UI's ring, then 0.5 s snd's ring if it moved, else 0.5 s to UAIF0 (One UI's boot "
                            "route); abox_off; budget 12 s (boot <= 3.5 s)"
                          : "level 2 (attended): Calliope booted, a 1 kHz tone at -24 dBFS -> UAIF1 -> the earpiece "
                            "amp at its lowest level, 2 s; then everything off");
        if (abox_state() != ABOX_ST_OFF && abox_state() != ABOX_ST_FAILED)
            return finish(0, "ABOX is in use already (a call?): not touched", 0);
        P.found_pd = pd_on();
        {
            struct abox_cfg c;
            abox_cfg_default(&c);
            c.fw_log_max = 60;                  /* Calliope's boot lines (run 5: ~45); 60 more at each stream start */
            if (!abox_cfg_set(&c))
                P.cfg = 1;
        }
        if ((r = cal_map())) {
            tinit(&t, "abox_map_extra answered ");
            tu(&t, (uint64_t)(-r));
            return fail_out("RDMA0's ring could not be added to ABOX's SysMMU tables", t.b);
        }
        P.mapped = 1;
        P.step = 1;
        return 0;
    case 1: {
        uint64_t t0 = now_us();
        P.booted = 1;                           /* abox_off() even on a failure (abox.c already tore down) */
        r = abox_boot();
        P.boot_ms = (now_us() - t0) / 1000U;
        if (r)
            return fail_out("1/4 boot: Calliope did not boot", abox_why());
        tinit(&t, "drvprobe: calliope: 1/4 boot: Calliope said BOOT_DONE, version ");
        put_ver(&t);
        ts(&t, " (");
        tx(&t, abox_version());
        ts(&t, "); abox_boot() took ");
        tu(&t, P.boot_ms);
        ts(&t, " ms with the VENDOR reads");
        dline(&t);
        P.step = 2;
        P.tm0 = now_us();
        return 0;
    }
    case 2: {
        /* One UI opened RDMA0 ~1.3 s after BOOT_DONE; Calliope first sends its log ring, ~30 REPORT_DUMPs and a gear
         * request, one at a time through the RX slot. Take them before our PCM messages. */
        uint64_t t0 = now_us();
        while (now_us() - t0 < 100000U) {
            if (abox_poll() < 0)
                return fail_out("1/4 boot: Calliope stopped after BOOT_DONE", abox_why());
            H.delay_us(H.ctx, 5000);
        }
        if (now_us() - P.tm0 < SETTLE_US)
            return 0;
        dsay("1/4 boot: 300 ms after BOOT_DONE taken (Calliope's post-boot messages above)");
        P.phase = 0;
        P.step = 3;
        return 0;
    }
    case 3: {
        const unsigned ph = (unsigned)P.phase;
        abox_fw_log_more(60);                   /* Calliope's own lines from the stream's start */
        abox_trace_rx(24);                      /* and the messages it sends, raw */
        cal_trace(1);
        tinit(&t, "drvprobe: calliope: 2/4 stream: ");
        ts(&t, phs[ph].name);
        dline(&t);
        if ((r = cal_play_start(phs[ph].uaif, phs[ph].period, phs[ph].periods))) {
            if (ph == 0)
                return fail_out("2/4 stream: RDMA0 did not start through Calliope", cal_why());
            dsay("2/4 stream: that phase did not start: see the cal: lines");
            P.step = 5;
            return 0;
        }
        if (P.level == 2) {
            tone_fill();
            if (!O.amp_probe || !O.amp_receiver || O.amp_probe() || O.amp_receiver(1, 0))
                return fail_out("2/4 stream: the earpiece amp did not come on", 0);
            P.amp = 1;
            dsay("2/4 stream: the tone plays on the earpiece now (amp level 0)");
        }
        phase_begin();
        P.step = 4;
        return 0;
    }
    case 4: {
        const unsigned ph = (unsigned)P.phase;
        if (measure_slice())
            return fail_out("3/4 measure: Calliope stopped during the stream", abox_why());
        if (++P.slices == 1)
            cal_dump("100 ms after TRIGGER");
        uint64_t want = ph ? 500000U : P.level == 2 ? 2000000U : 1000000U;
        if (P.sampled_us < want)
            return 0;
        cal_dump("end of the phase");
        phase_end(ph);
        if (P.amp && O.amp_off)
            O.amp_off();
        P.amp = 0;
        cal_play_stop();
        if (P.level == 1 && ph == 0) {
            P.phase = P.ok[0] || P.rate[0] || P.prate[0] ? 1 : 2;
            P.step = 3;
        } else {
            P.step = 5;
        }
        return 0;
    }
    default: {
        put_back();
        /* PMU registers only (outside the domain) */
        int pd = pd_on(), cpu = (int)(r32(ABOX_PMU + ABOX_PMU_CPU_STAT) & 1U);
        tinit(&t, "drvprobe: calliope: 4/4 off: ABOX CPU ");
        ts(&t, cpu ? "STILL ON" : "off");
        ts(&t, ", AUD domain ");
        ts(&t, pd ? "on" : "off");
        ts(&t, P.found_pd == 1 ? " (it was on from the boot; abox_off powers it down by design)" : "");
        dline(&t);
        if (cpu)
            return finish(0, "4/4 off: ABOX's CPU did not stop", 0);
        struct tb p;
        tinit(&p, "");
        if (P.ok[0]) {
            tinit(&p, "Calliope booted (");
            put_ver(&p);
            ts(&p, " in ");
            tu(&p, P.boot_ms);
            ts(&p, P.level == 2 ? " ms), the earpiece tone played 2 s (did you hear it?); RDMA0 moved "
                                : " ms), RDMA0 moved ");
            put_rate(&p, 0);
            ts(&p, " through it to UAIF1 (192000 expected)");
            if (P.ran[1]) {
                ts(&p, "; snd's 32 x 2048 ring ");
                put_rate(&p, 1);
                ts(&p, P.ok[1] ? "" : " NOT at rate");
            }
            return finish(1, p.b, 0);
        }
        if (P.rate[0] || P.prate[0]) {
            ts(&p, "3/4 measure: RDMA0 moved but not at 48 kHz: ");
            put_rate(&p, 0);
            return finish(0, p.b, 0);
        }
        ts(&p, "3/4 measure: RDMA0 did not move through Calliope to UAIF1");
        if (P.ran[2]) {
            ts(&p, P.ok[2] ? "; to UAIF0 (One UI's boot route) it DID: " : "; to UAIF0 (One UI's boot route): ");
            put_rate(&p, 2);
        }
        ts(&p, " (see the cal: ipc / abox: rx / abox: fw lines)");
        return finish(0, p.b, 0);
    }
    }
}

int drv_calliope_probe(int level, unsigned *wait_ms)
{
    unsigned w = 0;
    if (!wait_ms)
        wait_ms = &w;
    *wait_ms = 0;
    if (!P.active || P.level != level) {
        P.level = level;
        P.step = 0;
        if (setup()) {
            dsay("FAIL no hardware access set up (HAL / clock / VENDOR reader missing)");
            return 1;
        }
    }
    if (level == DRV_LVL_READ)
        return probe0();
    return probe_active(wait_ms);
}

void drv_calliope_abort(void)
{
    if (!have_hal || !P.active)                 /* nothing of ours is on between runs */
        return;
    dsay("aborted: putting everything back");
    put_back();
    P.active = 0;
    P.step = 0;
}
