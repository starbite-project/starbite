/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: media playback and capture through Calliope (abox_media.h has the what, the wiring and the sources).
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
 * The capture (cal_cap_start), the same steps for WDMA1 (abox_media.h: the facts and where they come from):
 *   1  NSRC0 = UAIF0 (ROUTE_CTRL1 [3:0] 8, nsrcx_texts), SPUM NSRC0 ASRC off and OUT to WDMA (SPUM_CTRL0 bits 4, 7 = 0);
 *      AUDIF / UAIF0 BCLK dividers; UAIF0 I2S master 48 kHz 2 x 16; QCHANNEL_DISABLE bit 2 (UAIF0's BCLK runs);
 *   2  the ring zeroed;
 *   3  IPC_PCMCAPTURE channel 1: OPEN, SET_BUFFER (IOVA 0x92100000, period bytes, periods), HW_PARAMS, PREPARE;
 *   4  SIFM0 flush (SPUM_CTRL3 bit 0, PRE_PMU), DRAM on, TRIGGER 1, then UAIF0 MIC_EN (capture: the DMA first, as One
 *      UI's log shows abox_wdma_trigger before abox_uaif_trigger[C]).
 * Stop (cal_cap_stop): TRIGGER 0, wait for WDMA1 CTRL0 ENABLE to clear (40 ms, as RDMA0's barrier), UAIF0 MIC_EN off
 * (One UI: abox_wdma_trigger(0), then abox_uaif_trigger[C](0)), DRAM off, HW_FREE, CLOSE, NSRC0 / SPUM / UAIF0 /
 * QCHANNEL_DISABLE as found, SIFM0 flush (POST_PMD). The call's own capture (NSRC1 -> SIFM1 -> WDMA2, abox.c) is never
 * touched; while a call has ABOX, capture is refused (the call owns UAIF0 MIC).
 *
 * Writes only: ABOX SFR's SYSPOWER_CTRL, QCHANNEL_DISABLE (bit 2), ROUTE_CTRL0 / CTRL1, SPUS_CTRL0 / CTRL2, SPUM_CTRL0
 * / CTRL3, UAIF0/1 CTRL0 / CTRL1; CMU_AUD's AUDIF and UAIF0/1 dividers; the PMU's AUD_CONF (only to switch the domain
 * back on when the probe found it on); abox_rmem's two ring windows. Reads only registers Samsung's driver or our
 * earlier phone runs read (RDMA0 / WDMA1 CTRL0 / BUF_STR / STATUS, UAIF STATUS, the PMU AUD / CPU status, IP_INDEX,
 * SysMMU CTRL). Everything else goes through abox.c's own calls. */
#include "abox_media.h"
#include "drvprobe.h"
#include "goertzel.h"

#ifdef SBOS_KERNEL
#include "sb.h"
#include "amp.h"
#include "codec.h"
#include "sb/bcmw_sb.h"
#else
void *memset(void *d, int c, size_t n);
#endif

#define SFR_SYSPOWER_CTRL  0x0010U
#define SFR_IP_INDEX       0x0000U
#define SFR_QCH_DISABLE    0x0038U          /* bit 2: BCLK_UAIF0 always on (abox.c's call path, set_tristate(0)) */
#define SFR_ROUTE_CTRL0    0x0040U
#define SFR_ROUTE_CTRL1    0x0044U          /* NSRCn [4n+3:4n]: 8 = UAIF0 (nsrcx_texts) */
#define SFR_SPUS_CTRL0     0x0200U
#define SFR_SPUS_CTRL2     0x0208U
#define SFR_SPUM_CTRL0     0x0300U          /* NSRC0: ASRC bit 4, OUT bit 7 (0 = WDMA) */
#define SFR_SPUM_CTRL3     0x030CU          /* SIFMn flush bit n */
#define SFR_UAIF_CTRL0(n)  (0x0500U + 0x10U * (unsigned)(n))
#define SFR_UAIF_CTRL1(n)  (0x0504U + 0x10U * (unsigned)(n))
#define SFR_UAIF_STATUS(n) (0x050CU + 0x10U * (unsigned)(n))
#define SFR_RDMA0          0x1000U
#define SFR_WDMA1          0x2100U          /* 17c52100.abox_wdma */
#define RDMA_CTRL0         0x00U
#define RDMA_BUF_STR       0x08U
#define RDMA_STATUS        0x30U
#define RDMA_ENABLE        (1U << 0)
#define RDMA_PROGRESS      (1U << 31)
#define UAIF_SPK_EN        (1U << 0)
#define UAIF_MIC_EN        (1U << 1)
#define UAIF_MASTER        (1U << 2)
#define UAIF_IRQ_MODE      (1U << 3)
#define UAIF_DATA_MODE     (1U << 4)
#define UAIF_CTRL1_MASK    0x1FFFFFFFU
#define UAIF_CTRL1_VAL     0x0984F000U
#define CHAIN0_MASK        0xFU
#define CHAIN0_SIFS0       0x2U             /* IN 0 (RDMA), OUT 1 (SIFS0) at [2:1], ASRC 0 */
#define NSRC0_MASK         0xFU
#define NSRC0_UAIF0        0x8U
#define SPUM_NSRC0_BITS    ((1U << 4) | (1U << 7))
#define QCH_UAIF0          (1U << 2)
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
    int playing, opened, triggered, uaif, call, t_uaif, t_dram;
    uint32_t period, periods, ring;
    uint8_t *win;
    uint32_t spus0, route0;
} E;
/* the capture (WDMA1) */
static struct {
    int capping, opened, triggered, t_uaif, t_dram, t_qch;
    uint32_t period, periods, ring;
    uint8_t *win;
    uint32_t route1, spum0, qch;
} K;
/* shared by the two directions: a UAIF's format (found values back when its last user stops) and DRAM on */
static struct { int users; uint32_t c0, c1; } U[2];
static int dram_users;
static uint32_t dram_found;
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
    return pa >= ABOX_SFR && (s == SFR_SYSPOWER_CTRL || s == SFR_QCH_DISABLE || s == SFR_ROUTE_CTRL0 ||
                              s == SFR_ROUTE_CTRL1 || s == SFR_SPUS_CTRL0 || s == SFR_SPUS_CTRL2 ||
                              s == SFR_SPUM_CTRL0 || s == SFR_SPUM_CTRL3 || s == SFR_UAIF_CTRL0(0) ||
                              s == SFR_UAIF_CTRL1(0) || s == SFR_UAIF_CTRL0(1) || s == SFR_UAIF_CTRL1(1));
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

void cal_abox_hal(void)
{
    if (have_hal && abox_state() == ABOX_ST_OFF)
        abox_hal_set(&H);
}

/* the windows are shared by their users (snd.c keeps them for any boot, the probe adds and removes its own): counted.
 * Both or neither: RDMA0's (slot 1 of abox.c's two) and WDMA1's (slot 2). */
static int map_users;
int cal_map(void)
{
    int r = abox_map_extra(CAL_IOVA, rmem_base() + CAL_RM_PLAY, CAL_RM_WINDOW);
    if (!r && (r = abox_map_extra(CAL_CAP_IOVA, rmem_base() + CAL_RM_CAP, CAL_RM_WINDOW)) && !map_users)
        (void)abox_map_extra(CAL_IOVA, 0, 0);
    if (!r)
        map_users++;
    return r;
}
void cal_unmap(void)
{
    if (map_users > 0 && --map_users == 0) {
        (void)abox_map_extra(CAL_IOVA, 0, 0);
        (void)abox_map_extra(CAL_CAP_IOVA, 0, 0);
    }
}
void cal_sync(void) { sync_ram(); }

/* our own tables (abox.c writes them at boot into abox_rmem): is iova a page table onto the window at rm_off? */
static int ring_mapped(uint32_t iova, uint32_t rm_off, uint32_t bytes)
{
    uint64_t base = rmem_base();
    const uint32_t *l1 = (const uint32_t *)H.ram(H.ctx, base + ABOX_RM_PT, 0x4000);
    if (!l1)
        return 0;
    uint32_t e = l1[iova >> 20];
    uint64_t l2pa = (uint64_t)(e & ~0x3FU) << 4;
    if ((e & 3U) != 1U || l2pa < base + ABOX_RM_PT || l2pa >= base + ABOX_RM_PT + 0x10000U)
        return 0;
    const uint32_t *l2 = (const uint32_t *)H.ram(H.ctx, l2pa, 0x400);
    for (uint32_t k = 0; l2 && k < (bytes + 0xFFFU) >> 12; ++k)
        if ((l2[k] & 3U) != 2U || (uint64_t)(l2[k] & ~0xFFU) << 4 != base + rm_off + 0x1000ULL * k)
            return 0;
    return l2 != 0;
}
int cal_playing(void) { return E.playing; }
uint8_t *cal_play_buf(void) { return E.playing ? E.win : 0; }
uint32_t cal_ring_bytes(void) { return E.playing ? E.ring : 0; }
int cal_capping(void) { return K.capping; }
const uint8_t *cal_cap_buf(void) { return K.capping ? K.win : 0; }
uint32_t cal_cap_ring_bytes(void) { return K.capping ? K.ring : 0; }

static int abox_up(void) { return abox_state() == ABOX_ST_READY || abox_state() == ABOX_ST_CALL; }

int cal_settle(uint32_t ms)
{
    if (!have_hal || !abox_up())
        return ABOX_ESTATE;
    for (uint32_t t = 0; t < ms; t += 5U) {
        int r = abox_poll();
        if (r < 0)
            return r;
        H.delay_us(H.ctx, 5000);
    }
    return 0;
}

/* ---- shared by both directions ------------------------------------------------------------------------------- */
static void uaif_take(int n)
{
    if (!U[n].users++) {
        U[n].c0 = r32(ABOX_SFR + SFR_UAIF_CTRL0(n));
        U[n].c1 = r32(ABOX_SFR + SFR_UAIF_CTRL1(n));
    }
    uint32_t mask = UAIF_MASTER | (n == CAL_UAIF0 ? UAIF_DATA_MODE | UAIF_IRQ_MODE : 0U);
    m32(ABOX_SFR + SFR_UAIF_CTRL0(n), mask, UAIF_MASTER | (n == CAL_UAIF0 ? UAIF_DATA_MODE : 0U));
    m32(ABOX_SFR + SFR_UAIF_CTRL1(n), UAIF_CTRL1_MASK, UAIF_CTRL1_VAL);
}
/* write 0: only counted (the call has the UAIF now) */
static void uaif_give(int n, int write)
{
    if (U[n].users <= 0 || --U[n].users || !write)
        return;
    m32(ABOX_SFR + SFR_UAIF_CTRL0(n), ~(UAIF_SPK_EN | UAIF_MIC_EN), U[n].c0);
    m32(ABOX_SFR + SFR_UAIF_CTRL1(n), UAIF_CTRL1_MASK, U[n].c1);
}
static void dram_take(void)
{
    if (!dram_users++)
        dram_found = r32(ABOX_SFR + SFR_SYSPOWER_CTRL);
    m32(ABOX_SFR + SFR_SYSPOWER_CTRL, 1U, 1U);
}
static void dram_give(int write)
{
    if (dram_users > 0 && !--dram_users && write)
        m32(ABOX_SFR + SFR_SYSPOWER_CTRL, 1U, dram_found);
}

/* ---- the streams --------------------------------------------------------------------------------------------- */
static int trace_ipc;                           /* cal_trace(): every PCM message we send, with its fields */
void cal_trace(int on) { trace_ipc = on; }

/* one PCM task message: IPC_PCMPLAYBACK for RDMA0 or IPC_PCMCAPTURE for WDMA1 (abox.c: msgtype, channel_id =
 * task_id, three param words) */
static int pcm_x(int cap, uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    static const char *const nm[] = { "OPEN", "CLOSE", "?13", "HW_PARAMS", "HW_FREE", "PREPARE", "TRIGGER", "POINTER",
                                      "?19", "SET_BUFFER" };
    int r = abox_pcm_msg(cap, cap ? CAL_CAP_ID : CAL_ID, type, a, b, c);
    if (trace_ipc) {
        struct tb t;
        tinit(&t, cap ? "cal: ipc -> PCMCAPTURE (3) task 1 ch 1 " : "cal: ipc -> PCMPLAYBACK (2) task 0 ch 0 ");
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
static int pcm(uint32_t type, uint32_t a, uint32_t b, uint32_t c) { return pcm_x(0, type, a, b, c); }
static int pcmc(uint32_t type, uint32_t a, uint32_t b, uint32_t c) { return pcm_x(1, type, a, b, c); }

void cal_dump(const char *tag)
{
    struct tb t;
    if (!have_hal || !abox_up())                /* never inside the domain when Calliope is not up */
        return;
    static const uint8_t ro[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x20, 0x30 };
    if (!E.opened && K.opened)
        goto wdma1;
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
    if (!K.opened)
        return;
wdma1:
    tinit(&t, "cal: dump ");
    ts(&t, tag);
    ts(&t, ": WDMA1");
    for (unsigned i = 0; i < sizeof ro; ++i) {
        tc(&t, ' ');
        tx(&t, r32(ABOX_SFR + SFR_WDMA1 + ro[i]));
    }
    ts(&t, " | ROUTE1 ");
    tx(&t, r32(ABOX_SFR + SFR_ROUTE_CTRL1));
    ts(&t, " SPUM0 ");
    tx(&t, r32(ABOX_SFR + SFR_SPUM_CTRL0));
    ts(&t, " QCH_DIS ");
    tx(&t, r32(ABOX_SFR + SFR_QCH_DISABLE));
    ts(&t, " UAIF0 ");
    tx(&t, r32(ABOX_SFR + SFR_UAIF_CTRL0(0)));
    tc(&t, ' ');
    tx(&t, r32(ABOX_SFR + SFR_UAIF_CTRL1(0)));
    ts(&t, " st ");
    tx(&t, r32(ABOX_SFR + SFR_UAIF_STATUS(0)));
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
    if (!ring_mapped(CAL_IOVA, CAL_RM_PLAY, period * periods))
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
    E.opened = 1;                               /* from here cal_play_stop() puts things back */
    if (!cmu_div(CMU_DIV_AUDIF, 0x1FFU, DIV_AUDIF_UAIF) || !cmu_div(CMU_DIV_UAIF(uaif), 0x1FFU, DIV_UAIF_BCLK))
        return fail_start("a UAIF clock divider did not take its value", ABOX_ECLOCK);
    m32(ABOX_SFR + SFR_SPUS_CTRL0, CHAIN0_MASK, CHAIN0_SIFS0);
    m32(ABOX_SFR + SFR_ROUTE_CTRL0, 0xFU << (4 * uaif), 1U << (4 * uaif));
    if (!(E.call && uaif == CAL_UAIF1)) {
        uaif_take(uaif);
        E.t_uaif = 1;
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
    dram_take();
    E.t_dram = 1;
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

/* a DMA's STATUS as a ring offset (abox_rdma_pointer(): RBUF_OFFSET [28:16] in 16 bytes + RBUF_CNT [12:0]) */
static uint32_t pos_of(uint32_t st, uint32_t ring)
{
    if (!(st & RDMA_PROGRESS) || !ring)
        return CAL_NOPOS;
    return ((((st >> 16) & 0x1FFFU) << 4) + (st & 0x1FFFU)) % ring;
}

uint32_t cal_play_pos(uint32_t *status)
{
    if (!E.playing || !abox_up())
        return CAL_NOPOS;
    uint32_t st = r32(ABOX_SFR + SFR_RDMA0 + RDMA_STATUS);
    if (status)
        *status = st;
    return pos_of(st, E.ring);
}

uint32_t cal_play_ptr(uint32_t *count)
{
    uint32_t p = abox_pcm_pointer(0, CAL_ID, count);
    if (!E.playing || p < CAL_IOVA || p - CAL_IOVA >= E.ring)
        return CAL_NOPOS;
    return p - CAL_IOVA;
}

/* RDMA0 / WDMA1 CTRL0 ENABLE cleared by Calliope after TRIGGER 0 (abox_rdma_disable_barrier: 40 ms) */
static void barrier(uint32_t dma, const char *late)
{
    uint32_t t = 0;
    while ((r32(ABOX_SFR + dma + RDMA_CTRL0) & RDMA_ENABLE) && t < STOP_US) {
        H.delay_us(H.ctx, 100);
        t += 100;
    }
    if (t >= STOP_US)
        say(late);
}

void cal_play_stop(void)
{
    if (!E.opened)
        return;
    int up = have_hal && abox_up(), call = up && abox_state() == ABOX_ST_CALL;
    if (up) {
        if (E.triggered)
            (void)pcm(ABOX_PCM_TRIGGER, 0, 0, 0);
        barrier(SFR_RDMA0, "stop: RDMA0 still enabled after 40 ms (Calliope's barrier): going on");
        H.delay_us(H.ctx, FLUSH_US);
        if (!(call && E.uaif == CAL_UAIF1)) {
            m32(ABOX_SFR + SFR_UAIF_CTRL0(E.uaif), UAIF_SPK_EN, 0);
            if (E.t_uaif)
                uaif_give(E.uaif, 1);
            m32(ABOX_SFR + SFR_ROUTE_CTRL0, 0xFU << (4 * E.uaif), E.route0);
        } else if (E.t_uaif) {
            uaif_give(E.uaif, 0);
        }
        if (E.t_dram)
            dram_give(!call);
        (void)pcm(ABOX_PCM_HW_FREE, 0, 0, 0);
        (void)pcm(ABOX_PCM_CLOSE, 0, 0, 0);
        m32(ABOX_SFR + SFR_SPUS_CTRL0, CHAIN0_MASK, E.spus0);
        m32(ABOX_SFR + SFR_SPUS_CTRL2, 1U, 1U);
        say(call ? "stop: RDMA0 stopped and closed; the call's UAIF1, route and DRAM left on"
                 : "stop: RDMA0 stopped and closed, UAIF and routes as found");
    } else {
        if (E.t_uaif)
            uaif_give(E.uaif, 0);
        if (E.t_dram)
            dram_give(0);
        say("stop: Calliope is no longer running: the stream is just forgotten");
    }
    E.playing = E.opened = E.triggered = E.t_uaif = E.t_dram = 0;
    set_why(K.capping ? "capturing through Calliope" : "off");
}

/* ---- capture: WDMA1 <- SIFM0 <- NSRC0 <- UAIF0 MIC ------------------------------------------------------------- */
static int fail_cap(const char *what, int r)
{
    struct tb t;
    tinit(&t, "cal: capture: ");
    ts(&t, what);
    out(&t);
    set_why(t.b + 5);
    cal_cap_stop();
    set_why(t.b + 5);
    return r;
}

int cal_cap_start(int uaif, uint32_t period, uint32_t periods)
{
    struct tb t;
    int r;
    if (!have_hal)
        return ABOX_ESTATE;
    if (K.capping || K.opened)
        return fail_cap("already capturing", ABOX_ESTATE);
    if (!abox_up())
        return fail_cap("Calliope is not running (abox_boot first)", ABOX_ESTATE);
    if (abox_state() == ABOX_ST_CALL)
        return fail_cap("a call has the mic (UAIF0 MIC is the call's): refused", ABOX_ESTATE);
    if (uaif != CAL_UAIF0 || period < 128U || period % 16U || periods < 2U || periods > CAL_RING_MAX / 128U ||
        period * periods > CAL_RING_MAX)
        return fail_cap("bad UAIF or geometry (UAIF0; period >= 128 bytes in 16-byte steps, ring <= 64 KiB)",
                        ABOX_ECFG);
    if (!ring_mapped(CAL_CAP_IOVA, CAL_RM_CAP, period * periods))
        return fail_cap("WDMA1's ring is not in ABOX's SysMMU tables (Calliope was booted before cal_map())",
                        ABOX_EMMU);
    K.win = (uint8_t *)H.ram(H.ctx, rmem_base() + CAL_RM_CAP, CAL_RM_WINDOW);
    if (!K.win)
        return fail_cap("the ring's window in abox_rmem is not mappable", ABOX_EMMU);
    K.period = period, K.periods = periods, K.ring = period * periods;

    /* 1: routes, clocks, UAIF0 (the found values kept to put back) */
    K.route1 = r32(ABOX_SFR + SFR_ROUTE_CTRL1);
    K.spum0 = r32(ABOX_SFR + SFR_SPUM_CTRL0);
    K.qch = r32(ABOX_SFR + SFR_QCH_DISABLE);
    K.opened = 1;                               /* from here cal_cap_stop() puts things back */
    if (!cmu_div(CMU_DIV_AUDIF, 0x1FFU, DIV_AUDIF_UAIF) || !cmu_div(CMU_DIV_UAIF(0), 0x1FFU, DIV_UAIF_BCLK))
        return fail_cap("a UAIF clock divider did not take its value", ABOX_ECLOCK);
    m32(ABOX_SFR + SFR_ROUTE_CTRL1, NSRC0_MASK, NSRC0_UAIF0);
    m32(ABOX_SFR + SFR_SPUM_CTRL0, SPUM_NSRC0_BITS, 0);
    uaif_take(CAL_UAIF0);
    K.t_uaif = 1;
    m32(ABOX_SFR + SFR_QCH_DISABLE, QCH_UAIF0, QCH_UAIF0);
    K.t_qch = 1;
    /* 2: the ring and its guard zeroes */
    memset(K.win, 0, CAL_RM_WINDOW);
    sync_ram();
    /* 3: the PCM task */
    if ((r = pcmc(ABOX_PCM_OPEN, 0, 0, 0)) ||
        (r = pcmc(ABOX_PCM_SET_BUFFER, CAL_CAP_IOVA, period, periods)) ||
        (r = pcmc(ABOX_PCM_HW_PARAMS, CAL_RATE, 16, 2)) ||
        (r = pcmc(ABOX_PCM_PREPARE, 0, 0, 0)))
        return fail_cap("Calliope did not take the PCM set-up (OPEN / SET_BUFFER / HW_PARAMS / PREPARE)", r);
    /* 4: start: the DMA first, then the UAIF's MIC side */
    m32(ABOX_SFR + SFR_SPUM_CTRL3, 1U, 1U);
    dram_take();
    K.t_dram = 1;
    K.triggered = 1;
    if ((r = pcmc(ABOX_PCM_TRIGGER, 1, 0, 0)))
        return fail_cap("Calliope did not take TRIGGER start", r);
    m32(ABOX_SFR + SFR_UAIF_CTRL0(0), UAIF_MIC_EN, UAIF_MIC_EN);
    K.capping = 1;
    tinit(&t, "cal: capture: WDMA1 ");
    tu(&t, periods);
    ts(&t, " x ");
    tu(&t, period);
    ts(&t, " bytes at IOVA ");
    tx(&t, CAL_CAP_IOVA);
    ts(&t, " <- SIFM0 <- NSRC0 <- UAIF0 MIC; WDMA1 CTRL0 ");
    tx(&t, r32(ABOX_SFR + SFR_WDMA1 + RDMA_CTRL0));
    ts(&t, " BUF_STR ");
    tx(&t, r32(ABOX_SFR + SFR_WDMA1 + RDMA_BUF_STR));
    ts(&t, " STATUS ");
    tx(&t, r32(ABOX_SFR + SFR_WDMA1 + RDMA_STATUS));
    out(&t);
    set_why("capturing through Calliope");
    return 0;
}

uint32_t cal_cap_pos(uint32_t *status)
{
    if (!K.capping || !abox_up())
        return CAL_NOPOS;
    uint32_t st = r32(ABOX_SFR + SFR_WDMA1 + RDMA_STATUS);
    if (status)
        *status = st;
    return pos_of(st, K.ring);
}

void cal_cap_stop(void)
{
    if (!K.opened)
        return;
    int up = have_hal && abox_up(), call = up && abox_state() == ABOX_ST_CALL;
    if (up) {
        if (K.triggered)
            (void)pcmc(ABOX_PCM_TRIGGER, 0, 0, 0);
        barrier(SFR_WDMA1, "capture: WDMA1 still enabled after 40 ms (Calliope's barrier): going on");
        if (!call) {                            /* a call (one came without snd_suspend_for_call) owns UAIF0 MIC */
            m32(ABOX_SFR + SFR_UAIF_CTRL0(0), UAIF_MIC_EN, 0);
            if (K.t_qch)
                m32(ABOX_SFR + SFR_QCH_DISABLE, QCH_UAIF0, K.qch);
        }
        if (K.t_uaif)
            uaif_give(CAL_UAIF0, !call);
        if (K.t_dram)
            dram_give(!call);
        (void)pcmc(ABOX_PCM_HW_FREE, 0, 0, 0);
        (void)pcmc(ABOX_PCM_CLOSE, 0, 0, 0);
        m32(ABOX_SFR + SFR_ROUTE_CTRL1, NSRC0_MASK, K.route1);
        m32(ABOX_SFR + SFR_SPUM_CTRL0, SPUM_NSRC0_BITS, K.spum0);
        m32(ABOX_SFR + SFR_SPUM_CTRL3, 1U, 1U);
        say(call ? "capture: WDMA1 stopped and closed; the call's UAIF0 MIC and DRAM left on"
                 : "capture: WDMA1 stopped and closed, UAIF0 and routes as found");
    } else {
        if (K.t_uaif)
            uaif_give(CAL_UAIF0, 0);
        if (K.t_dram)
            dram_give(0);
        say("capture: Calliope is no longer running: the capture is just forgotten");
    }
    K.capping = K.opened = K.triggered = K.t_uaif = K.t_dram = K.t_qch = 0;
    set_why(E.playing ? (E.uaif ? "playing through Calliope to the amplifiers" : "playing through Calliope to the codec")
                      : "off");
}

/* ======================================== the driver probe ======================================================== */
static struct {
    int level, step, active, booted, mapped, cfg, amp, mic, found_pd, phase, cphase;
    uint64_t t0, tm0, boot_ms;
    uint64_t moved, sampled_us, cmoved, csampled_us;
    uint32_t last, st_first, st_last, ptr0, ptr1, polls, slices, cst_first, cst_last, crd, cskip;
    uint32_t rate[3], prate[3], crate[2];
    int ok[3], ran[3], cran[2], cok;
    struct gz gz[2];                            /* the mic: [0] level 1's capture / level 2's baseline, [1] the tone */
    struct gz *gzc;
    struct gz_verdict heard;
} P;

/* the stream phases: 0 One UI's ring to UAIF1, 1 snd's ring to UAIF1 (only after 0 moved), 2 One UI's ring to UAIF0
 * (One UI's own boot route, the codec in reset: only after 0 did not move) */
static const struct { int uaif; uint32_t period, periods; const char *name; } phs[3] = {
    { CAL_UAIF1, CAL_ONEUI_PERIOD, CAL_ONEUI_PERIODS, "One UI's ring (4 x 3840) -> UAIF1" },
    { CAL_UAIF1, CAL_SND_PERIOD, CAL_SND_PERIODS, "snd's ring (32 x 2048) -> UAIF1" },
    { CAL_UAIF0, CAL_ONEUI_PERIOD, CAL_ONEUI_PERIODS, "One UI's ring (4 x 3840) -> UAIF0 (One UI's boot route)" },
};
/* the capture phases: 0 snd's ring (the engine's), 1 One UI's capture geometry (only after 0 did not move) */
static const struct { uint32_t period, periods; const char *name; } cph[2] = {
    { CAL_SND_PERIOD, CAL_SND_PERIODS, "snd's ring (32 x 2048)" },
    { CAL_ONEUI_CPERIOD, CAL_ONEUI_CPERIODS, "One UI's ring (4 x 1920)" },
};
#define SETTLE_US 300000U                       /* Calliope's post-boot messages (log ring, dumps, gear) taken first */
#define CSKIP 4800U                             /* level 2: the first 100 ms of the mic / of the tone not judged */
#define RATE_OK(r) ((r) >= 182400U && (r) <= 201600U)
enum { S_SETUP, S_BOOT, S_SETTLE, S_PLAY, S_PMEAS, S_CAP, S_CMEAS, S_OFF };

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
        O.codec_mic = codec_mic;
        O.codec_off = codec_off;
        abox_hal_libsb(&kh, k_file, 0);
        (void)cal_hal_set(&kh);
        ops_set = 1;
    }
    amp_hal_set(audio_hal_libsb());
    if (!codec_hal())
        codec_hal_set(audio_hal_libsb());
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
    P.step = S_SETUP;
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
    cal_cap_stop();
    if (P.mic && O.codec_mic)
        (void)O.codec_mic(0);
    if (P.mic && O.codec_off)
        O.codec_off();
    P.mic = 0;
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

/* the mic's samples WDMA1 wrote since the last look (the left slot: AIF1TX1, the main mic) into the current filter */
static void cap_take(void)
{
    const uint8_t *ring = cal_cap_buf();
    uint32_t rb = cal_cap_ring_bytes(), pos = cal_cap_pos(0);
    if (!ring || !P.gzc || pos == CAL_NOPOS)
        return;
    pos &= ~3U;
    for (uint32_t guard = rb / 4U; P.crd != pos && guard--; P.crd = (P.crd + 4U) % rb) {
        int16_t l = (int16_t)(ring[P.crd] | ring[P.crd + 1] << 8);
        if (P.cskip) {
            P.cskip--;
            continue;
        }
        gz_put(P.gzc, l);
    }
}

/* up to ~100 ms of 1 ms samples of RDMA0's and WDMA1's positions (and Calliope's messages and the mic's samples every
 * 10 ms); 0 ok, <0 Calliope fell */
static int measure_slice(void)
{
    uint64_t t0 = now_us(), t = t0;
    uint32_t last = cal_play_pos(&P.st_last), clast = cal_cap_pos(&P.cst_last);
    int play = cal_playing(), cap = cal_capping();
    unsigned n = 0;
    if (play && !P.st_first)
        P.st_first = P.st_last;
    if (cap && !P.cst_first)
        P.cst_first = P.cst_last;
    while (t - t0 < 100000U) {
        if (!(n++ % 10U)) {
            P.polls++;
            if (abox_poll() < 0)
                return -1;
            cap_take();
        }
        H.delay_us(H.ctx, 1000);
        if (play) {
            uint32_t pos = cal_play_pos(&P.st_last);
            if (pos != CAL_NOPOS && last != CAL_NOPOS)
                P.moved += (pos + cal_ring_bytes() - last) % cal_ring_bytes();
            last = pos;
        }
        if (cap) {
            uint32_t pos = cal_cap_pos(&P.cst_last);
            if (pos != CAL_NOPOS && clast != CAL_NOPOS)
                P.cmoved += (pos + cal_cap_ring_bytes() - clast) % cal_cap_ring_bytes();
            clast = pos;
        }
        t = now_us();
    }
    cap_take();
    if (play)
        P.sampled_us += t - t0;
    if (cap)
        P.csampled_us += t - t0;
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

static void cap_begin(void)
{
    P.crd = 0;
    P.cmoved = P.csampled_us = 0;
    P.cst_first = P.cst_last = 0;
    P.slices = 0;
    gz_reset(&P.gz[0]);
    gz_reset(&P.gz[1]);
    P.gzc = &P.gz[0];
    P.cskip = P.level == 2 ? CSKIP : 0U;
}

/* the capture's rate and the mic's level; the mic off and WDMA1 stopped */
static void cap_end(unsigned c)
{
    struct tb t;
    const struct gz *g = &P.gz[0];
    P.cran[c] = 1;
    P.crate[c] = P.csampled_us ? (uint32_t)(P.cmoved * 1000000U / P.csampled_us) : 0;
    P.cok |= RATE_OK(P.crate[c]);
    tinit(&t, "drvprobe: calliope: capture ");
    ts(&t, cph[c].name);
    ts(&t, ": WDMA1 STATUS moved ");
    tu(&t, P.crate[c]);
    ts(&t, " B/s over ");
    tu(&t, P.csampled_us / 1000U);
    ts(&t, " ms (STATUS ");
    tx(&t, P.cst_first);
    ts(&t, " .. ");
    tx(&t, P.cst_last);
    ts(&t, "); mic RMS ");
    tu(&t, gz_rms(g));
    ts(&t, " peak ");
    tu(&t, g->peak);
    ts(&t, P.mic ? " (codec mic on)" : " (codec mic NOT on)");
    ts(&t, "; UAIF0 STATUS ");
    tx(&t, r32(ABOX_SFR + SFR_UAIF_STATUS(0)));
    dline(&t);
    cal_cap_stop();
    if (P.mic && O.codec_mic)
        (void)O.codec_mic(0);
    if (P.mic && O.codec_off)
        O.codec_off();                          /* the codec back in reset */
    P.mic = 0;
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
/* tenths of a dB as "-12.3" */
static void put_db(struct tb *t, int32_t d10)
{
    if (d10 < 0) {
        tc(t, '-');
        d10 = -d10;
    }
    tu(t, (uint64_t)d10 / 10U);
    tc(t, '.');
    tu(t, (uint64_t)d10 % 10U);
}
/* the capture's rates: "WDMA1 N B/s" (snd's ring) [", One UI's ring N B/s"] */
static void put_crate(struct tb *t)
{
    ts(t, "WDMA1 ");
    tu(t, P.crate[0]);
    ts(t, " B/s");
    if (P.cran[1]) {
        ts(t, ", 4 x 1920 ring ");
        tu(t, P.crate[1]);
        ts(t, " B/s");
    }
}

/* Steps: S_SETUP, S_BOOT, S_SETTLE (300 ms of Calliope's post-boot messages), S_PLAY / S_PMEAS (a playback phase),
 * S_CAP / S_CMEAS (a capture phase), S_OFF (off and the last line).
 * Level 1: the playback phases, then the capture phases. Level 2: the capture (1 s of baseline), then playback phase 0
 * with the tone and the earpiece amp while the capture goes on, then everything off. */
static int probe_active(unsigned *wait_ms)
{
    struct tb t;
    int r;
    *wait_ms = 0;
    switch (P.step) {
    case S_SETUP:
        P.active = 1;
        P.booted = P.mapped = P.cfg = P.amp = P.mic = 0;
        P.cok = 0;
        for (unsigned i = 0; i < 3; ++i)
            P.ok[i] = P.ran[i] = 0, P.rate[i] = P.prate[i] = 0;
        for (unsigned i = 0; i < 2; ++i)
            P.cran[i] = 0, P.crate[i] = 0;
        P.gzc = 0;
        dsay(P.level == 1 ? "level 1 (silent): Calliope booted, 300 ms settle; RDMA0 zeroes -> UAIF1 (amps off) 1 s + "
                            "0.5 s; WDMA1 <- UAIF0 mic 0.5 s; abox_off; budget 12 s"
                          : "level 2 (quiet): Calliope booted; mic baseline 1 s, then 1 kHz -24 dBFS on the earpiece "
                            "(amp level 0) 2 s while the mic listens (Goertzel); then everything off");
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
        P.step = S_BOOT;
        return 0;
    case S_BOOT: {
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
        P.step = S_SETTLE;
        P.tm0 = now_us();
        return 0;
    }
    case S_SETTLE: {
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
        P.cphase = 0;
        P.step = P.level == 2 ? S_CAP : S_PLAY;
        return 0;
    }
    case S_PLAY: {
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
            P.step = S_CAP;
            return 0;
        }
        if (P.level == 2) {
            tone_fill();
            if (!O.amp_probe || !O.amp_receiver || O.amp_probe() || O.amp_receiver(1, 0))
                return fail_out("2/4 stream: the earpiece amp did not come on", 0);
            P.amp = 1;
            dsay("2/4 stream: the tone plays on the earpiece now (amp level 0); the mic listens");
        }
        phase_begin();
        P.step = S_PMEAS;
        return 0;
    }
    case S_PMEAS: {
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
        if (P.level == 2) {
            gz_judge(&P.gz[1], &P.gz[0], &P.heard);
            tinit(&t, "drvprobe: calliope: loopback: 1 kHz ");
            put_db(&t, P.heard.over_base);
            ts(&t, " dB over the baseline, ");
            put_db(&t, P.heard.over_nb);
            ts(&t, " dB over 700/1300 Hz (");
            tu(&t, P.gz[1].blocks);
            ts(&t, " x 20 ms; tone RMS ");
            tu(&t, gz_rms(&P.gz[1]));
            ts(&t, ", baseline RMS ");
            tu(&t, gz_rms(&P.gz[0]));
            ts(&t, "): heard ");
            ts(&t, P.heard.heard ? "yes" : "no");
            dline(&t);
            cap_end(0);
            P.step = S_OFF;
        } else if (ph == 0) {
            P.phase = P.ok[0] || P.rate[0] || P.prate[0] ? 1 : 2;
            P.step = S_PLAY;
        } else {
            P.step = S_CAP;
        }
        return 0;
    }
    case S_CAP: {
        const unsigned c = (unsigned)P.cphase;
        abox_fw_log_more(30);
        abox_trace_rx(12);
        cal_trace(1);
        tinit(&t, "drvprobe: calliope: capture: WDMA1 <- UAIF0 mic, ");
        ts(&t, cph[c].name);
        ts(&t, P.level == 2 ? ", 1 s of baseline first" : ", 0.5 s");
        dline(&t);
        if (O.codec_mic && !P.mic) {
            if (O.codec_mic(1))
                dsay("capture: the codec's mic did not come on (WDMA1 runs anyway)");
            else
                P.mic = 1;
        }
        if ((r = cal_cap_start(CAL_UAIF0, cph[c].period, cph[c].periods))) {
            if (P.level == 2)
                return fail_out("capture: WDMA1 did not start through Calliope", cal_why());
            dsay("capture: WDMA1 did not start: see the cal: lines");
            P.cran[c] = 1;
            P.step = S_OFF;
            return 0;
        }
        cap_begin();
        P.step = S_CMEAS;
        return 0;
    }
    case S_CMEAS: {
        const unsigned c = (unsigned)P.cphase;
        if (measure_slice())
            return fail_out("capture: Calliope stopped during the capture", abox_why());
        if (++P.slices == 1)
            cal_dump("100 ms after capture TRIGGER");
        if (P.csampled_us < (P.level == 2 ? 1100000U : 500000U))
            return 0;
        if (P.level == 2) {                     /* the baseline is in gz[0]; the tone's samples go to gz[1] */
            P.gzc = &P.gz[1];
            P.cskip = CSKIP;
            tinit(&t, "drvprobe: calliope: capture: baseline RMS ");
            tu(&t, gz_rms(&P.gz[0]));
            ts(&t, " peak ");
            tu(&t, P.gz[0].peak);
            ts(&t, ", WDMA1 so far ");
            tu(&t, P.csampled_us ? (uint32_t)(P.cmoved * 1000000U / P.csampled_us) : 0U);
            ts(&t, " B/s");
            dline(&t);
            P.phase = 0;
            P.step = S_PLAY;
            return 0;
        }
        cal_dump("end of the capture");
        cap_end(c);
        if (c == 0 && !P.crate[0]) {
            P.cphase = 1;
            P.step = S_CAP;
        } else {
            P.step = S_OFF;
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
        if (P.ok[0] && P.cok && P.level == 2) {
            ts(&p, "earpiece tone: heard by the mic: ");
            ts(&p, P.heard.heard ? "yes (1 kHz " : "no (1 kHz ");
            put_db(&p, P.heard.over_base);
            ts(&p, " dB over baseline); Calliope ");
            put_ver(&p);
            ts(&p, ", RDMA0 ");
            put_rate(&p, 0);
            ts(&p, ", ");
            put_crate(&p);
            return finish(1, p.b, 0);
        }
        if (P.ok[0] && P.cok) {
            ts(&p, "Calliope booted (");
            put_ver(&p);
            ts(&p, " in ");
            tu(&p, P.boot_ms);
            ts(&p, " ms); RDMA0 ");
            put_rate(&p, 0);
            ts(&p, " to UAIF1");
            if (P.ran[1]) {
                ts(&p, "; snd's 32 x 2048 ring ");
                put_rate(&p, 1);
                ts(&p, P.ok[1] ? "" : " NOT at rate");
            }
            ts(&p, "; ");
            put_crate(&p);
            ts(&p, ", mic RMS ");
            tu(&p, gz_rms(&P.gz[0]));
            return finish(1, p.b, 0);
        }
        if (P.ok[0]) {
            ts(&p, P.crate[0] || P.crate[1] ? "capture: WDMA1 moved but not at 48 kHz: "
                                            : "capture: WDMA1 did not move through Calliope (UAIF0 mic): ");
            put_crate(&p);
            ts(&p, "; RDMA0 ");
            put_rate(&p, 0);
            ts(&p, " ok");
            return finish(0, p.b, 0);
        }
        if (P.rate[0] || P.prate[0]) {
            ts(&p, "3/4 measure: RDMA0 moved but not at 48 kHz: ");
            put_rate(&p, 0);
            ts(&p, "; ");
            put_crate(&p);
            return finish(0, p.b, 0);
        }
        ts(&p, "3/4 measure: RDMA0 did not move through Calliope to UAIF1");
        if (P.ran[2]) {
            ts(&p, P.ok[2] ? "; to UAIF0 (One UI's boot route) it DID: " : "; to UAIF0 (One UI's boot route): ");
            put_rate(&p, 2);
        }
        ts(&p, " (see the cal: / abox: lines); ");
        put_crate(&p);
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
        P.step = S_SETUP;
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
    P.step = S_SETUP;
}
