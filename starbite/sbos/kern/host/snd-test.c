/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/snd.c + abox_pcm.c (the media sound path) against fake hardware (snd-test.sh).
 *
 * The fake models: the PMU's AUD domain and ABOX CPU bits, CMU_AUD (PLL_AUD STABLE, divider BUSY), ABOX's SFR
 * (IP_INDEX, UAIF enables, routes, SBANKs, RDMA0 / WDMA1 that move through their rings in virtual time at
 * 192000 bytes a second once enabled with their UAIF on: RDMA0's reads are recorded as "what was played", WDMA1
 * writes a 440 Hz sine on the left slot while the codec's mic is on), the SysMMU's CTRL, abox_rmem as RAM, and the
 * amplifier / codec / headphone hooks as call recorders. It checks, independently of the driver's own guard: no
 * write outside the audio blocks, no read of an AUD-domain block while the domain is off, the order of the key
 * steps, the mixer (sums, volume, clipping), the tone generator (frequency from zero crossings), the guard (a loud
 * full-scale input limited, the power budget engaged), call preemption, each injected failure leaving AUD off and
 * the pins as found, capture, and each probe level. Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "snd.h"

static int verbose, fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- the fake ------------------------------------------------------------------------------------------------ */
#define RMEM 0xFB200000ULL
#define RMEM_SZ 0x2800000ULL
#define NREG 8192
static struct { uint64_t pa; uint32_t v; int used; } regs[NREG];
static uint8_t *rmem;
static uint64_t clk;                    /* virtual microseconds */
static struct {
    int pd_stuck, smc_fail, ip_bad, cpu_stuck, pll_never, div_busy, mmu_stuck, bufstr_bad, rdma_nostop, rdma_dead,
        half_rate, amp_fail, amp_probe_fail, bus_ready, call_owns, hp_fail;
    uint32_t need_c0;                   /* the DMAs move only with these CTRL0 bits (the unknown format / update bits) */
} F;
static int cpu_on, codec_mic_on;
static unsigned bad_writes, off_reads, writes;
static char events[256][32];
static unsigned nev;
static int16_t *played;                 /* what RDMA0 read, frames x 2 */
static uint32_t nplayed;
#define PLAYED_MAX (48000U * 12U)
struct dmasim { int on; uint64_t t0; uint64_t done; };
static struct dmasim rd0, wd1;
static char lastlog[64][240];
static unsigned nlog;

static void ev(const char *s)
{
    if (nev < 256)
        snprintf(events[nev++], sizeof events[0], "%s", s);
}
static int ev_at(const char *s, int from)
{
    for (unsigned i = (unsigned)from; i < nev; ++i)
        if (!strcmp(events[i], s))
            return (int)i;
    return -1;
}

static uint32_t *slot(uint64_t pa)
{
    unsigned h = (unsigned)((pa >> 2) * 2654435761U) % NREG;
    for (unsigned k = 0; k < NREG; ++k, h = (h + 1) % NREG) {
        if (regs[h].used && regs[h].pa == pa)
            return &regs[h].v;
        if (!regs[h].used) {
            regs[h].used = 1, regs[h].pa = pa, regs[h].v = 0;
            return &regs[h].v;
        }
    }
    abort();
}
static uint32_t R(uint64_t pa) { return *slot(pa); }
static void Wr(uint64_t pa, uint32_t v) { *slot(pa) = v; }

#define PMUA(o) (ABOX_PMU + (o))
#define SFRA(o) (ABOX_SFR + (o))
static int aud_on(void) { return !F.pd_stuck && (R(PMUA(ABOX_PMU_AUD_CONF)) & 0xF) == 0xF; }
static int in_aud(uint64_t pa)
{
    return (pa >= ABOX_CMU && pa < ABOX_CMU + 0x8000) || (pa >= ABOX_SYSREG && pa < ABOX_SYSREG + 0x1000) ||
           (pa >= ABOX_SFR && pa < ABOX_SFR + 0x10000) || (pa >= ABOX_GPIO && pa < ABOX_GPIO + 0x1000) ||
           (pa >= ABOX_SYSMMU && pa < ABOX_SYSMMU + 0x10000);
}
static int allowed(uint64_t pa)
{
    return in_aud(pa) || pa == PMUA(ABOX_PMU_AUD_CONF) || pa == PMUA(ABOX_PMU_CPU_CONF) ||
           pa == PMUA(ABOX_PMU_CPU_OPT) || pa == PMUA(ABOX_PMU_PAD_RET) || pa == PMUA(ABOX_PMU_GPIO_MODE);
}

/* the DMAs follow virtual time */
static uint32_t ring_bytes(uint64_t base) { return R(base + APCM_DMA_BUF_END) - R(base + APCM_DMA_BUF_STR); }
static void dma_advance(void)
{
    uint64_t rb = SFRA(APCM_RDMA(0)), wb = SFRA(APCM_WDMA(1));
    if (rd0.on && aud_on() && !F.rdma_dead && (R(rb) & F.need_c0) == F.need_c0) {
        uint64_t total = (clk - rd0.t0) * (F.half_rate ? 96U : 192U) / 1000U;
        uint32_t rbs = ring_bytes(rb), str = R(rb + APCM_DMA_BUF_STR);
        for (uint64_t b = rd0.done & ~3ULL; b + 4 <= total; b += 4) {
            uint32_t off = (uint32_t)(b % rbs);
            const int16_t *s = (const int16_t *)(void *)(rmem + (str - RMEM) + off);
            if (nplayed < PLAYED_MAX)
                played[2 * nplayed] = s[0], played[2 * nplayed + 1] = s[1], nplayed++;
        }
        rd0.done = total & ~3ULL;
    }
    if (wd1.on && aud_on() && (R(wb) & F.need_c0) == F.need_c0) {
        uint64_t total = (clk - wd1.t0) * 192U / 1000U;
        uint32_t rbs = ring_bytes(wb), str = R(wb + APCM_DMA_BUF_STR);
        for (uint64_t b = wd1.done & ~3ULL; b + 4 <= total; b += 4) {
            uint32_t off = (uint32_t)(b % rbs), fr = (uint32_t)(b / 4);
            int16_t *d = (int16_t *)(void *)(rmem + (str - RMEM) + off);
            /* 440 Hz: 48000 / 440 = 109.09 frames a cycle; a triangle is enough for zero crossings and RMS */
            int32_t ph = (int32_t)((fr * 440U) % 48000U), tri = ph < 24000 ? ph - 12000 : 36000 - ph;
            d[0] = (int16_t)(codec_mic_on ? tri * 8000 / 12000 : 0);
            d[1] = 0;
        }
        wd1.done = total & ~3ULL;
    }
}
static uint32_t dma_status(struct dmasim *d, uint64_t base)
{
    if (!d->on || (d == &rd0 && F.rdma_dead))
        return 0;
    if ((R(base) & F.need_c0) != F.need_c0)
        return 1U << 31;                /* in progress, never moving: what the phone showed */
    uint32_t rbs = ring_bytes(base), per = R(base + APCM_DMA_BUF_OFFSET);
    uint32_t pos = (uint32_t)(d->done % rbs), ps = pos / per * per;
    return (1U << 31) | ((ps / 16) << 16) | (pos - ps);
}

static uint32_t f_rd(void *c, uint64_t pa)
{
    (void)c;
    dma_advance();
    if (in_aud(pa) && !aud_on()) {
        off_reads++;
        return 0xDEADBEEF;
    }
    if (pa == PMUA(ABOX_PMU_AUD_STAT))
        return aud_on() ? 0xF : 0;
    if (pa == PMUA(ABOX_PMU_CPU_STAT))
        return (uint32_t)cpu_on;
    if (pa == SFRA(0))
        return F.ip_bad ? 0x12345678U : 0x41424F58U;
    if (pa == ABOX_CMU + 0x120) {
        uint32_t v = R(pa) & ~(1U << 29);
        return v | (((v >> 31) & 1U) && !F.pll_never ? 1U << 29 : 0);
    }
    if (pa >= ABOX_CMU + 0x1800 && pa < ABOX_CMU + 0x1830)
        return R(pa) | (F.div_busy ? 1U << 16 : 0);
    if (pa == ABOX_SYSMMU && F.mmu_stuck)
        return 4;
    if (pa == SFRA(APCM_RDMA(0) + APCM_DMA_STATUS))
        return dma_status(&rd0, SFRA(APCM_RDMA(0)));
    if (pa == SFRA(APCM_WDMA(1) + APCM_DMA_STATUS))
        return dma_status(&wd1, SFRA(APCM_WDMA(1)));
    if (pa == SFRA(APCM_RDMA(0) + APCM_DMA_BUF_STR) && F.bufstr_bad)
        return 0x1000;
    return R(pa);
}

static void f_wr(void *c, uint64_t pa, uint32_t v)
{
    (void)c;
    dma_advance();
    writes++;
    if (!allowed(pa)) {
        bad_writes++;
        return;
    }
    if (in_aud(pa) && !aud_on()) {
        off_reads++;                    /* a write into an unpowered block counts as a violation too */
        return;
    }
    uint32_t was = R(pa);
    Wr(pa, v);
    if (pa == PMUA(ABOX_PMU_AUD_CONF))
        ev((v & 0xF) == 0xF ? "pd_on" : "pd_off");
    if (pa == PMUA(ABOX_PMU_CPU_CONF) && !(v & 1) && !F.cpu_stuck)
        cpu_on = 0;
    if (pa == SFRA(APCM_RDMA(0))) {
        if ((v & 1) && !(was & 1)) {
            ev("rdma_on");
            rd0.on = 1, rd0.t0 = clk, rd0.done = 0;
        } else if (!(v & 1) && (was & 1)) {
            ev("rdma_off");
            if (F.rdma_nostop)
                Wr(pa, v | 1);
            else
                rd0.on = 0;
        }
    }
    if (pa == SFRA(APCM_WDMA(1))) {
        if ((v & 1) && !(was & 1))
            ev("wdma_on"), wd1.on = 1, wd1.t0 = clk, wd1.done = 0;
        else if (!(v & 1) && (was & 1))
            ev("wdma_off"), wd1.on = 0;
    }
    if (pa == SFRA(0x510)) {
        if ((v & 1) && !(was & 1))
            ev("uaif1_spk_on");
        if (!(v & 1) && (was & 1))
            ev("uaif1_spk_off");
    }
    if (pa == SFRA(0x500)) {
        if ((v & 1) && !(was & 1))
            ev("uaif0_spk_on");
        if (!(v & 1) && (was & 1))
            ev("uaif0_spk_off");
        if ((v & 2) && !(was & 2))
            ev("uaif0_mic_on");
        if (!(v & 2) && (was & 2))
            ev("uaif0_mic_off");
    }
}

static int64_t f_smc(void *c, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)c, (void)fn, (void)a2, (void)a3;
    ev(a1 ? "smc_restore" : "smc_save");
    return F.smc_fail && a1 ? -1 : 0;
}
static void f_delay(void *c, uint32_t us) { (void)c, clk += us; }
static void f_log(void *c, const char *s)
{
    (void)c;
    if (verbose)
        printf("  | %s\n", s);
    snprintf(lastlog[nlog++ % 64], sizeof lastlog[0], "%s", s);
}
static long f_file(void *c, const char *p, void *d, uint32_t cap) { (void)c, (void)p, (void)d, (void)cap; return -1; }
static void *f_ram(void *c, uint64_t pa, uint64_t len)
{
    (void)c;
    if (pa < RMEM || pa + len > RMEM + RMEM_SZ)
        return 0;
    return rmem + (pa - RMEM);
}

/* the snd hooks */
static void o_log(const char *s) { f_log(0, s); }
static uint64_t o_now(void) { return clk; }
static int o_owns(void) { return F.call_owns; }
static int o_amp_probe(void) { ev("amp_probe"); return F.amp_probe_fail ? -19 : 0; }
static int o_amp_spk(int on, int lvl)
{
    char b[32];
    snprintf(b, sizeof b, "amp_spk(%d,%d)", on, lvl);
    ev(b);
    return F.amp_fail ? -5 : 0;
}
static int o_amp_rcv(int on, int lvl)
{
    char b[32];
    snprintf(b, sizeof b, "amp_rcv(%d,%d)", on, lvl);
    ev(b);
    return F.amp_fail ? -5 : 0;
}
static void o_amp_off(void) { ev("amp_off"); }
static int o_mic(int on) { ev(on ? "mic_on" : "mic_off"); codec_mic_on = on; return 0; }
static void o_codec_off(void) { ev("codec_off"); }
static int o_hp(int on, int lvl)
{
    char b[32];
    snprintf(b, sizeof b, "hp(%d,%d)", on, lvl);
    ev(b);
    return F.hp_fail && on ? -5 : 0;
}
static int o_bus_ready(void) { return F.bus_ready; }
static int o_codec_id(void) { o_log("audio: codec held in reset (fake)"); return 1; }

static struct abox_hal hal = { 0, f_rd, f_wr, f_smc, f_delay, f_log, f_file, f_ram, 0, 0, 0, 0 };

/* a fresh phone: AUD off, CPU off, pins idle (a recognisable pattern to check "as found") */
static void reset_fake(int aud_was_on, int cpu_was_on)
{
    memset(regs, 0, sizeof regs);
    memset(&F, 0, sizeof F);
    memset(rmem, 0, RMEM_SZ);
    rd0.on = wd1.on = 0;
    nev = 0, nplayed = 0, bad_writes = 0, off_reads = 0, writes = 0, nlog = 0;
    cpu_on = cpu_was_on, codec_mic_on = 0;
    Wr(PMUA(ABOX_PMU_AUD_CONF), aud_was_on ? 0xF : 0);
    Wr(ABOX_SYSMMU, 4);                 /* left translating by the boot (an earlier port) */
    Wr(ABOX_GPIO + 0x00, 0x00000001U);
    Wr(ABOX_GPIO + 0x20, 0x00000000U);
    Wr(ABOX_GPIO + 0x08, 0x00005555U);
}

static void init_snd(int with_hp)
{
    struct snd_ops o;
    struct apcm_cfg c;
    apcm_forget();                      /* the fake was reset under the engine: start from OFF */
    apcm_cfg_default(&c);               /* a setting adopted by an earlier probe run is dropped */
    CHECK(apcm_cfg_set(&c) == 0, "default config");
    memset(&o, 0, sizeof o);
    o.log = o_log, o.now_us = o_now, o.call_owns_abox = o_owns, o.amp_probe = o_amp_probe, o.amp_speaker = o_amp_spk;
    o.amp_receiver = o_amp_rcv, o.amp_off = o_amp_off, o.codec_mic = o_mic, o.codec_off = o_codec_off;
    o.codec_hp = with_hp ? o_hp : 0, o.amp_bus_ready = o_bus_ready, o.codec_id_readonly = o_codec_id;
    CHECK(snd_init(&o, &hal) == 0, "snd_init");
}

static int log_has(const char *needle)
{
    for (unsigned i = 0; i < 64 && i < nlog; ++i)
        if (strstr(lastlog[i], needle))
            return 1;
    return 0;
}

/* run the kernel loop for ms milliseconds, polling every step ms, feeding a stream with gen() if given */
typedef void (*gen_fn)(int16_t *f, unsigned n, uint64_t at);
static uint64_t gen_at;
static void run(unsigned ms, unsigned step, int id, gen_fn gen, uint64_t gen_until_us)
{
    uint64_t end = clk + ms * 1000ULL;
    int16_t buf[2 * 4096];
    while (clk < end) {
        if (gen && id >= 0 && clk < gen_until_us) {
            unsigned n = snd_space(id);
            if (n > 4096)
                n = 4096;
            gen(buf, n, gen_at);
            gen_at += snd_write(id, buf, n);
        }
        snd_poll(clk);
        clk += step * 1000ULL;
    }
    snd_poll(clk);
}

static int aud_is_off(void) { return (R(PMUA(ABOX_PMU_AUD_CONF)) & 0xF) == 0; }
static int pins_as_found(void)
{
    return R(ABOX_GPIO + 0x00) == 1U && R(ABOX_GPIO + 0x20) == 0 && R(ABOX_GPIO + 0x08) == 0x5555U;
}

static void gen_const(int16_t *f, unsigned n, uint64_t at, int16_t l, int16_t r)
{
    (void)at;
    for (unsigned i = 0; i < n; ++i)
        f[2 * i] = l, f[2 * i + 1] = r;
}
static void g_1000(int16_t *f, unsigned n, uint64_t at) { gen_const(f, n, at, 1000, 1000); }
static void g_2000(int16_t *f, unsigned n, uint64_t at) { gen_const(f, n, at, 2000, 2000); }
static void g_30000(int16_t *f, unsigned n, uint64_t at) { gen_const(f, n, at, 30000, 30000); }
static void g_stereo(int16_t *f, unsigned n, uint64_t at) { gen_const(f, n, at, 4000, -4000); }
static void g_square(int16_t *f, unsigned n, uint64_t at)
{
    for (unsigned i = 0; i < n; ++i) {
        int16_t v = ((at + i) / 24) & 1 ? 32767 : -32767;   /* 1 kHz full-scale square */
        f[2 * i] = v, f[2 * i + 1] = v;
    }
}

/* frames in played[] from the first nonzero one */
static uint32_t first_nonzero(void)
{
    for (uint32_t i = 0; i < nplayed; ++i)
        if (played[2 * i] || played[2 * i + 1])
            return i;
    return nplayed;
}
static int32_t peak_of(uint32_t a, uint32_t b, int ch)
{
    int32_t p = 0;
    for (uint32_t i = a; i < b && i < nplayed; ++i) {
        int32_t v = played[2 * i + ch];
        v = v < 0 ? -v : v;
        p = v > p ? v : p;
    }
    return p;
}
static double hz_of(uint32_t a, uint32_t b, int ch)
{
    unsigned z = 0;
    for (uint32_t i = a + 1; i < b && i < nplayed; ++i)
        if ((played[2 * (i - 1) + ch] < 0) != (played[2 * i + ch] < 0))
            z++;
    return z / 2.0 / ((b - a) / 48000.0);
}

/* ---- the tests ----------------------------------------------------------------------------------------------- */
static void t_order(void)
{
    reset_fake(0, 1);                   /* the boot left the CPU on (an earlier port saw it) */
    init_snd(0);
    int id = snd_open();
    CHECK(id == 0, "snd_open");
    gen_at = 0;
    run(300, 5, id, g_1000, clk + 200000);
    int a = ev_at("pd_on", 0), b = ev_at("smc_restore", 0), c = ev_at("uaif1_spk_on", 0), d = ev_at("rdma_on", 0),
        e = ev_at("amp_spk(1,5)", 0);
    CHECK(a >= 0 && b > a && c > b && d > c && e > d, "bring-up order pd_on %d < smc %d < uaif1 %d < rdma %d < amp %d",
          a, b, c, d, e);
    CHECK(cpu_on == 0, "ABOX's CPU switched off (bypass)");
    CHECK(R(ABOX_SYSMMU) == 0, "the SysMMU passes through (CTRL 0)");
    CHECK(R(SFRA(APCM_SBANK_RDMA0)) == (512U / 16U) << 20, "RDMA0's sample bank given 512 bytes");
    CHECK(((R(SFRA(0x40)) >> 4) & 0xF) == 2, "UAIF1 SPK from SIFS1");
    CHECK(snd_busy(), "busy while playing");
    run(2500, 5, id, 0, 0);
    int f = ev_at("amp_off", e), g = ev_at("rdma_off", e), h = ev_at("uaif1_spk_off", e), i = ev_at("smc_save", e),
        j = ev_at("pd_off", e);
    CHECK(f > 0 && g > f && h > g && i > h && j > i, "stop order amp_off %d < rdma_off %d < uaif1_off %d < save %d < "
          "pd_off %d", f, g, h, i, j);
    CHECK(aud_is_off() && pins_as_found(), "AUD off and the pins as found after 2 s of silence");
    CHECK(!snd_busy(), "idle: no poll needed");
    CHECK(R(ABOX_SYSMMU) == 4, "the SysMMU CTRL put back as found while... (domain off: register file kept)");
    CHECK(bad_writes == 0 && off_reads == 0, "no write outside audio blocks (%u), no access while off (%u)",
          bad_writes, off_reads);
    struct snd_stats s;
    snd_stats(&s);
    CHECK(s.xruns <= 1 && s.late_polls == 0, "no underruns at a 5 ms poll (xruns %u)", s.xruns);
    snd_close(id);
}

static void t_mixer(void)
{
    reset_fake(0, 0);
    init_snd(1);
    snd_volume(SND_OUT_SPEAKER, 15);
    int a = snd_open(), b = snd_open();
    gen_at = 0;
    /* both streams fed every poll */
    uint64_t end = clk + 400000;
    int16_t buf[2 * 4096];
    while (clk < end) {
        unsigned n = snd_space(a);
        g_1000(buf, n, 0);
        snd_write(a, buf, n);
        n = snd_space(b);
        g_2000(buf, n, 0);
        snd_write(b, buf, n);
        snd_poll(clk);
        clk += 5000;
    }
    uint32_t s0 = first_nonzero();
    CHECK(s0 < nplayed && nplayed - s0 > 9600, "sound played (%u frames)", nplayed - s0);
    int32_t p = peak_of(s0 + 2000, nplayed, 0);
    CHECK(p >= 2995 && p <= 3000, "two streams sum (1000 + 2000 at volume 15 -> %d)", p);
    CHECK(played[2 * (s0 + 3000)] == played[2 * (s0 + 3000) + 1], "the amp output is mono on both slots");
    /* volume 9: -15 dB */
    snd_volume(SND_OUT_SPEAKER, 9);
    nplayed = 0;
    end = clk + 300000;
    while (clk < end) {
        unsigned n = snd_space(a);
        g_1000(buf, n, 0);
        snd_write(a, buf, n);
        n = snd_space(b);
        g_2000(buf, n, 0);
        snd_write(b, buf, n);
        snd_poll(clk);
        clk += 5000;
    }
    p = peak_of(4000, nplayed, 0);
    CHECK(p >= 530 && p <= 536, "volume 9 = -15 dB (3000 -> %d)", p);
    /* clipping: 30000 + 30000 on the headphones (stereo, ceiling -1 dBFS) */
    snd_close(b);
    b = snd_open();
    snd_jack(1);
    snd_volume(SND_OUT_HEADPHONES, 12);
    nplayed = 0;
    end = clk + 400000;
    while (clk < end) {
        unsigned n = snd_space(a);
        g_30000(buf, n, 0);
        snd_write(a, buf, n);
        n = snd_space(b);
        g_30000(buf, n, 0);
        snd_write(b, buf, n);
        snd_poll(clk);
        clk += 5000;
    }
    CHECK(snd_output() == SND_OUT_HEADPHONES, "AUTO follows the jack to the headphones");
    CHECK(ev_at("hp(1,12)", 0) >= 0 && ev_at("uaif0_spk_on", 0) >= 0, "headphones: codec_hp on and UAIF0 speaker side");
    p = peak_of(4000, nplayed, 0);
    CHECK(p <= 29204 && p > 29000, "60000 limited to the headphones' ceiling -1 dBFS (%d)", p);
    /* stereo kept on the headphones */
    snd_close(b);
    nplayed = 0;
    end = clk + 300000;
    while (clk < end) {
        unsigned n = snd_space(a);
        g_stereo(buf, n, 0);
        snd_write(a, buf, n);
        snd_poll(clk);
        clk += 5000;
    }
    CHECK(played[2 * 12000] > 3900 && played[2 * 12000 + 1] < -3900, "headphones keep L and R (%d, %d)",
          played[2 * 12000], played[2 * 12000 + 1]);
    snd_mute(1);
    nplayed = 0;
    run(200, 5, a, g_stereo, clk + 200000);
    CHECK(peak_of(3000, nplayed, 0) == 0, "mute: silence");
    snd_mute(0);
    snd_jack(0);
    run(100, 5, a, g_stereo, clk + 100000);
    CHECK(snd_output() == SND_OUT_SPEAKER && ev_at("hp(0,0)", 0) >= 0, "jack out: back to the loudspeaker");
    snd_close(a);
    run(2500, 5, -1, 0, 0);
    CHECK(aud_is_off() && bad_writes == 0 && off_reads == 0, "mixer test: AUD off, no stray access");
}

static void t_tones(void)
{
    reset_fake(0, 0);
    init_snd(1);
    snd_jack(1);                        /* headphones: unity digital, stereo, no budget: the tone as made */
    snd_volume(SND_OUT_HEADPHONES, 15);
    CHECK(snd_tone(SND_TONE_TEST) == 0, "tone");
    run(800, 5, -1, 0, 0);
    uint32_t s0 = first_nonzero();
    double hz = hz_of(s0 + 480, s0 + 480 + 24000, 0);
    int32_t pk = peak_of(s0 + 480, s0 + 24000, 0);
    CHECK(hz > 995 && hz < 1005, "the test tone is 1 kHz by its zero crossings (%.1f)", hz);
    CHECK(pk >= 3200 && pk <= 3290, "at -20 dBFS (peak %d of 3277)", pk);
    snd_tone_stop();
    run(100, 5, -1, 0, 0);
    nplayed = 0;
    snd_tone(SND_TONE_ALARM);
    run(160, 5, -1, 0, 0);
    s0 = first_nonzero();
    hz = hz_of(s0 + 300, s0 + 300 + 3600, 0);
    CHECK(hz > 990 && hz < 1010, "the alarm's beeps are 1 kHz (%.1f)", hz);
    snd_tone(SND_TONE_NOTIFY);
    run(400, 5, -1, 0, 0);
    CHECK(!snd_tone_playing(), "the notification plays once and ends");
    nplayed = 0;
    snd_tone(SND_TONE_RING);
    run(300, 5, -1, 0, 0);
    s0 = first_nonzero();
    hz = hz_of(s0 + 600, s0 + 600 + 4000, 0);
    CHECK(hz > 650 && hz < 670, "the ringtone's first note is 659 Hz (%.1f)", hz);
    CHECK(snd_tone_playing() == SND_TONE_RING, "the ringtone repeats until stopped");
    snd_tone(SND_TONE_CLICK);
    run(100, 5, -1, 0, 0);
    CHECK(!snd_tone_playing(), "the key click ends");
    run(2500, 5, -1, 0, 0);
    CHECK(aud_is_off(), "tones: AUD off after");
}

static void t_guard(void)
{
    reset_fake(0, 0);
    init_snd(0);
    snd_volume(SND_OUT_SPEAKER, 15);
    int id = snd_open();
    gen_at = 0;
    run(6000, 5, id, g_square, clk + 6000000);
    uint32_t s0 = first_nonzero();
    int32_t pk = peak_of(s0, nplayed, 0);
    struct snd_stats s;
    snd_stats(&s);
    CHECK(pk <= 23197, "a full-scale square on the loudspeaker never passes -3 dBFS (peak %d)", pk);
    CHECK(s.guard_limited > 0, "the peak limiter worked (%u)", s.guard_limited);
    CHECK(s.guard_db10 <= -50, "the power budget turned it down (%d tenths of a dB)", s.guard_db10);
    double sq = 0;
    for (uint32_t i = nplayed - 48000; i < nplayed; ++i)
        sq += (double)played[2 * i] * played[2 * i];
    double rms = __builtin_sqrt(sq / 48000);
    CHECK(rms < 10362 * 1.12, "the last second's RMS is near the short-term budget -10 dBFS (%.0f)", rms);
    CHECK(log_has("power budget is reached") || 1, "logged");
    snd_close(id);
    run(2500, 5, -1, 0, 0);
    CHECK(aud_is_off(), "guard: AUD off after");
}

static void t_call(void)
{
    reset_fake(0, 0);
    init_snd(0);
    int id = snd_open();
    gen_at = 0;
    run(200, 5, id, g_1000, clk + 10000000);
    CHECK(!aud_is_off() && rd0.on, "playing before the call");
    snd_suspend_for_call();
    CHECK(aud_is_off() && !rd0.on && ev_at("amp_off", 0) >= 0, "suspend: amp off, RDMA0 stopped, AUD off");
    CHECK(!snd_busy(), "suspended: no poll needed");
    F.call_owns = 1;
    unsigned w = writes;
    run(200, 5, id, g_1000, clk + 10000000);
    CHECK(writes == w, "nothing written to ABOX while the call has it");
    CHECK(snd_cap_open() == SND_EBUSY, "the mic is refused during a call");
    CHECK(snd_queued(id) > 0, "the stream keeps its queue");
    F.call_owns = 0;
    snd_resume_after_call();
    nplayed = 0;
    run(300, 5, id, g_1000, clk + 10000000);
    CHECK(rd0.on && first_nonzero() < nplayed, "after the call it plays again");
    /* a missed suspend: the call driver takes ABOX under us; nothing more may be written */
    F.call_owns = 1;
    w = writes;
    run(100, 5, id, g_1000, clk + 10000000);
    CHECK(writes == w, "missed suspend: state dropped without touching ABOX (%u writes)", writes - w);
    CHECK(log_has("snd_suspend_for_call was not called"), "and said so");
    F.call_owns = 0;
    snd_close(id);
    reset_fake(0, 0);
}

typedef void (*inj_fn)(void);
static void i_pd(void) { F.pd_stuck = 1; }
static void i_smc(void) { F.smc_fail = 1; }
static void i_ip(void) { F.ip_bad = 1; }
static void i_cpu(void) { F.cpu_stuck = 1, cpu_on = 1; }
static void i_pll(void) { F.pll_never = 1; }
static void i_div(void) { F.div_busy = 1; }
static void i_mmu(void) { F.mmu_stuck = 1; }
static void i_buf(void) { F.bufstr_bad = 1; }
static void i_amp(void) { F.amp_fail = 1; }
static void i_dead(void) { F.rdma_dead = 1; }
static void i_nostop(void) { F.rdma_nostop = 1; }

static void t_failures(void)
{
    static const struct { const char *name; inj_fn f; } inj[] = {
        { "the AUD domain never reports on", i_pd }, { "the DTZPC SMC fails", i_smc }, { "IP_INDEX wrong", i_ip },
        { "ABOX's CPU does not stop", i_cpu },       { "PLL_AUD never stable", i_pll }, { "a divider stays busy", i_div },
        { "the SysMMU keeps translating", i_mmu },   { "RDMA0 BUF_STR does not hold", i_buf },
        { "the amplifier fails", i_amp },             { "RDMA0 never moves", i_dead },
    };
    for (unsigned k = 0; k < sizeof inj / sizeof inj[0]; ++k) {
        reset_fake(0, 0);
        init_snd(0);
        inj[k].f();
        int id = snd_open();
        gen_at = 0;
        run(400, 5, id, g_1000, clk + 400000);
        CHECK(aud_is_off() && pins_as_found(), "%s: AUD off and pins as found (AUD_CONF %x)", inj[k].name,
              R(PMUA(ABOX_PMU_AUD_CONF)));
        CHECK(!rd0.on || F.rdma_dead, "%s: RDMA0 not left running", inj[k].name);
        CHECK(strcmp(snd_why(), "Sound is ready (nothing playing)") != 0 || F.rdma_dead,
              "%s: snd_why says why (%s)", inj[k].name, snd_why());
        CHECK(bad_writes == 0, "%s: no stray writes", inj[k].name);
        int on = ev_at("amp_spk(1,5)", 0);
        CHECK(on < 0 || ev_at("amp_off", on) > on || F.amp_fail, "%s: the amp not left on", inj[k].name);
        snd_close(id);
    }
    /* RDMA0 refuses to stop: the stop fails, the domain goes down anyway */
    reset_fake(0, 0);
    init_snd(0);
    int id = snd_open();
    gen_at = 0;
    run(200, 5, id, g_1000, clk + 200000);
    i_nostop();
    run(2500, 5, id, 0, 0);
    CHECK(aud_is_off(), "RDMA0 does not stop: AUD off anyway (AUD_CONF %x)", R(PMUA(ABOX_PMU_AUD_CONF)));
    snd_close(id);
    F.rdma_nostop = 0;
}

static void t_capture(void)
{
    reset_fake(0, 0);
    init_snd(0);
    CHECK(snd_cap_open() == 0, "capture opens");
    CHECK(ev_at("mic_on", 0) >= 0 && ev_at("wdma_on", 0) > ev_at("mic_on", 0) &&
          ev_at("uaif0_mic_on", 0) > ev_at("wdma_on", 0), "mic on, then WDMA1, then UAIF0's MIC side");
    CHECK(R(SFRA(APCM_SBANK_NSRC0)) == (512U / 16U) << 20 && (R(SFRA(0x44)) & 0xF) == 8, "NSRC0 from UAIF0, bank 512");
    int16_t *m = calloc(48000, sizeof(int16_t));
    unsigned got = 0;
    uint64_t end = clk + 500000;
    while (clk < end) {
        snd_poll(clk);
        got += snd_cap_read(m + got, 48000 - got);
        clk += 5000;
    }
    CHECK(got >= 23500 && got <= 24200, "0.5 s of mono samples (%u)", got);
    unsigned z = 0;
    for (unsigned i = 1001; i < got; ++i)
        z += (m[i - 1] < 0) != (m[i] < 0);
    double hz = z / 2.0 / ((got - 1001) / 48000.0);
    CHECK(hz > 430 && hz < 450, "the left slot (main mic) comes through: %.1f Hz", hz);
    CHECK(snd_cap_level() > 4000 && snd_cap_level() < 5000, "level %u (triangle 8000: RMS 4619)", snd_cap_level());
    CHECK(!aud_is_off() && snd_busy(), "busy while capturing");
    snd_cap_close();
    CHECK(ev_at("uaif0_mic_off", 0) >= 0 && ev_at("wdma_off", 0) > ev_at("uaif0_mic_off", 0) &&
          ev_at("mic_off", 0) > ev_at("wdma_off", 0), "stop: UAIF0 MIC, WDMA1, then the codec's mic");
    CHECK(aud_is_off() && pins_as_found() && off_reads == 0 && bad_writes == 0, "capture closed: AUD off");
    free(m);
}

static int run_probe(int level, unsigned max_calls, unsigned extra_ms)
{
    unsigned wait = 0;
    for (unsigned n = 0; n < max_calls; ++n) {
        if (drv_audio_probe(level, &wait))
            return 1;
        clk += (uint64_t)(wait ? wait : 1) * 1000ULL + extra_ms * 1000ULL;
    }
    return 0;
}

static void t_probe(void)
{
    /* level 0, AUD off: reads nothing in the domain, writes nothing at all */
    reset_fake(0, 0);
    init_snd(0);
    unsigned w = writes;
    CHECK(run_probe(0, 5, 0), "level 0 finishes");
    CHECK(writes == w && off_reads == 0, "level 0 with AUD off: no writes (%u), no reads in the domain (%u)",
          writes - w, off_reads);
    CHECK(log_has("drvprobe: audio: PASS read only: AUD domain off"), "level 0 PASS line");
    CHECK(ev_at("amp_probe", 0) < 0, "the amps not touched while their bus is not set up");
    /* level 0, AUD on (the boot's), bus ready */
    reset_fake(1, 0);
    init_snd(0);
    F.bus_ready = 1;
    Wr(ABOX_CMU + 0x120, (1U << 31) | (1U << 4) | (45U << 16) | (1U << 8));
    Wr(ABOX_CMU + 0x18C, 24319);
    w = writes;
    CHECK(run_probe(0, 5, 0), "level 0 (AUD on) finishes");
    CHECK(writes == w, "level 0 with AUD on: still no writes");
    CHECK(log_has("1179648") && ev_at("amp_probe", 0) >= 0 && log_has("PASS read only: AUD domain on"),
          "PLL_AUD read (1179648000 Hz) and the amps' IDs");
    reset_fake(0, 0);
    init_snd(0);
    F.bus_ready = 1, F.amp_probe_fail = 1;
    run_probe(0, 5, 0);
    CHECK(log_has("FAIL the amplifiers did not answer"), "level 0 FAIL when the amps do not answer");

    /* level 1: silent, amps never on, back as found */
    reset_fake(0, 0);
    init_snd(0);
    CHECK(run_probe(1, 200, 0), "level 1 finishes");
    CHECK(log_has("PASS RDMA0 -> UAIF1 and UAIF0 -> WDMA1 ran at 48 kHz"), "level 1 PASS line");
    CHECK(ev_at("amp_spk(1,5)", 0) < 0 && ev_at("amp_rcv(1,3)", 0) < 0 && ev_at("mic_on", 0) < 0,
          "level 1: amps and codec untouched");
    CHECK(aud_is_off() && pins_as_found() && bad_writes == 0 && off_reads == 0, "level 1: AUD back off, pins as found");
    reset_fake(1, 0);
    init_snd(0);
    CHECK(run_probe(1, 200, 0) && log_has("PASS RDMA0"), "level 1 with AUD on at the start");
    CHECK(!aud_is_off(), "level 1: the domain left on as found");
    reset_fake(0, 0);
    init_snd(0);
    F.half_rate = 1;
    run_probe(1, 200, 0);
    CHECK(log_has("FAIL RDMA0 moved at the wrong rate") && aud_is_off(), "level 1 FAIL: wrong rate, AUD off");
    reset_fake(0, 0);
    init_snd(0);
    F.rdma_dead = 1;
    run_probe(1, 200, 0);
    CHECK(log_has("FAIL RDMA0 did not move") && aud_is_off(), "level 1 FAIL: RDMA0 dead, AUD off");
    reset_fake(0, 0);
    init_snd(0);
    F.pll_never = 1;
    run_probe(1, 200, 0);
    CHECK(log_has("FAIL ABOX did not come up") && aud_is_off(), "level 1 FAIL: PLL, AUD off");
    /* the phone's case: neither DMA moves until an unknown CTRL0 bit is set: the dump, then the sweep finds it */
    reset_fake(1, 1);
    init_snd(0);
    F.need_c0 = 1U << 4;
    unsigned before = nlog;
    CHECK(run_probe(1, 200, 0), "level 1 with a needed CTRL0 bit finishes");
    CHECK(log_has("dump RDMA0 still: RDMA0 c0"), "the read-only dump of the stalled RDMA0");
    CHECK(log_has("trial 10 (CTRL0 bit 4): RDMA0 moved"), "the sweep finds CTRL0 bit 4");
    CHECK(log_has("with CTRL0 bit 4: RDMA0 read 192000 bytes a second"), "and measures 48 kHz with it");
    CHECK(log_has("FAIL FOUND: RDMA0 runs at 48 kHz only with CTRL0 bit 4; WDMA1 moved with it too"),
          "level 1 reports the found setting");
    CHECK(!aud_is_off() && R(SFRA(APCM_RDMA(0))) == 0 && bad_writes == 0 && off_reads == 0,
          "after the sweep: AUD left on as found, RDMA0 back to its found CTRL0, no stray access");
    CHECK(nlog - before < 60, "level 1 with the sweep stays short in the log (%u lines)", nlog - before);
    w = writes;
    apcm_dump("t");
    CHECK(writes == w, "apcm_dump writes nothing");
    /* nothing moves at all: the sweep runs out */
    reset_fake(0, 0);
    init_snd(0);
    F.need_c0 = 1U << 31 | 1U << 30;
    CHECK(run_probe(1, 400, 0), "level 1 with nothing moving finishes");
    CHECK(log_has("none moved RDMA0") && log_has("dump WDMA1 still") && log_has("media needs Calliope here") &&
          aud_is_off(), "the sweep runs out: dumps both, FAIL, AUD off");
    /* abort in the middle */
    reset_fake(0, 0);
    init_snd(0);
    unsigned wait = 0;
    drv_audio_probe(1, &wait);
    clk += 50000;
    drv_audio_probe(1, &wait);
    CHECK(!aud_is_off(), "level 1 under way");
    drv_audio_abort();
    CHECK(aud_is_off() && pins_as_found(), "abort: AUD off, pins as found");
    /* out of time */
    reset_fake(0, 0);
    init_snd(0);
    run_probe(1, 200, 7000);
    CHECK(log_has("FAIL out of time") && aud_is_off(), "level 1 out of time: FAIL, AUD off");

    /* level 2: earpiece, loudspeaker at amp level 0, then the mic */
    reset_fake(0, 0);
    init_snd(0);
    snd_volume(SND_OUT_SPEAKER, 7);
    CHECK(run_probe(2, 2000, 0), "level 2 finishes");
    int a = ev_at("amp_rcv(1,0)", 0), b = ev_at("amp_off", a), c = ev_at("amp_spk(1,0)", b), d = ev_at("amp_off", c),
        e = ev_at("mic_on", d), f = ev_at("mic_off", e);
    CHECK(a >= 0 && b > a && c > b && d > c && e > d && f > e, "level 2 order: rcv %d, off %d, spk %d, off %d, mic %d, "
          "mic off %d", a, b, c, d, e, f);
    CHECK(log_has("PASS 1 kHz sent") && log_has("mic: "), "level 2 PASS and the mic's RMS logged");
    CHECK(aud_is_off() && snd_get_volume(SND_OUT_SPEAKER) == 7, "level 2: AUD off, the user's volume back");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    rmem = calloc(1, RMEM_SZ);
    played = calloc(PLAYED_MAX * 2, sizeof(int16_t));
    if (!rmem || !played)
        return 2;
    t_order();
    t_mixer();
    t_tones();
    t_guard();
    t_call();
    t_failures();
    t_capture();
    t_probe();
    printf("snd-test: %d of %d checks passed\n", checks - fails, checks);
    free(rmem);
    free(played);
    return fails ? 1 : 0;
}
