/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/abox_media.c (media through Calliope) with kern/abox.c, against a fake register
 * space, a fake Calliope and a fake RDMA0 (calliope-test.sh builds it).
 *
 * The fake is abox-test.c's (PMU AUD domain and ABOX CPU bits, PLL_AUD, SRAM IPC words, ABOX's GIC, the SysMMU fault
 * bits, abox_rmem as RAM, VENDOR files with the phone's sizes) plus RDMA0 as Calliope runs it: it only starts on an
 * IPC_PCMPLAYBACK TRIGGER 1 for channel 0 after OPEN, SET_BUFFER, HW_PARAMS and PREPARE, and only if IOVA 0x91000000
 * is mapped in OUR page tables to the ring (else a SysMMU fault, as the hardware would raise); then STATUS advances at
 * 192000 bytes a second and Calliope sends a PCM_PLTDAI_POINTER every period; TRIGGER 0 stops it. WDMA1 the same on
 * IPC_PCMCAPTURE channel 1 (IOVA 0x92100000 -> CAL_RM_CAP): its STATUS advances at 192000 bytes a second and it writes
 * the "mic" into the ring: with NSRC0 = UAIF0, UAIF0 MIC_EN and the codec's mic on, a 1 kHz sine (amplitude 3000)
 * while a fake amp is on, plus noise (+-200) always; zeroes otherwise. F.deaf: noise only (the mic hears nothing).
 * Checked: the probe at levels 0 / 1 / 2, Samsung's IPC order, the page tables, the routes and the AUD domain put back,
 * every failure step leaving ABOX off, the abort, and the abox.c hooks' refusals. Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "abox.h"
#include "abox_media.h"
#include "drvprobe.h"
#include "snd.h"
#include "goertzel.h"

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

#define RMEM_BASE 0xFB200000ULL
#define RMEM_SIZE 0x2800000ULL
#define MAGIC_PA  (0xF7900000ULL + 0x600000ULL)
#define HSLOTS    (1u << 16)
#define RD0       (ABOX_SFR + 0x1000)
#define WD1       (ABOX_SFR + 0x2100)

struct wr { uint64_t seq, pa; uint32_t v; };
struct ipc { uint64_t seq, at; uint32_t w[8]; };
struct rxm { uint64_t at; uint32_t w[32]; };

static struct {
    uint64_t key[HSLOTS];
    uint32_t val[HSLOTS];
    uint8_t used[HSLOTS];
    uint32_t sram[ABOX_SRAM_SIZE / 4];
    uint8_t *rmem;
    uint32_t magic;
    uint64_t now, seq;
    struct wr *w;
    size_t nw, capw;
    struct ipc ipc[1024];
    unsigned nipc;
    struct rxm rx[32];
    unsigned nrx;
    struct { uint64_t seq, fn, a1, a2, a3; } smc[32];
    unsigned nsmc;
    unsigned violations, ram_violations;
    int boot_ok, pcm_dead, rdma_dead, missing_sram, only_uaif0, wdma_dead, deaf;
    /* the fake RDMA0 */
    int r_open, r_buf, r_par, r_prep, r_run;
    uint32_t r_iova, r_period, r_periods;
    uint64_t r_t0, r_next;
    unsigned r_starts, r_ptrs;
    /* the fake WDMA1 */
    int w_open, w_buf, w_par, w_prep, w_run;
    uint32_t w_iova, w_period, w_periods, w_route1, w_spum0, w_qch;
    uint64_t w_t0, w_done, w_trig_seq, w_stop_seq;
    unsigned w_starts;
    uint32_t noise;
    /* the fake amp */
    int amp_on;
    uint64_t amp_on_seq, amp_off_seq;
    unsigned amp_calls;
    /* the fake codec mic */
    int mic_on;
    unsigned mic_calls, codec_offs;
    char logs[6000][220];
    unsigned nlogs;
} F;

static int allowed(uint64_t pa)
{
    if (pa == ABOX_PMU + 0x4000 || pa == ABOX_PMU + 0x415C || pa == ABOX_PMU + 0x4164 || pa == ABOX_PMU + 0x4170 ||
        pa == ABOX_PMU + 0x1424)
        return 1;
    struct { uint64_t b, s; } r[] = { { ABOX_CMU, 0x8000 },  { ABOX_SYSREG, 0x1000 },     { ABOX_SFR, 0x10000 },
                                      { ABOX_GPIO, 0x1000 }, { ABOX_GICD, 0x1000 },       { ABOX_GICC, 0x1004 },
                                      { ABOX_SRAM, ABOX_SRAM_SIZE }, { ABOX_SYSMMU, 0x10000 } };
    for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); ++i)
        if (pa >= r[i].b && pa < r[i].b + r[i].s)
            return 1;
    return 0;
}
static unsigned slot(uint64_t pa)
{
    unsigned h = (unsigned)((pa * 0x9E3779B97F4A7C15ULL) >> 48) & (HSLOTS - 1);
    while (F.used[h] && F.key[h] != pa)
        h = (h + 1) & (HSLOTS - 1);
    return h;
}
static uint32_t reg(uint64_t pa)
{
    unsigned h = slot(pa);
    return F.used[h] ? F.val[h] : 0;
}
static void set(uint64_t pa, uint32_t v)
{
    unsigned h = slot(pa);
    F.used[h] = 1, F.key[h] = pa, F.val[h] = v;
}
static void rx_push(uint64_t at, uint32_t ipcid, uint32_t type, uint32_t p1, uint32_t p2, uint32_t p3)
{
    if (F.nrx >= 32)
        return;
    struct rxm *m = &F.rx[F.nrx++];
    memset(m, 0, sizeof(*m));
    m->at = at, m->w[0] = ipcid, m->w[2] = type, m->w[3] = p1, m->w[4] = p2, m->w[5] = p3;
}

static void calliope_start(void)
{
    static const char text[] = "Calliope boot\nmedia ready\n";
    uint32_t n = (uint32_t)strlen(text);
    F.sram[0x30800 / 4] = n, F.sram[0x30804 / 4] = 0, F.sram[0x30808 / 4] = 256;
    memcpy((uint8_t *)F.sram + 0x3080C, text, n);
    if (!F.boot_ok)
        return;
    rx_push(F.now + 20000, 1, 0x10, 0, 0x30800u, 0);
    rx_push(F.now + 64000, 1, 0x03, 0, 0, 0x43524C30);
}
static void cpu_update(void)
{
    int on = (reg(ABOX_PMU + 0x415C) & 1) && (reg(ABOX_PMU + 0x4164) >> 28 & 1);
    uint32_t was = reg(ABOX_PMU + 0x4160) & 1;
    set(ABOX_PMU + 0x4160, (uint32_t)on);
    if (on && !was) {
        set(ABOX_PMU + 0x3804, 0);
        F.r_open = F.r_buf = F.r_par = F.r_prep = F.r_run = 0;
        calliope_start();
    }
    if (!on)
        F.r_run = 0, F.w_run = 0;
}

/* RDMA0's position and Calliope's period pointers, as time passes */
static uint32_t rdma_bytes(void)
{
    if (!F.r_run || F.rdma_dead)
        return 0;
    return (uint32_t)((F.now - F.r_t0) * 192000ULL / 1000000ULL);
}
static void rdma_tick(void)
{
    if (!F.r_run || F.rdma_dead)
        return;
    uint64_t per_us = (uint64_t)F.r_period * 1000000ULL / 192000ULL;
    while (F.r_next <= F.now && F.nrx < 30) {
        uint32_t n = (uint32_t)((F.r_next - F.r_t0) * 192000ULL / 1000000ULL);
        rx_push(F.r_next, 2, 18, 0, F.r_iova + n % (F.r_period * F.r_periods), 0);
        F.r_ptrs++;
        F.r_next += per_us;
    }
}

/* the SysMMU walk for a ring: our L1 at PT_BASE, a page entry, L2 small pages onto the ring */
static int ring_mapped_at(uint32_t iova, uint64_t rm_off, uint32_t bytes)
{
    uint64_t pt = (uint64_t)reg(ABOX_SYSMMU + 0x00C) << 12;
    if (pt < RMEM_BASE || pt >= RMEM_BASE + RMEM_SIZE || !(reg(ABOX_SYSMMU) & 1))
        return 0;
    uint32_t e = ((uint32_t *)(void *)(F.rmem + (pt - RMEM_BASE)))[iova >> 20];
    if ((e & 3) != 1)
        return 0;
    uint64_t l2 = (uint64_t)(e & ~0x3Fu) << 4;
    if (l2 < RMEM_BASE || l2 >= RMEM_BASE + RMEM_SIZE)
        return 0;
    uint32_t *t2 = (uint32_t *)(void *)(F.rmem + (l2 - RMEM_BASE));
    for (uint32_t k = 0; k < (bytes + 0xFFF) >> 12; ++k)
        if ((t2[k] & 3) != 2 || ((uint64_t)(t2[k] & ~0xFFu) << 4) != RMEM_BASE + rm_off + 0x1000ULL * k)
            return 0;
    return 1;
}
static int ring_mapped(void) { return ring_mapped_at(0x91000000u, CAL_RM_PLAY, F.r_period * F.r_periods); }

/* WDMA1: bytes written so far, and the "mic" into the ring up to now */
static unsigned spk_on, rcv_on;                             /* snd's fake amp hooks (below) */
static uint32_t wdma_bytes(void)
{
    if (!F.w_run || F.wdma_dead)
        return 0;
    return (uint32_t)((F.now - F.w_t0) * 192000ULL / 1000000ULL);
}
static void wdma_fill(void)
{
    uint32_t ring = F.w_period * F.w_periods, total = wdma_bytes() & ~3u;
    if (!F.w_run || !ring)
        return;
    int16_t *d = (int16_t *)(void *)(F.rmem + CAL_RM_CAP);
    int path = (reg(ABOX_SFR + 0x44) & 0xF) == 8 && !(reg(ABOX_SFR + 0x300) & 0x90) && (reg(ABOX_SFR + 0x500) & 2) &&
               F.mic_on;
    int tone = (F.amp_on || spk_on || rcv_on) && !F.deaf;
    if (F.w_done + ring < total)
        F.w_done = total - ring;
    for (uint64_t b = F.w_done; b < total; b += 4) {
        uint32_t fr = (uint32_t)(b / 4), off = (uint32_t)(b % ring) / 2;
        int32_t v = 0;
        if (path) {
            F.noise = F.noise * 1103515245u + 12345u;
            v = (int32_t)((F.noise >> 16) % 401u) - 200;
            if (tone)
                v += (int32_t)lround(3000.0 * sin(2.0 * M_PI * (fr % 48u) / 48.0));
        }
        d[off] = (int16_t)v;
        d[off + 1] = 0;
    }
    F.w_done = total;
}

/* Calliope's own log: one line per PCM message into the SRAM ring at 0x30800 (writer, reader, size 256, text) */
static void fw_say(const char *s)
{
    uint8_t *text = (uint8_t *)F.sram + 0x3080C;
    uint32_t wr = F.sram[0x30800 / 4];
    for (; *s; ++s) {
        text[wr] = (uint8_t)*s;
        wr = (wr + 1) % 256;
    }
    F.sram[0x30800 / 4] = wr;
}

static void pcm_cap_msg(const uint32_t *m)
{
    char line[32];
    snprintf(line, sizeof line, "cap %u\n", m[2]);
    fw_say(line);
    switch (m[2]) {
    case 11: F.w_open = 1; break;
    case 20: F.w_buf = F.w_open, F.w_iova = m[4], F.w_period = m[5], F.w_periods = m[6]; break;
    case 14: F.w_par = F.w_buf && m[4] == 48000 && m[5] == 16 && m[6] == 2; break;
    case 16: F.w_prep = F.w_par; break;
    case 17:
        if (m[4] == 1 && F.w_prep && F.w_iova == 0x92100000u) {
            if (!ring_mapped_at(0x92100000u, CAL_RM_CAP, F.w_period * F.w_periods)) {
                set(ABOX_SYSMMU + 0x060, 1);
                set(ABOX_SYSMMU + 0x070, 0x92100000u);
                break;
            }
            F.w_run = 1, F.w_t0 = F.now, F.w_done = 0, F.w_starts++, F.w_trig_seq = F.seq;
            F.w_route1 = reg(ABOX_SFR + 0x44), F.w_spum0 = reg(ABOX_SFR + 0x300), F.w_qch = reg(ABOX_SFR + 0x38);
            set(WD1, reg(WD1) | 1);
            set(WD1 + 8, F.w_iova);
        } else if (m[4] == 0) {
            wdma_fill();
            F.w_run = 0, F.w_stop_seq = F.seq;
            set(WD1, reg(WD1) & ~1u);
        }
        break;
    case 15: F.w_prep = F.w_par = F.w_buf = 0; break;
    case 12: F.w_open = 0; break;
    }
}

static void pcm_msg(const uint32_t *m)
{
    if (m[0] == 3 && m[1] == 1 && m[3] == 1) {
        pcm_cap_msg(m);
        return;
    }
    if (m[0] != 2 || m[3] != 0)
        return;
    char line[32];
    snprintf(line, sizeof line, "pcm %u\n", m[2]);
    fw_say(line);
    switch (m[2]) {
    case 11: F.r_open = 1; break;
    case 20: F.r_buf = F.r_open, F.r_iova = m[4], F.r_period = m[5], F.r_periods = m[6]; break;
    case 14: F.r_par = F.r_buf && m[4] == 48000 && m[5] == 16 && m[6] == 2; break;
    case 16: F.r_prep = F.r_par; break;
    case 17:
        if (m[4] == 1 && F.r_prep && F.r_iova == 0x91000000u && (!F.only_uaif0 || (reg(ABOX_SFR + 0x40) & 0xF) == 1)) {
            if (!ring_mapped()) {                       /* the DMA's first read faults */
                set(ABOX_SYSMMU + 0x060, 1);
                set(ABOX_SYSMMU + 0x070, 0x91000000u);
                break;
            }
            F.r_run = 1, F.r_t0 = F.now, F.r_next = F.now + (uint64_t)F.r_period * 1000000ULL / 192000ULL;
            F.r_starts++;
            set(RD0, reg(RD0) | 1);
            set(RD0 + 8, F.r_iova);
        } else if (m[4] == 0) {
            F.r_run = 0;
            set(RD0, reg(RD0) & ~1u);
        }
        break;
    case 15: F.r_prep = F.r_par = F.r_buf = 0; break;
    case 12: F.r_open = 0; break;
    }
}

static uint32_t f_rd32(void *c, uint64_t pa)
{
    (void)c;
    wdma_fill();
    if (pa == WD1 + 0x30) {
        if (!F.w_run)
            return 0;
        uint32_t ring = F.w_period * F.w_periods, pos = ring ? wdma_bytes() % ring : 0;
        return 0x80000000u | ((pos >> 4) << 16) | (pos & 0xF);
    }
    if (pa >= ABOX_SRAM && pa < ABOX_SRAM + ABOX_SRAM_SIZE)
        return F.sram[(pa - ABOX_SRAM) / 4];
    if (pa == ABOX_GICC + 0x0C) {
        rdma_tick();
        if (F.nrx && F.rx[0].at <= F.now && !F.sram[ABOX_IPC_RX_ACK / 4]) {
            memcpy(&F.sram[ABOX_IPC_RX / 4], F.rx[0].w, sizeof(F.rx[0].w));
            F.sram[ABOX_IPC_RX_ACK / 4] = 1;
            memmove(F.rx, F.rx + 1, (--F.nrx) * sizeof(F.rx[0]));
            return (1u << 10) | 2u;
        }
        return 1023;
    }
    if (pa == RD0 + 0x30) {
        if (!F.r_run)
            return 0;
        uint32_t ring = F.r_period * F.r_periods, pos = ring ? rdma_bytes() % ring : 0;
        return 0x80000000u | ((pos >> 4) << 16) | (pos & 0xF);
    }
    return reg(pa);
}

static void f_wr32(void *c, uint64_t pa, uint32_t v)
{
    (void)c;
    F.seq++;
    if (!allowed(pa))
        F.violations++;
    if (F.nw == F.capw) {
        F.capw = F.capw ? F.capw * 2 : 65536;
        F.w = realloc(F.w, F.capw * sizeof(*F.w));
    }
    F.w[F.nw++] = (struct wr){ F.seq, pa, v };
    if (pa >= ABOX_SRAM && pa < ABOX_SRAM + ABOX_SRAM_SIZE) {
        F.sram[(pa - ABOX_SRAM) / 4] = v;
        return;
    }
    set(pa, v);
    if (pa == ABOX_PMU + 0x4000)
        set(ABOX_PMU + 0x4004, v & 0xF);
    if (pa == ABOX_PMU + 0x415C || pa == ABOX_PMU + 0x4164)
        cpu_update();
    if (pa == ABOX_CMU + 0x0120) {
        uint32_t x = v & ~(1u << 7) & ~(1u << 29);
        if (v >> 31)
            x |= 1u << 29;
        set(pa, x);
    }
    if (pa == ABOX_GICD + 0xF00) {
        uint32_t *m = &F.sram[ABOX_IPC_TX / 4];
        if (F.pcm_dead && m[0] == 2)
            return;
        if (F.nipc < 1024) {
            F.ipc[F.nipc].seq = F.seq, F.ipc[F.nipc].at = F.now;
            memcpy(F.ipc[F.nipc].w, m, sizeof(F.ipc[0].w));
            F.nipc++;
        }
        pcm_msg(m);
        if (m[0] == 1 && m[2] == 0x01)
            set(ABOX_PMU + 0x3804, 1u << 28);
        F.sram[ABOX_IPC_TX_ACK / 4] = 0;
    }
}

static int64_t f_smc(void *c, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)c;
    F.seq++;
    if (F.nsmc < 32)
        F.smc[F.nsmc++] = (typeof(F.smc[0])){ F.seq, fn, a1, a2, a3 };
    return 0;
}
static void f_delay(void *c, uint32_t us) { (void)c; F.now += us; }
static uint64_t f_now(void *c) { (void)c; return F.now; }
static void f_log(void *c, const char *s)
{
    (void)c;
    if (verbose)
        printf("  | %s\n", s);
    if (F.nlogs < 6000)
        snprintf(F.logs[F.nlogs++], sizeof(F.logs[0]), "%s", s);
}
static void *f_ram(void *c, uint64_t pa, uint64_t len)
{
    (void)c;
    if (pa >= RMEM_BASE && pa + len <= RMEM_BASE + RMEM_SIZE)
        return F.rmem + (pa - RMEM_BASE);
    if (pa == MAGIC_PA && len == 4)
        return &F.magic;
    F.ram_violations++;
    return 0;
}
static const struct { const char *name; long size; } files[] = {
    { "calliope_sram.bin", 34676 }, { "calliope_dram.bin", 2209916 }, { "calliope_iva.bin", 57728 },
};
static long f_file(void *c, const char *path, void *dst, uint32_t cap)
{
    (void)c;
    F.now += 2000;                                          /* a VENDOR read takes time */
    if (strncmp(path, "firmware/", 9))
        return -1;
    for (unsigned f = 0; f < sizeof(files) / sizeof(files[0]); ++f)
        if (!strcmp(path + 9, files[f].name)) {
            if ((F.missing_sram && f == 0) || !dst || files[f].size > (long)cap)
                return -1;
            for (long i = 0; i < files[f].size; ++i)
                ((uint8_t *)dst)[i] = (uint8_t)(i * 131u + f * 17u);
            return files[f].size;
        }
    return -1;
}
static int a_probe(void) { F.amp_calls++; return 0; }
static int a_rcv(int on, int level)
{
    F.amp_calls++;
    F.seq++;
    if (on && level == 0)
        F.amp_on = 1, F.amp_on_seq = F.seq;
    return on && level == 0 ? 0 : -1;
}
static void a_off(void)
{
    F.seq++;
    if (F.amp_on)
        F.amp_off_seq = F.seq;
    F.amp_on = 0;
}
static int c_mic(int on)
{
    F.seq++;
    F.mic_on = on;
    F.mic_calls++;
    return 0;
}
static void c_off(void) { F.codec_offs++; }

static void reset(int pd_on)
{
    uint8_t *rm = F.rmem;
    struct wr *w = F.w;
    size_t capw = F.capw;
    memset(&F, 0, sizeof(F));
    F.rmem = rm, F.w = w, F.capw = capw;
    memset(F.rmem, 0, RMEM_SIZE);
    F.boot_ok = 1;
    F.noise = 1;
    spk_on = rcv_on = 0;
    set(ABOX_SFR + 0x300, 0x90);                           /* SPUM NSRC0 ASRC on, OUT not WDMA: capture must change it */
    set(ABOX_SFR + 0x0, 0x41424F58);
    set(ABOX_SFR + 0x4, 0x10100);
    set(ABOX_SYSMMU + 0x0, 0x4);
    set(RD0, 0x400000);                                     /* as the phone's RDMA0 CTRL0 read after the boot */
    set(ABOX_SFR + 0x510, 0x1000010);                       /* UAIF1 CTRL0 as found on the phone */
    if (pd_on)
        set(ABOX_PMU + 0x4000, 0xF), set(ABOX_PMU + 0x4004, 0xF);
    struct abox_hal h = { 0 };
    h.rd32 = f_rd32, h.wr32 = f_wr32, h.smc = f_smc, h.delay_us = f_delay, h.log = f_log, h.file = f_file;
    h.ram = f_ram, h.now_us = f_now;
    struct cal_probe_ops o = { f_file, a_probe, a_rcv, a_off, c_mic, c_off };
    cal_probe_setup(&h, &o);
    abox_hal_set(&h);
}

static int logged(const char *needle)
{
    for (unsigned i = 0; i < F.nlogs; ++i)
        if (strstr(F.logs[i], needle))
            return 1;
    return 0;
}
static const char *last_line(void)
{
    for (unsigned i = F.nlogs; i--;)
        if (strstr(F.logs[i], "drvprobe: calliope: PASS") || strstr(F.logs[i], "drvprobe: calliope: FAIL"))
            return F.logs[i];
    return "";
}
/* the index of the n-th playback-channel-0 message of `type` from `from` */
static int find_pcm(unsigned from, uint32_t type)
{
    for (unsigned i = from; i < F.nipc; ++i)
        if (F.ipc[i].w[0] == 2 && F.ipc[i].w[3] == 0 && F.ipc[i].w[2] == type)
            return (int)i;
    return -1;
}
/* the same for capture channel 1 (task 1) */
static int find_cap(unsigned from, uint32_t type)
{
    for (unsigned i = from; i < F.nipc; ++i)
        if (F.ipc[i].w[0] == 3 && F.ipc[i].w[1] == 1 && F.ipc[i].w[3] == 1 && F.ipc[i].w[2] == type)
            return (int)i;
    return -1;
}
/* the seq of the first write to pa that sets (on) / clears (!on) bit, after seq from; 0 none */
static uint64_t bit_write(uint64_t pa, uint32_t bit, int on, uint64_t from)
{
    uint32_t was = 0;
    int known = 0;
    for (size_t i = 0; i < F.nw; ++i) {
        if (F.w[i].pa != pa)
            continue;
        int now = !!(F.w[i].v & bit);
        if (F.w[i].seq > from && now == on && (!known || (int)!!(was & bit) != on))
            return F.w[i].seq;
        was = F.w[i].v, known = 1;
    }
    return 0;
}

/* WDMA1's capture: Samsung's message order, its arguments, the routes while it ran, the routes after */
static void check_capture(const char *t, uint32_t period, uint32_t periods)
{
    int o = find_cap(0, 11), b = find_cap(0, 20), p = find_cap(0, 14), pr = find_cap(0, 16), tr = find_cap(0, 17);
    CHECK(o >= 0 && o < b && b < p && p < pr && pr < tr, "%s: capture OPEN %d SET_BUFFER %d HW_PARAMS %d PREPARE %d "
          "TRIGGER %d", t, o, b, p, pr, tr);
    if (o < 0 || b < 0 || p < 0 || tr < 0)
        return;
    CHECK(F.ipc[b].w[4] == 0x92100000u && F.ipc[b].w[5] == period && F.ipc[b].w[6] == periods,
          "%s: capture SET_BUFFER %#x %u %u", t, F.ipc[b].w[4], F.ipc[b].w[5], F.ipc[b].w[6]);
    CHECK(F.ipc[p].w[4] == 48000 && F.ipc[p].w[5] == 16 && F.ipc[p].w[6] == 2 && F.ipc[tr].w[4] == 1,
          "%s: capture HW_PARAMS / TRIGGER 1", t);
    int ts = find_cap((unsigned)tr + 1, 17), hf = find_cap((unsigned)tr + 1, 15), cl = find_cap((unsigned)tr + 1, 12);
    CHECK(ts > tr && F.ipc[ts].w[4] == 0 && hf > ts && cl > hf, "%s: capture stop TRIGGER 0 %d HW_FREE %d CLOSE %d", t,
          ts, hf, cl);
    /* the DMA first, then UAIF0's MIC side; stopped the same way round (One UI's log) */
    uint64_t mic_on = bit_write(ABOX_SFR + 0x500, 2, 1, 0), mic_off = mic_on ? bit_write(ABOX_SFR + 0x500, 2, 0, mic_on) : 0;
    CHECK(mic_on > F.ipc[tr].seq && mic_off > F.ipc[ts].seq && (hf < 0 || mic_off < F.ipc[hf].seq),
          "%s: UAIF0 MIC_EN after TRIGGER 1 (%llu / %llu) and off after TRIGGER 0, before HW_FREE", t,
          (unsigned long long)mic_on, (unsigned long long)F.ipc[tr].seq);
    CHECK((F.w_route1 & 0xF) == 8 && !(F.w_spum0 & 0x90) && (F.w_qch & 4), "%s: while WDMA1 ran: ROUTE1 %#x SPUM0 %#x "
          "QCH_DIS %#x", t, F.w_route1, F.w_spum0, F.w_qch);
    CHECK((reg(ABOX_SFR + 0x44) & 0xF) == 0 && (reg(ABOX_SFR + 0x300) & 0x90) == 0x90 && !(reg(ABOX_SFR + 0x38) & 4) &&
          reg(ABOX_SFR + 0x500) == 0, "%s: after: ROUTE1 %#x SPUM0 %#x QCH_DIS %#x UAIF0 CTRL0 %#x not as found", t,
          reg(ABOX_SFR + 0x44), reg(ABOX_SFR + 0x300), reg(ABOX_SFR + 0x38), reg(ABOX_SFR + 0x500));
    CHECK(!F.w_run && !cal_capping(), "%s: WDMA1 left running", t);
}

/* the probe at one level until it says it is done (or 400 calls) */
static int run(int level)
{
    unsigned w = 0, n = 0;
    while (!drv_calliope_probe(level, &w) && ++n < 400)
        F.now += (uint64_t)w * 1000U;
    return n < 400;
}

/* pd_on: the AUD domain expected after (abox_off powers it down by design; untouched when nothing booted) */
static void off_as_found(const char *t, int pd_on)
{
    CHECK(abox_state() == ABOX_ST_OFF, "%s: abox state %d", t, abox_state());
    CHECK(!(reg(ABOX_PMU + 0x4160) & 1), "%s: ABOX CPU still on", t);
    CHECK((reg(ABOX_PMU + 0x4004) & 0xF) == (pd_on ? 0xFu : 0u), "%s: AUD domain %#x, want %s", t,
          reg(ABOX_PMU + 0x4004), pd_on ? "on" : "off");
    CHECK(!logged("DTZPC restore SMC answered") && !logged("back on"), "%s: the probe powered the domain", t);
    CHECK(!F.r_run, "%s: RDMA0 left running", t);
    CHECK(!F.amp_on, "%s: amp left on", t);
    CHECK(F.violations == 0 && F.ram_violations == 0, "%s: %u / %u writes outside the audio blocks / RAM", t,
          F.violations, F.ram_violations);
    CHECK(!logged("REFUSED"), "%s: a driver refused a write (a bug)", t);
    CHECK(!cal_playing(), "%s: cal still playing", t);
}

/* ---- tests -------------------------------------------------------------------------------------------------------- */
static void test_level0(void)
{
    reset(1);
    CHECK(run(0), "L0: did not finish");
    CHECK(strstr(last_line(), "PASS level 0") && strstr(last_line(), "calliope_sram.bin 34676") &&
          strstr(last_line(), "calliope_dram.bin 2209916") && strstr(last_line(), "calliope_iva.bin 57728"),
          "L0: %s", last_line());
    CHECK(logged("AUD domain on (the boot's)") && logged("IP_INDEX 0x41424f58"), "L0: state line");
    CHECK(F.nw == 0 && F.nsmc == 0, "L0: %zu register writes, %u SMCs (must be read-only)", F.nw, F.nsmc);

    reset(0);
    F.missing_sram = 1;
    CHECK(run(0), "L0 missing: did not finish");
    CHECK(strstr(last_line(), "FAIL level 0: Calliope's firmware is not on VENDOR") &&
          strstr(last_line(), "calliope_sram.bin MISSING"), "L0 missing: %s", last_line());
    CHECK(F.nw == 0 && F.nsmc == 0, "L0 missing: wrote registers");
    CHECK(!logged("IP_INDEX"), "L0: SFR read with the domain off");

    reset(1);
    set(ABOX_PMU + 0x4160, 1);                              /* ABOX's CPU runs (S-Boot): files not read */
    CHECK(run(0) && strstr(last_line(), "PASS level 0: ABOX's CPU runs"), "L0 cpu on: %s", last_line());
    CHECK(F.nw == 0, "L0 cpu on: wrote");
}

static void test_level1(int pd_on)
{
    const char *t = pd_on ? "L1 (domain on)" : "L1 (domain off)";
    reset(pd_on);
    CHECK(run(1), "%s: did not finish", t);
    const char *l = last_line();
    CHECK(strstr(l, "PASS Calliope booted (CRL0 in ") && strstr(l, "RDMA0 192000 B/s (STATUS) to UAIF1") &&
          strstr(l, "snd's 32 x 2048 ring 19") && strstr(l, "; WDMA1 192000 B/s, mic RMS 11"), "%s: %s", t, l);
    CHECK(strlen(l) < 199, "%s: the PASS line is cut (%zu chars)", t, strlen(l));
    check_capture(t, 2048, 32);
    CHECK(F.w_starts == 1 && F.mic_calls == 2 && !F.mic_on && F.codec_offs == 1, "%s: one capture, mic on and off "
          "(%u starts, %u mic calls)", t, F.w_starts, F.mic_calls);
    CHECK(logged("cal: ipc -> PCMCAPTURE (3) task 1 ch 1 SET_BUFFER (msgtype 20; 0x92100000, 0x800, 0x20): taken") &&
          logged("cal: capture: WDMA1 32 x 2048 bytes at IOVA 0x92100000 <- SIFM0 <- NSRC0 <- UAIF0 MIC") &&
          logged("cal: dump 100 ms after capture TRIGGER: WDMA1 0x1") &&
          logged("capture snd's ring (32 x 2048): WDMA1 STATUS moved 192000 B/s over 500 ms") &&
          logged("cal: capture: WDMA1 stopped and closed, UAIF0 and routes as found"), "%s: capture lines", t);
    CHECK(logged("abox: 5/13 SysMMU: extra window 0x92100000 -> 0xfc7a0000 (128 KiB, 4 KiB pages)"),
          "%s: WDMA1's extra window line", t);
    /* while capturing, the dump has no stale RDMA0 line */
    CHECK(!logged("cal: dump 100 ms after capture TRIGGER: RDMA0"), "%s: RDMA0 dumped while only WDMA1 ran", t);
    CHECK(logged("abox: 2/13") && logged("abox: 12/13 ready") && logged("abox: fw: Calliope boot") &&
          logged("1/4 boot: Calliope said BOOT_DONE, version CRL0"), "%s: abox step / BOOT_DONE / fw lines", t);
    CHECK(logged("abox: 5/13 SysMMU: extra window 0x91000000 -> 0xfc780000 (128 KiB, 4 KiB pages)"),
          "%s: extra window line", t);
    CHECK(logged("Calliope sent 49 period pointers = 188160 B/s"), "%s: pointer rate", t);
    CHECK(F.r_starts == 2, "%s: RDMA0 started %u times (want 2)", t, F.r_starts);
    /* run 6's diagnostics: settle, every PCM message with its fields, Calliope's messages raw, the dumps, its log */
    CHECK(logged("1/4 boot: 300 ms after BOOT_DONE taken"), "%s: settle line", t);
    CHECK(logged("cal: ipc -> PCMPLAYBACK (2) task 0 ch 0 SET_BUFFER (msgtype 20; 0x91000000, 0xf00, 0x4): taken") &&
          logged("cal: ipc -> PCMPLAYBACK (2) task 0 ch 0 OPEN (msgtype 11; 0x0, 0x0, 0x0): taken") &&
          logged("cal: ipc -> PCMPLAYBACK (2) task 0 ch 0 HW_PARAMS (msgtype 14; 0xbb80, 0x10, 0x2): taken") &&
          logged("cal: ipc -> PCMPLAYBACK (2) task 0 ch 0 TRIGGER (msgtype 17; 0x1, 0x0, 0x0): taken") &&
          logged("cal: ipc -> PCMPLAYBACK (2) task 0 ch 0 CLOSE (msgtype 12; 0x0, 0x0, 0x0): taken"), "%s: ipc trace", t);
    CHECK(logged("abox: rx: 0x2 0x0 0x12 0x0 0x91000f00"), "%s: raw rx of the first pointer", t);
    CHECK(logged("cal: dump 100 ms after TRIGGER: RDMA0 0x400001") && logged("cal: dump end of the phase"), "%s: dumps",
          t);
    CHECK(logged("abox: fw: pcm 17"), "%s: Calliope's log after the stream start", t);

    /* Samsung's order, both phases */
    int o = find_pcm(0, 11), b = find_pcm(0, 20), p = find_pcm(0, 14), pr = find_pcm(0, 16), tr = find_pcm(0, 17);
    CHECK(o >= 0 && o < b && b < p && p < pr && pr < tr, "%s: OPEN %d SET_BUFFER %d HW_PARAMS %d PREPARE %d TRIGGER %d",
          t, o, b, p, pr, tr);
    if (b >= 0 && p >= 0 && tr >= 0) {
        CHECK(F.ipc[b].w[4] == 0x91000000u && F.ipc[b].w[5] == 3840 && F.ipc[b].w[6] == 4, "%s: SET_BUFFER args", t);
        CHECK(F.ipc[p].w[4] == 48000 && F.ipc[p].w[5] == 16 && F.ipc[p].w[6] == 2, "%s: HW_PARAMS args", t);
        CHECK(F.ipc[tr].w[4] == 1 && F.ipc[tr].w[1] == 0, "%s: TRIGGER 1 / task id", t);
        int ts = find_pcm((unsigned)tr + 1, 17), hf = find_pcm((unsigned)tr + 1, 15), cl = find_pcm((unsigned)tr + 1, 12);
        CHECK(ts > tr && F.ipc[ts].w[4] == 0 && hf > ts && cl > hf, "%s: stop order TRIGGER 0 %d HW_FREE %d CLOSE %d", t,
              ts, hf, cl);
        CHECK(F.ipc[ts].at - F.ipc[tr].at >= 1000000, "%s: measured less than 1 s", t);
        int b2 = find_pcm((unsigned)cl + 1, 20);
        CHECK(b2 > cl && F.ipc[b2].w[5] == 2048 && F.ipc[b2].w[6] == 32, "%s: snd's geometry second", t);
        /* UAIF1 SPK_EN on before TRIGGER 1 (playback POST_PRE: the UAIF first) */
        uint64_t spk = 0;
        for (size_t i = 0; i < F.nw && !spk; ++i)
            if (F.w[i].pa == ABOX_SFR + 0x510 && (F.w[i].v & 1))
                spk = F.w[i].seq;
        CHECK(spk && spk < F.ipc[tr].seq, "%s: UAIF1 SPK_EN before TRIGGER", t);
    }
    /* the page tables: L1 0x910 -> the second L2 table -> the ring's pages */
    uint32_t *l1 = (uint32_t *)(void *)(F.rmem + ABOX_RM_PT), *l2 = (uint32_t *)(void *)(F.rmem + ABOX_RM_PT_L2 + 0x400);
    CHECK(l1[0x910] == (uint32_t)(((RMEM_BASE + ABOX_RM_PT_L2 + 0x400) >> 4) | 1), "%s: L1[0x910] %#x", t, l1[0x910]);
    CHECK(l2[0] == (uint32_t)(((RMEM_BASE + CAL_RM_PLAY) >> 4) | 2) &&
          l2[31] == (uint32_t)(((RMEM_BASE + CAL_RM_PLAY + 31 * 4096) >> 4) | 2) && l2[32] == 0, "%s: L2 pages", t);
    CHECK(l1[0x914] == (uint32_t)(((RMEM_BASE + ABOX_RM_RDMA4) >> 4) | 2), "%s: the call's RDMA4 window kept", t);
    CHECK(l1[0x922] == (uint32_t)(((RMEM_BASE + ABOX_RM_WDMA2) >> 4) | 2), "%s: the call's WDMA2 window kept", t);
    uint32_t *l2c = (uint32_t *)(void *)(F.rmem + ABOX_RM_PT_L2 + 0x800);
    CHECK(l1[0x921] == (uint32_t)(((RMEM_BASE + ABOX_RM_PT_L2 + 0x800) >> 4) | 1) &&
          l2c[0] == (uint32_t)(((RMEM_BASE + CAL_RM_CAP) >> 4) | 2) &&
          l2c[31] == (uint32_t)(((RMEM_BASE + CAL_RM_CAP + 31 * 4096) >> 4) | 2) && l2c[32] == 0,
          "%s: WDMA1's L1[0x921] %#x / L2 pages", t, l1[0x921]);
    /* routes and UAIF1 as found */
    CHECK((reg(ABOX_SFR + 0x200) & 0xF) == 0 && (reg(ABOX_SFR + 0x40) & 0xF0) == 0 && reg(ABOX_SFR + 0x510) == 0x1000010,
          "%s: SPUS0 %#x ROUTE0 %#x UAIF1 CTRL0 %#x not as found", t, reg(ABOX_SFR + 0x200), reg(ABOX_SFR + 0x40),
          reg(ABOX_SFR + 0x510));
    off_as_found(t, 0);
    CHECK(F.smc[F.nsmc - 1].a1 == 0, "%s: the last SMC is not the TZPC save", t);
    CHECK(logged(pd_on ? "AUD domain off (it was on from the boot; abox_off powers it down by design)"
                       : "4/4 off: ABOX CPU off, AUD domain off"), "%s: off line", t);

    /* the extra window is gone: a later boot (a call) does not map it */
    reset(0);
    CHECK(abox_boot() == 0, "%s: plain boot", t);
    CHECK(((uint32_t *)(void *)(F.rmem + ABOX_RM_PT))[0x910] == 0 &&
          ((uint32_t *)(void *)(F.rmem + ABOX_RM_PT))[0x921] == 0, "%s: extra window left mapped", t);
    abox_off();
}

static void test_level2(void)
{
    reset(1);
    CHECK(run(2), "L2: did not finish");
    CHECK(strstr(last_line(), "PASS earpiece tone: heard by the mic: yes (1 kHz ") &&
          strstr(last_line(), " dB over baseline); Calliope CRL0, RDMA0 192000 B/s (STATUS), WDMA1 192000 B/s"),
          "L2: %s", last_line());
    CHECK(logged("drvprobe: calliope: loopback: 1 kHz ") && logged("): heard yes"), "L2: loopback line");
    int tr = find_pcm(0, 17);
    int ts = tr >= 0 ? find_pcm((unsigned)tr + 1, 17) : -1;
    CHECK(tr >= 0 && ts > tr && F.amp_on_seq > F.ipc[tr].seq && F.amp_off_seq && F.amp_off_seq < F.ipc[ts].seq,
          "L2: amp on after TRIGGER 1 and off before TRIGGER 0");
    /* the mic's baseline first, then the tone while it records; the capture stops after the tone */
    int ctr = find_cap(0, 17), cts = ctr >= 0 ? find_cap((unsigned)ctr + 1, 17) : -1;
    CHECK(ctr >= 0 && ctr < tr && cts > ts && F.ipc[tr].at - F.ipc[ctr].at >= 1100000, "L2: capture TRIGGER %d, "
          "baseline >= 1.1 s before the tone's TRIGGER %d, capture stopped %d after it %d", ctr, tr, cts, ts);
    check_capture("L2", 2048, 32);
    CHECK(F.mic_calls == 2 && !F.mic_on && F.codec_offs == 1, "L2: the codec's mic on, off, codec off (%u, %u)",
          F.mic_calls, F.codec_offs);
    CHECK(F.ipc[ts].at - F.ipc[tr].at >= 2000000, "L2: tone shorter than 2 s");
    CHECK(find_pcm(0, 20) >= 0 && F.ipc[find_pcm(0, 20)].w[5] == 3840, "L2: One UI's ring");
    const int16_t *s = (const int16_t *)(const void *)(F.rmem + CAL_RM_PLAY);
    int peak = 0;
    long sum = 0;
    for (int i = 0; i < 3840 * 2; ++i) {
        peak = abs(s[i]) > peak ? abs(s[i]) : peak;
        sum += s[i];
    }
    CHECK(peak >= 2000 && peak <= 2064 && s[2 * 12] == 2064 && s[2 * 12 + 1] == 2064 && s[2 * 36] == -2064 && sum == 0,
          "L2: tone peak %d sum %ld (-24 dBFS 1 kHz)", peak, sum);
    CHECK(F.rmem[CAL_RM_PLAY + 15360] == 0, "L2: guard zeroes");
    off_as_found("L2", 0);

    /* the mic hears nothing: still PASS (the earpiece is quiet and far from the mic), but it says "no" */
    reset(1);
    F.deaf = 1;
    CHECK(run(2), "L2 deaf: did not finish");
    CHECK(strstr(last_line(), "PASS earpiece tone: heard by the mic: no (1 kHz "), "L2 deaf: %s", last_line());
    off_as_found("L2 deaf", 0);

    /* WDMA1 does not move: FAIL with the capture first */
    reset(1);
    F.wdma_dead = 1;
    CHECK(run(2), "L2 wdma dead: did not finish");
    CHECK(strstr(last_line(), "FAIL capture: WDMA1 did not move through Calliope (UAIF0 mic): WDMA1 0 B/s; RDMA0 "
                              "192000 B/s (STATUS) ok"), "L2 wdma dead: %s", last_line());
    off_as_found("L2 wdma dead", 0);

    /* abort while the tone plays and the mic records: the amp, both DMAs and the mic off */
    reset(1);
    unsigned w = 0, n = 0;
    while (!F.amp_on && n++ < 100)
        (void)drv_calliope_probe(2, &w);
    CHECK(F.amp_on && F.r_run && F.w_run && F.mic_on, "L2 abort: tone and capture should be running");
    drv_calliope_abort();
    off_as_found("L2 abort", 0);
    CHECK(!F.w_run && !F.mic_on && !cal_capping() && (reg(ABOX_SFR + 0x500) & 2) == 0 &&
          (reg(ABOX_SFR + 0x44) & 0xF) == 0, "L2 abort: WDMA1 / mic / UAIF0 MIC / NSRC0 left on");
}

static void test_failures(void)
{
    /* no BOOT_DONE */
    reset(1);
    F.boot_ok = 0;
    CHECK(run(1), "boot fail: did not finish");
    CHECK(strstr(last_line(), "FAIL 1/4 boot: Calliope did not boot: 12/13 ready: no BOOT_DONE within 3000 ms") &&
          strstr(last_line(), "everything off"), "boot fail: %s", last_line());
    CHECK(F.r_starts == 0, "boot fail: RDMA started");
    off_as_found("boot fail", 0);

    /* missing firmware */
    reset(0);
    F.missing_sram = 1;
    CHECK(run(1) && strstr(last_line(), "FAIL 1/4 boot") && strstr(last_line(), "calliope_sram.bin"), "no fw: %s",
          last_line());
    off_as_found("no fw", 0);

    /* Calliope does not take PCM messages */
    reset(1);
    F.pcm_dead = 1;
    CHECK(run(1), "pcm dead: did not finish");
    CHECK(strstr(last_line(), "FAIL 2/4 stream: RDMA0 did not start through Calliope"), "pcm dead: %s", last_line());
    off_as_found("pcm dead", 0);

    /* RDMA0 never moves and no pointers */
    reset(1);
    F.rdma_dead = 1;
    CHECK(run(1), "rdma dead: did not finish");
    CHECK(strstr(last_line(), "FAIL 3/4 measure: RDMA0 did not move through Calliope to UAIF1; to UAIF0 (One UI's "
                              "boot route): 0 B/s (pointers)"), "rdma dead: %s", last_line());
    CHECK(F.r_starts == 2, "rdma dead: UAIF1 then UAIF0 tried (%u starts)", F.r_starts);
    off_as_found("rdma dead", 0);

    /* WDMA1 never moves: both capture geometries tried, FAIL with the capture first */
    reset(1);
    F.wdma_dead = 1;
    CHECK(run(1), "wdma dead: did not finish");
    CHECK(strstr(last_line(), "FAIL capture: WDMA1 did not move through Calliope (UAIF0 mic): WDMA1 0 B/s, 4 x 1920 "
                              "ring 0 B/s; RDMA0 192000 B/s (STATUS) ok"), "wdma dead: %s", last_line());
    CHECK(F.w_starts == 2 && find_cap(0, 20) >= 0 && F.ipc[find_cap((unsigned)find_cap(0, 20) + 1, 20)].w[5] == 1920,
          "wdma dead: snd's then One UI's capture ring (%u starts)", F.w_starts);
    CHECK(strlen(last_line()) - strlen("drvprobe: calliope: ") > 72 &&
          !strncmp(last_line() + strlen("drvprobe: calliope: FAIL "), "capture: WDMA1 did not move", 27),
          "wdma dead: the key fact first");
    off_as_found("wdma dead", 0);

    /* RDMA0 moves only on One UI's boot route (SIFS0 -> UAIF0): phase 2 finds it */
    reset(1);
    F.only_uaif0 = 1;
    CHECK(run(1), "uaif0 only: did not finish");
    CHECK(strstr(last_line(), "FAIL 3/4 measure: RDMA0 did not move through Calliope to UAIF1; to UAIF0 (One UI's boot "
                              "route) it DID: 192000 B/s (STATUS)"), "uaif0 only: %s", last_line());
    CHECK(F.r_starts == 1, "uaif0 only: %u starts", F.r_starts);
    off_as_found("uaif0 only", 0);

    /* both extra slots taken: the ring cannot be mapped -> nothing booted */
    reset(1);
    CHECK(abox_map_extra(0x95000000u, RMEM_BASE + 0x1510000, 0x1000) == 0 &&
          abox_map_extra(0x96000000u, RMEM_BASE + 0x1511000, 0x1000) == 0, "slots: fill");
    CHECK(run(1) && strstr(last_line(), "FAIL RDMA0's ring could not be added"), "slots: %s", last_line());
    CHECK(F.nw == 0, "slots: hardware touched");
    (void)abox_map_extra(0x95000000u, 0, 0);
    (void)abox_map_extra(0x96000000u, 0, 0);
    off_as_found("slots", 1);

    /* ABOX busy (a call has it): not touched */
    reset(1);
    CHECK(abox_boot() == 0, "busy: boot");
    size_t nw = F.nw;
    CHECK(run(1) && strstr(last_line(), "FAIL ABOX is in use already"), "busy: %s", last_line());
    CHECK(F.nw == nw && abox_state() == ABOX_ST_READY, "busy: touched the running ABOX");
    abox_off();

    /* abort in the middle of the measurement */
    reset(1);
    unsigned w = 0;
    for (int i = 0; i < 6; ++i)
        (void)drv_calliope_probe(1, &w);
    CHECK(F.r_run, "abort: RDMA0 should be running by now");
    drv_calliope_abort();
    CHECK(logged("aborted: putting everything back"), "abort: line");
    off_as_found("abort", 0);
    CHECK((reg(ABOX_SFR + 0x40) & 0xF0) == 0, "abort: route left");
    drv_calliope_abort();                                   /* twice is safe */
    off_as_found("abort twice", 0);
}

static void test_hooks(void)
{
    reset(0);
    CHECK(abox_map_extra(0x91000001u, RMEM_BASE + 0x1580000, 0x1000) == ABOX_ECFG, "map: iova alignment");
    CHECK(abox_map_extra(0x91000000u, RMEM_BASE + 0x1580100, 0x1000) == ABOX_ECFG, "map: pa alignment");
    CHECK(abox_map_extra(0x91000000u, RMEM_BASE + 0x1500000, 0x1000) == ABOX_ECFG, "map: the page tables");
    CHECK(abox_map_extra(0x91000000u, RMEM_BASE + 0x0, 0x1000) == ABOX_ECFG, "map: the firmware");
    CHECK(abox_map_extra(0x91000000u, RMEM_BASE + 0x15F0000, 0x20000) == ABOX_ECFG, "map: into the dump window");
    CHECK(abox_map_extra(0x91000000u, 0x80000000ULL, 0x1000) == ABOX_ECFG, "map: outside abox_rmem");
    /* a clash with our own window is skipped at boot */
    CHECK(abox_map_extra(0x91400000u, RMEM_BASE + 0x1580000, 0x1000) == 0, "map: clash accepted until boot");
    CHECK(abox_boot() == 0, "map: boot");
    CHECK(logged("extra window 0x91400000 clashes"), "map: clash not logged");
    CHECK(((uint32_t *)(void *)(F.rmem + ABOX_RM_PT))[0x914] == (uint32_t)(((RMEM_BASE + ABOX_RM_RDMA4) >> 4) | 2),
          "map: clash replaced RDMA4");
    CHECK(abox_map_extra(0x91000000u, RMEM_BASE + 0x1580000, 0x1000) == ABOX_ESTATE, "map: while running");
    CHECK(abox_pcm_msg(0, 4, ABOX_PCM_OPEN, 0, 0, 0) == ABOX_ESTATE, "pcm: the call's RDMA4");
    CHECK(abox_pcm_msg(1, 2, ABOX_PCM_OPEN, 0, 0, 0) == ABOX_ESTATE, "pcm: the call's WDMA2");
    CHECK(abox_pcm_msg(0, 8, ABOX_PCM_OPEN, 0, 0, 0) == ABOX_ESTATE, "pcm: id 8");
    CHECK(cal_play_start(CAL_UAIF1, 100, 4) == ABOX_ECFG && !cal_playing(), "cal: bad period");
    CHECK(cal_play_start(CAL_UAIF1, 4096, 32) == ABOX_ECFG, "cal: ring too big");
    abox_off();
    (void)abox_map_extra(0x91400000u, 0, 0);
    CHECK(abox_pcm_msg(0, 0, ABOX_PCM_OPEN, 0, 0, 0) == ABOX_ESTATE, "pcm: while off");
    CHECK(cal_play_start(CAL_UAIF1, 3840, 4) == ABOX_ESTATE, "cal: while off");
    CHECK(cal_play_pos(0) == CAL_NOPOS && cal_play_buf() == 0, "cal: pos / buf while off");
    /* capture's refusals */
    CHECK(cal_cap_start(CAL_UAIF0, 2048, 32) == ABOX_ESTATE && !cal_capping(), "cap: while off");
    CHECK(cal_cap_pos(0) == CAL_NOPOS && cal_cap_buf() == 0, "cap: pos / buf while off");
    /* cal_map needs both slots: with one taken it fails and leaves nothing of its own mapped */
    reset(0);
    CHECK(abox_map_extra(0x95000000u, RMEM_BASE + 0x1510000, 0x1000) == 0, "map: one slot taken");
    CHECK(cal_map() == ABOX_ECFG, "map: cal_map with one free slot");
    CHECK(abox_map_extra(0x96000000u, RMEM_BASE + 0x1511000, 0x1000) == 0, "map: the RDMA0 slot was given back");
    (void)abox_map_extra(0x95000000u, 0, 0);
    (void)abox_map_extra(0x96000000u, 0, 0);
    CHECK(cal_map() == 0, "map: both free");
    CHECK(abox_boot() == 0, "cap: boot");
    CHECK(cal_cap_start(CAL_UAIF1, 2048, 32) == ABOX_ECFG && cal_cap_start(CAL_UAIF0, 100, 4) == ABOX_ECFG,
          "cap: UAIF1 / bad period refused");
    CHECK(!F.w_open && reg(ABOX_SFR + 0x38) == 0 && (reg(ABOX_SFR + 0x44) & 0xF) == 0, "cap: a refusal touched things");
    /* playback and capture together: the UAIF / DRAM found values come back when the last one stops */
    set(ABOX_SFR + 0x10, 0);
    CHECK(cal_play_start(CAL_UAIF0, 2048, 32) == 0 && cal_cap_start(CAL_UAIF0, 2048, 32) == 0, "both: start");
    CHECK(F.r_run && F.w_run && (reg(ABOX_SFR + 0x500) & 3) == 3, "both: RDMA0, WDMA1, UAIF0 SPK and MIC");
    cal_play_stop();
    CHECK(F.w_run && (reg(ABOX_SFR + 0x500) & 0x16) == 0x16 && (reg(ABOX_SFR + 0x10) & 1) &&
          (reg(ABOX_SFR + 0x504) & 0x1FFFFFFF) == 0x0984F000, "both: playback's stop kept the capture's UAIF0 / DRAM");
    cal_cap_stop();
    CHECK(reg(ABOX_SFR + 0x500) == 0 && reg(ABOX_SFR + 0x504) == 0 && !(reg(ABOX_SFR + 0x10) & 1),
          "both: UAIF0 CTRL0 %#x CTRL1 %#x SYSPOWER %#x not as found", reg(ABOX_SFR + 0x500), reg(ABOX_SFR + 0x504),
          reg(ABOX_SFR + 0x10));
    /* the other way round */
    CHECK(cal_cap_start(CAL_UAIF0, 2048, 32) == 0 && cal_play_start(CAL_UAIF0, 2048, 32) == 0, "both 2: start");
    cal_cap_stop();
    CHECK(F.r_run && (reg(ABOX_SFR + 0x500) & 0x17) == 0x15 && (reg(ABOX_SFR + 0x10) & 1), "both 2: kept for playback");
    cal_play_stop();
    CHECK(reg(ABOX_SFR + 0x500) == 0 && !(reg(ABOX_SFR + 0x10) & 1), "both 2: not as found");
    /* a call owns UAIF0 MIC: capture refused */
    CHECK(abox_call(1) == 0, "cap: call on");
    uint32_t r1 = reg(ABOX_SFR + 0x44);
    CHECK(cal_cap_start(CAL_UAIF0, 2048, 32) == ABOX_ESTATE && reg(ABOX_SFR + 0x44) == r1, "cap: refused in a call");
    abox_call(0);
    abox_off();
    cal_unmap();
    CHECK(F.violations == 0 && !logged("REFUSED"), "cap hooks: stray writes");
}

/* ---- snd.c with the Calliope engine ------------------------------------------------------------------------------ */
static void s_log(const char *s) { f_log(0, s); }
static uint64_t s_now(void) { return F.now; }
static int s_owns(void)
{
    int s = abox_state();
    return s == ABOX_ST_BOOTING || s == ABOX_ST_READY || s == ABOX_ST_CALL;
}
static char aev[64][16];                                   /* the amp calls, in order */
static uint64_t spk_t0, rcv_t0;
static unsigned naev, amp_calls_snd;
static void aev_add(const char *e)
{
    if (naev < 64)
        snprintf(aev[naev++], sizeof aev[0], "%s", e);
    amp_calls_snd++;
}
static int aev_at(const char *e, int from)
{
    for (unsigned i = (unsigned)(from < 0 ? 0 : from); i < naev; ++i)
        if (!strcmp(aev[i], e))
            return (int)i;
    return -1;
}
static int s_spk(int on, int level)
{
    char b[16];
    snprintf(b, sizeof b, "spk(%d,%d)", on, level);
    aev_add(b);
    if (on && !spk_on)
        spk_t0 = F.now;
    spk_on = (unsigned)on;
    return 0;
}
static int s_rcv(int on, int level)
{
    char b[16];
    snprintf(b, sizeof b, "rcv(%d,%d)", on, level);
    aev_add(b);
    if (on && !rcv_on)
        rcv_t0 = F.now;
    rcv_on = (unsigned)on;
    return 0;
}
static void s_off(void)
{
    aev_add("off");
    spk_on = rcv_on = 0;
}
static void polls(unsigned ms)
{
    for (unsigned i = 0; i < ms / 5U; ++i) {
        snd_poll(F.now);
        F.now += 5000;
    }
}
static int ring_nonzero(void)
{
    for (unsigned i = 0; i < 0x10000; ++i)
        if (F.rmem[CAL_RM_PLAY + i])
            return 1;
    return 0;
}

static void test_snd_engine(void)
{
    reset(0);
    struct abox_hal h = { 0 };
    h.rd32 = f_rd32, h.wr32 = f_wr32, h.smc = f_smc, h.delay_us = f_delay, h.log = f_log, h.file = f_file;
    h.ram = f_ram, h.now_us = f_now;
    struct snd_ops o;
    memset(&o, 0, sizeof o);
    o.log = s_log, o.now_us = s_now, o.call_owns_abox = s_owns, o.amp_speaker = s_spk, o.amp_receiver = s_rcv;
    o.amp_off = s_off, o.use_calliope = 1;
    CHECK(snd_init(&o, &h) == 0, "snd: init");

    /* a tone boots Calliope and plays through RDMA0 in snd's geometry */
    CHECK(snd_tone(SND_TONE_TEST) == 0, "snd: tone");
    polls(300);
    CHECK(logged("engine: Calliope booted for media"), "snd: Calliope not booted");
    CHECK(F.r_starts == 1 && F.r_run, "snd: RDMA0 not running through Calliope");
    int b = find_pcm(0, 20);
    CHECK(b >= 0 && F.ipc[b].w[5] == 2048 && F.ipc[b].w[6] == 32, "snd: snd's ring geometry");
    CHECK(ring_nonzero() && spk_on, "snd: the tone is not in the ring / speaker off");
    struct snd_stats ss;
    snd_stats(&ss);
    CHECK(ss.periods > 20 && ss.xruns <= 1, "snd: %u periods mixed, %u xruns", ss.periods, ss.xruns);
    CHECK(!logged("does not move"), "snd: stall reported");

    /* silence: the stream stops after 2 s, Calliope 30 s later */
    snd_tone_stop();
    polls(2600);
    CHECK(!F.r_run && !cal_playing() && abox_state() == ABOX_ST_READY && !spk_on, "snd: stream stop, Calliope kept");
    CHECK(snd_busy(), "snd: not polled while Calliope is booted");
    polls(31000);
    CHECK(abox_state() == ABOX_ST_OFF && logged("Calliope off (30 s without media)"), "snd: Calliope not off");
    CHECK(!(reg(ABOX_PMU + 0x4160) & 1) && (reg(ABOX_PMU + 0x4004) & 0xF) == 0, "snd: CPU / domain left on");

    /* a call: snd_suspend_for_call leaves Calliope booted for the modem, which takes it over */
    CHECK(snd_tone(SND_TONE_TEST) == 0, "snd: tone 2");
    polls(200);
    CHECK(F.r_run, "snd: second start");
    snd_suspend_for_call();
    CHECK(!F.r_run && abox_state() == ABOX_ST_READY, "snd: suspend stopped the stream and kept Calliope");
    CHECK(abox_boot() == 0 && logged("abox: boot: Calliope already runs"), "snd: the modem's boot finds it");
    CHECK(snd_tone(SND_TONE_RING) == 0, "snd: tone while suspended");
    polls(200);
    CHECK(!F.r_run, "snd: played during the call");
    snd_resume_after_call();
    polls(200);
    CHECK(F.r_run && logged("media plays through it"), "snd: no media through the modem's Calliope after the call");
    /* the modem stops (audio_stop -> abox_off) under the playing stream */
    abox_off();
    polls(50);
    CHECK(logged("Calliope stopped while media played: stream dropped"), "snd: drop not noticed");
    polls(2500);                                            /* the retry boots its own */
    CHECK(F.r_run && abox_state() == ABOX_ST_READY, "snd: restart after the modem's off");
    /* a call without snd_suspend_for_call */
    CHECK(abox_call(1) == 0, "snd: call on");
    polls(20);
    CHECK(logged("a call took Calliope while media played") && !F.r_run && !cal_playing(), "snd: missed suspend");
    snd_tone_stop();
    abox_call(0);
    abox_off();
    polls(100);
    CHECK(abox_state() == ABOX_ST_OFF && F.violations == 0 && !logged("REFUSED"), "snd: end state");
    drv_audio_abort();
}

/* goertzel.h on its own: a 1 kHz sine is heard over noise, noise and a 700 Hz tone are not */
static void test_goertzel(void)
{
    struct gz base, tone, other;
    struct gz_verdict v;
    uint32_t n = 1;
    gz_reset(&base), gz_reset(&tone), gz_reset(&other);
    for (unsigned i = 0; i < 48000; ++i) {
        n = n * 1103515245u + 12345u;
        int32_t noise = (int32_t)((n >> 16) % 401u) - 200;
        gz_put(&base, noise);
        gz_put(&tone, noise + (int32_t)lround(300.0 * sin(2.0 * M_PI * 1000.0 * i / 48000.0)));
        gz_put(&other, noise + (int32_t)lround(3000.0 * sin(2.0 * M_PI * 700.0 * i / 48000.0)));
    }
    CHECK(base.blocks == 50 && gz_rms(&base) >= 110 && gz_rms(&base) <= 122, "gz: blocks %u RMS %u", base.blocks,
          gz_rms(&base));
    gz_judge(&tone, &base, &v);
    CHECK(v.heard && v.over_base > 200 && v.over_nb > 200, "gz: 1 kHz at 300 over noise 200: %d / %d tenths", v.over_base,
          v.over_nb);
    gz_judge(&base, &base, &v);
    CHECK(!v.heard && v.over_base == 0, "gz: noise against itself");
    gz_judge(&other, &base, &v);
    CHECK(!v.heard && v.over_nb < 0, "gz: a 700 Hz tone is not 1 kHz (%d / %d)", v.over_base, v.over_nb);
    CHECK(gz_db10(100, 1) == 200 && gz_db10(1, 100) == -200 && gz_db10(2, 1) == 30 && gz_db10(0, 0) == 0,
          "gz: dB %d %d %d", gz_db10(100, 1), gz_db10(1, 100), gz_db10(2, 1));
}

/* snd with the Calliope engine and a mic */
static void snd_setup(void)
{
    reset(0);
    naev = amp_calls_snd = 0;
    static struct abox_hal h;
    memset(&h, 0, sizeof h);
    h.rd32 = f_rd32, h.wr32 = f_wr32, h.smc = f_smc, h.delay_us = f_delay, h.log = f_log, h.file = f_file;
    h.ram = f_ram, h.now_us = f_now;
    struct snd_ops o;
    memset(&o, 0, sizeof o);
    o.log = s_log, o.now_us = s_now, o.call_owns_abox = s_owns, o.amp_speaker = s_spk, o.amp_receiver = s_rcv;
    o.amp_off = s_off, o.codec_mic = c_mic, o.codec_off = c_off, o.use_calliope = 1;
    CHECK(snd_init(&o, &h) == 0, "snd: init");
}
/* poll for ms, reading the mic into g (if given) after skip samples */
static unsigned polls_mic(unsigned ms, struct gz *g, unsigned skip)
{
    int16_t b[512];
    unsigned got = 0, k;
    for (unsigned i = 0; i < ms / 5U; ++i) {
        snd_poll(F.now);
        while ((k = snd_cap_read(b, 512)) > 0)
            for (unsigned j = 0; j < k; ++j, ++got)
                if (g && got >= skip)
                    gz_put(g, b[j]);
        F.now += 5000;
    }
    return got;
}

static void test_snd_capture(void)
{
    struct gz base, tone;
    struct gz_verdict v;
    snd_setup();
    /* the mic alone boots Calliope (300 ms settle) and records through WDMA1 */
    CHECK(snd_cap_open() == 0, "snd cap: open");
    CHECK(logged("engine: Calliope booted for media") && logged("(with 300 ms for its post-boot messages)"),
          "snd cap: Calliope booted with the settle");
    CHECK(F.w_run && F.mic_on && cal_capping() && logged("capture: the main mic through Calliope (WDMA1)"),
          "snd cap: WDMA1 / mic not on");
    CHECK(snd_busy(), "snd cap: not busy while capturing");
    gz_reset(&base);
    unsigned got = polls_mic(500, &base, 0);
    CHECK(got >= 23500 && got <= 24200, "snd cap: 0.5 s of samples (%u)", got);
    CHECK(snd_cap_level() >= 80 && snd_cap_level() <= 160, "snd cap: noise level %u", snd_cap_level());
    /* the loudspeaker plays a tone while the mic records: one Calliope, both DMAs */
    CHECK(snd_tone(SND_TONE_TEST) == 0, "snd cap: tone");
    (void)polls_mic(200, 0, 0);
    CHECK(F.r_run && F.w_run && spk_on, "snd cap: RDMA0 %d and WDMA1 %d together, speaker %u", F.r_run, F.w_run, spk_on);
    gz_reset(&tone);
    (void)polls_mic(500, &tone, 0);
    gz_judge(&tone, &base, &v);
    CHECK(v.heard, "snd cap: the fake room's 1 kHz heard (%d / %d)", v.over_base, v.over_nb);
    CHECK(F.r_starts == 1 && F.w_starts == 1, "snd cap: one start each (%u / %u)", F.r_starts, F.w_starts);
    /* the mic closes while the tone plays: the stream goes on, UAIF0 back as found */
    snd_cap_close();
    CHECK(!F.w_run && F.r_run && !F.mic_on && F.codec_offs == 1 && abox_state() == ABOX_ST_READY,
          "snd cap: close kept playback / Calliope");
    CHECK(reg(ABOX_SFR + 0x500) == 0 && (reg(ABOX_SFR + 0x44) & 0xF) == 0 && (reg(ABOX_SFR + 0x10) & 1),
          "snd cap: UAIF0 %#x NSRC0 %#x not as found, or DRAM off under the playback", reg(ABOX_SFR + 0x500),
          reg(ABOX_SFR + 0x44));
    snd_tone_stop();
    polls(2600);
    CHECK(!F.r_run && abox_state() == ABOX_ST_READY, "snd cap: stream stopped, Calliope kept");
    polls(31000);
    CHECK(abox_state() == ABOX_ST_OFF, "snd cap: Calliope not off 30 s after");
    /* the mic alone: Calliope off 30 s after it closes */
    CHECK(snd_cap_open() == 0, "snd cap: open 2");
    (void)polls_mic(100, 0, 0);
    snd_cap_close();
    polls(1000);
    CHECK(abox_state() == ABOX_ST_READY, "snd cap: Calliope off at once after the mic");
    polls(30500);
    CHECK(abox_state() == ABOX_ST_OFF && !(reg(ABOX_PMU + 0x4004) & 0xF), "snd cap: Calliope / domain not off");
    /* a call: the mic stops before it, refused during it; then the modem's Calliope carries it */
    CHECK(snd_cap_open() == 0, "snd cap: open 3");
    (void)polls_mic(50, 0, 0);
    snd_suspend_for_call();
    CHECK(!F.w_run && !F.mic_on && !cal_capping() && abox_state() == ABOX_ST_READY &&
          logged("call: the mic's capture stopped; Calliope left running for the call"), "snd cap: suspend");
    CHECK(snd_cap_open() == SND_EBUSY, "snd cap: opened while suspended");
    CHECK(abox_boot() == 0 && abox_call(1) == 0, "snd cap: the modem's call");
    CHECK(snd_cap_open() == SND_EBUSY, "snd cap: opened during the call");
    abox_call(0);
    snd_resume_after_call();
    CHECK(snd_cap_open() == 0 && F.w_run && !logged("engine: Calliope booted for media in 0"), "snd cap: on the "
          "modem's Calliope");
    snd_cap_close();
    polls(31000);
    CHECK(abox_state() == ABOX_ST_READY, "snd cap: snd switched off the modem's Calliope");
    /* a call without snd_suspend_for_call */
    CHECK(snd_cap_open() == 0, "snd cap: open 4");
    CHECK(abox_call(1) == 0, "snd cap: call 2");
    polls(20);
    CHECK(logged("a call took Calliope while the mic recorded") && !cal_capping() && !F.w_run, "snd cap: missed "
          "suspend");
    CHECK(F.mic_on, "snd cap: the call's codec mic switched off by snd");
    abox_call(0);
    abox_off();
    /* Calliope stops under the mic */
    CHECK(snd_cap_open() == 0, "snd cap: open 5");
    (void)polls_mic(50, 0, 0);
    abox_off();
    polls(20);
    CHECK(logged("Calliope stopped while the mic recorded: capture dropped") && !cal_capping() && !F.mic_on,
          "snd cap: drop not noticed");
    CHECK(!snd_busy() || abox_state() == ABOX_ST_OFF, "snd cap: busy");
    CHECK(F.violations == 0 && F.ram_violations == 0 && !logged("REFUSED"), "snd cap: stray writes");
    drv_audio_abort();
}

static int run_snd(int level, unsigned *max_rcv_peak, unsigned *max_spk_peak)
{
    unsigned w = 0, n = 0;
    while (!drv_audio_probe(level, &w) && ++n < 20000) {
        F.now += (uint64_t)(w ? w : 1) * 1000U;
        /* the ring holds only this output's periods 400 ms after it came on (the ring is 341 ms) */
        if (max_rcv_peak && ((rcv_on && F.now - rcv_t0 > 400000) || (spk_on && F.now - spk_t0 > 400000))) {
            const int16_t *r = (const int16_t *)(const void *)(F.rmem + CAL_RM_PLAY);
            unsigned pk = 0;
            for (unsigned i = 0; i < 0x10000 / 2; ++i)
                pk = (unsigned)abs(r[i]) > pk ? (unsigned)abs(r[i]) : pk;
            unsigned *m = rcv_on ? max_rcv_peak : max_spk_peak;
            *m = pk > *m ? pk : *m;
        }
    }
    return n < 20000;
}
static const char *snd_last(void)
{
    for (unsigned i = F.nlogs; i--;)
        if (strstr(F.logs[i], "drvprobe: audio: PASS") || strstr(F.logs[i], "drvprobe: audio: FAIL"))
            return F.logs[i];
    return "";
}
static void snd_all_off(const char *t)
{
    CHECK(abox_state() == ABOX_ST_OFF && !(reg(ABOX_PMU + 0x4004) & 0xF), "%s: Calliope / AUD domain left on", t);
    CHECK(!F.r_run && !F.w_run && !F.mic_on && !spk_on && !rcv_on && !cal_playing() && !cal_capping(),
          "%s: something left on (RDMA0 %d WDMA1 %d mic %d spk %u rcv %u)", t, F.r_run, F.w_run, F.mic_on, spk_on,
          rcv_on);
    CHECK(F.violations == 0 && F.ram_violations == 0 && !logged("REFUSED"), "%s: stray writes", t);
}

static void test_snd_probe(void)
{
    /* level 1: silent, the real engine through Calliope, then the mic */
    snd_setup();
    snd_volume(SND_OUT_SPEAKER, 7);
    CHECK(run_snd(1, 0, 0), "snd L1: did not finish");
    CHECK(strstr(snd_last(), "PASS snd's engine through Calliope: RDMA0 19") && strstr(snd_last(), "WDMA1 19") &&
          strstr(snd_last(), "output stayed off"), "snd L1: %s", snd_last());
    CHECK(amp_calls_snd == 0 && logged("output: loudspeaker kept off (the probe's silent run)"),
          "snd L1: the amps were touched (%u calls)", amp_calls_snd);
    CHECK(F.mic_calls == 2 && F.r_starts == 1 && F.w_starts == 1, "snd L1: mic calls %u, starts %u / %u", F.mic_calls,
          F.r_starts, F.w_starts);
    snd_all_off("snd L1");
    /* the user's settings back: unmuted, volume 7, a tone plays out loud again */
    CHECK(snd_get_volume(SND_OUT_SPEAKER) == 7, "snd L1: volume");
    CHECK(snd_tone(SND_TONE_TEST) == 0, "snd L1: tone after");
    polls(300);
    CHECK(ring_nonzero() && spk_on, "snd L1: muted / silent left on");
    snd_tone_stop();
    polls(2600);
    abox_off();
    polls(10);
    drv_audio_abort();

    /* level 1, WDMA1 dead: FAIL with the mic first */
    snd_setup();
    F.wdma_dead = 1;
    CHECK(run_snd(1, 0, 0), "snd L1 wdma: did not finish");
    CHECK(strstr(snd_last(), "FAIL the mic through Calliope: WDMA1 0 B/s (192000 expected); RDMA0 192000 B/s ok"),
          "snd L1 wdma: %s", snd_last());
    snd_all_off("snd L1 wdma");

    /* level 2: the earpiece then the loudspeaker, both heard; quiet levels; the user's volume back */
    snd_setup();
    snd_volume(SND_OUT_SPEAKER, 7);
    unsigned prcv = 0, pspk = 0;
    CHECK(run_snd(2, &prcv, &pspk), "snd L2: did not finish");
    const char *l = snd_last();
    CHECK(strstr(l, "PASS earpiece: heard by the mic: yes (1 kHz ") &&
          strstr(l, "; loudspeaker: heard by the mic: yes (1 kHz "), "snd L2: %s", l);
    int a = aev_at("rcv(1,0)", 0), b = aev_at("off", a), c = aev_at("spk(1,0)", b), d = aev_at("off", c);
    CHECK(a >= 0 && b > a && c > b && d > c && naev == 4, "snd L2: amp order rcv %d off %d spk %d off %d (%u calls)", a,
          b, c, d, naev);
    CHECK(prcv > 3000 && prcv <= 3277 && pspk > 1700 && pspk <= 1843, "snd L2: peaks earpiece %u (<= -20 dBFS), "
          "loudspeaker %u (<= -25 dBFS)", prcv, pspk);
    CHECK(logged("drvprobe: audio: baseline: the mic's RMS") && logged("drvprobe: audio: earpiece: 1 kHz ") &&
          logged("drvprobe: audio: loudspeaker: 1 kHz "), "snd L2: lines");
    CHECK(snd_get_volume(SND_OUT_SPEAKER) == 7 && snd_get_volume(SND_OUT_EARPIECE) == 10, "snd L2: volumes back");
    CHECK(F.w_starts == 1 && F.r_starts == 1, "snd L2: one capture, one stream (%u / %u)", F.w_starts, F.r_starts);
    snd_all_off("snd L2");

    /* level 2, nothing heard: FAIL, everything off */
    snd_setup();
    F.deaf = 1;
    CHECK(run_snd(2, 0, 0), "snd L2 deaf: did not finish");
    CHECK(strstr(snd_last(), "FAIL no tone heard: earpiece: heard by the mic: no (1 kHz ") &&
          strstr(snd_last(), "loudspeaker: heard by the mic: no"), "snd L2 deaf: %s", snd_last());
    snd_all_off("snd L2 deaf");

    /* level 2 aborted while the loudspeaker plays */
    snd_setup();
    snd_volume(SND_OUT_SPEAKER, 7);
    unsigned w = 0, n = 0;
    while (!spk_on && n++ < 5000) {
        (void)drv_audio_probe(2, &w);
        F.now += (uint64_t)(w ? w : 1) * 1000U;
    }
    CHECK(spk_on && F.w_run && F.r_run, "snd L2 abort: the loudspeaker phase not reached");
    drv_audio_abort();
    CHECK(!spk_on && !rcv_on && !F.w_run && !F.mic_on && snd_get_volume(SND_OUT_SPEAKER) == 7, "snd L2 abort: left on");
    polls(10);
    CHECK(abox_state() == ABOX_ST_OFF, "snd L2 abort: Calliope left on");
    snd_all_off("snd L2 abort");
}

/* the phone's run order (run 7): audio L0, calliope L0, calliope L1, then audio L1 (and the old order, audio L1
 * right after calliope L0) */
static void test_probe_order(void)
{
    for (int order = 0; order < 2; ++order) {
        snd_setup();
        CHECK(run_snd(0, 0, 0), "order %d: audio L0 did not finish", order);
        CHECK(run(0), "order %d: calliope L0 did not finish", order);
        if (order == 0)
            CHECK(run(1), "order %d: calliope L1 did not finish", order);
        CHECK(run_snd(1, 0, 0), "order %d: audio L1 did not finish", order);
        CHECK(strstr(snd_last(), "PASS snd's engine through Calliope"), "order %d: audio L1: %s", order, snd_last());
        snd_all_off("order");
        drv_audio_abort();
    }
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    F.rmem = calloc(1, RMEM_SIZE);
    if (!F.rmem)
        return 2;
    test_level0();
    test_level1(1);
    test_level1(0);
    test_level2();
    test_failures();
    test_hooks();
    test_snd_engine();
    test_goertzel();
    test_snd_capture();
    test_snd_probe();
    test_probe_order();
    printf("calliope host test: %d checks, %d failed\n", checks, fails);
    free(F.w);
    free(F.rmem);
    return fails ? 1 : 0;
}
