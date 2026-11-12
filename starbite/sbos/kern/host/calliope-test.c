/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/abox_media.c (media through Calliope) with kern/abox.c, against a fake register
 * space, a fake Calliope and a fake RDMA0 (calliope-test.sh builds it).
 *
 * The fake is abox-test.c's (PMU AUD domain and ABOX CPU bits, PLL_AUD, SRAM IPC words, ABOX's GIC, the SysMMU fault
 * bits, abox_rmem as RAM, VENDOR files with the phone's sizes) plus RDMA0 as Calliope runs it: it only starts on an
 * IPC_PCMPLAYBACK TRIGGER 1 for channel 0 after OPEN, SET_BUFFER, HW_PARAMS and PREPARE, and only if IOVA 0x91000000
 * is mapped in OUR page tables to the ring (else a SysMMU fault, as the hardware would raise); then STATUS advances at
 * 192000 bytes a second and Calliope sends a PCM_PLTDAI_POINTER every period; TRIGGER 0 stops it.
 * Checked: the probe at levels 0 / 1 / 2, Samsung's IPC order, the page tables, the routes and the AUD domain put back,
 * every failure step leaving ABOX off, the abort, and the abox.c hooks' refusals. Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "abox.h"
#include "abox_media.h"
#include "drvprobe.h"
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

#define RMEM_BASE 0xFB200000ULL
#define RMEM_SIZE 0x2800000ULL
#define MAGIC_PA  (0xF7900000ULL + 0x600000ULL)
#define HSLOTS    (1u << 16)
#define RD0       (ABOX_SFR + 0x1000)

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
    int boot_ok, pcm_dead, rdma_dead, missing_sram;
    /* the fake RDMA0 */
    int r_open, r_buf, r_par, r_prep, r_run;
    uint32_t r_iova, r_period, r_periods;
    uint64_t r_t0, r_next;
    unsigned r_starts, r_ptrs;
    /* the fake amp */
    int amp_on;
    uint64_t amp_on_seq, amp_off_seq;
    unsigned amp_calls;
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
        F.r_run = 0;
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

/* the SysMMU walk for the ring: our L1 at PT_BASE, a page entry, L2 small pages onto the ring */
static int ring_mapped(void)
{
    uint64_t pt = (uint64_t)reg(ABOX_SYSMMU + 0x00C) << 12;
    if (pt < RMEM_BASE || pt >= RMEM_BASE + RMEM_SIZE || !(reg(ABOX_SYSMMU) & 1))
        return 0;
    uint32_t e = ((uint32_t *)(void *)(F.rmem + (pt - RMEM_BASE)))[0x910];
    if ((e & 3) != 1)
        return 0;
    uint64_t l2 = (uint64_t)(e & ~0x3Fu) << 4;
    if (l2 < RMEM_BASE || l2 >= RMEM_BASE + RMEM_SIZE)
        return 0;
    uint32_t *t2 = (uint32_t *)(void *)(F.rmem + (l2 - RMEM_BASE));
    for (uint32_t k = 0; k < (F.r_period * F.r_periods + 0xFFF) >> 12; ++k)
        if ((t2[k] & 3) != 2 || ((uint64_t)(t2[k] & ~0xFFu) << 4) != RMEM_BASE + CAL_RM_PLAY + 0x1000ULL * k)
            return 0;
    return 1;
}

static void pcm_msg(const uint32_t *m)
{
    if (m[0] != 2 || m[3] != 0)
        return;
    switch (m[2]) {
    case 11: F.r_open = 1; break;
    case 20: F.r_buf = F.r_open, F.r_iova = m[4], F.r_period = m[5], F.r_periods = m[6]; break;
    case 14: F.r_par = F.r_buf && m[4] == 48000 && m[5] == 16 && m[6] == 2; break;
    case 16: F.r_prep = F.r_par; break;
    case 17:
        if (m[4] == 1 && F.r_prep && F.r_iova == 0x91000000u) {
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

static void reset(int pd_on)
{
    uint8_t *rm = F.rmem;
    struct wr *w = F.w;
    size_t capw = F.capw;
    memset(&F, 0, sizeof(F));
    F.rmem = rm, F.w = w, F.capw = capw;
    memset(F.rmem, 0, RMEM_SIZE);
    F.boot_ok = 1;
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
    struct cal_probe_ops o = { f_file, a_probe, a_rcv, a_off };
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
static uint64_t last_wr(uint64_t pa)
{
    for (size_t i = F.nw; i--;)
        if (F.w[i].pa == pa)
            return F.w[i].seq;
    return 0;
}
/* the index of the n-th playback-channel-0 message of `type` from `from` */
static int find_pcm(unsigned from, uint32_t type)
{
    for (unsigned i = from; i < F.nipc; ++i)
        if (F.ipc[i].w[0] == 2 && F.ipc[i].w[3] == 0 && F.ipc[i].w[2] == type)
            return (int)i;
    return -1;
}

/* the probe at one level until it says it is done (or 400 calls) */
static int run(int level)
{
    unsigned w = 0, n = 0;
    while (!drv_calliope_probe(level, &w) && ++n < 400)
        F.now += (uint64_t)w * 1000U;
    return n < 400;
}

static void off_as_found(const char *t, int pd_on)
{
    CHECK(abox_state() == ABOX_ST_OFF, "%s: abox state %d", t, abox_state());
    CHECK(!(reg(ABOX_PMU + 0x4160) & 1), "%s: ABOX CPU still on", t);
    CHECK((reg(ABOX_PMU + 0x4004) & 0xF) == (pd_on ? 0xFu : 0u), "%s: AUD domain %#x, found %s", t,
          reg(ABOX_PMU + 0x4004), pd_on ? "on" : "off");
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
    CHECK(strstr(l, "PASS Calliope booted (CRL0 in ") && strstr(l, "RDMA0 moved 19") && strstr(l, "STATUS") &&
          strstr(l, "snd's 32 x 2048 ring 19"), "%s: %s", t, l);
    CHECK(logged("abox: 2/13") && logged("abox: 12/13 ready") && logged("abox: fw: Calliope boot") &&
          logged("1/4 boot: Calliope said BOOT_DONE, version CRL0"), "%s: abox step / BOOT_DONE / fw lines", t);
    CHECK(logged("abox: 5/13 SysMMU: extra window 0x91000000 -> 0xfc780000 (128 KiB, 4 KiB pages)"),
          "%s: extra window line", t);
    CHECK(logged("Calliope sent 49 period pointers = 188160 B/s"), "%s: pointer rate", t);
    CHECK(F.r_starts == 2, "%s: RDMA0 started %u times (want 2)", t, F.r_starts);

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
    /* routes and UAIF1 as found */
    CHECK((reg(ABOX_SFR + 0x200) & 0xF) == 0 && (reg(ABOX_SFR + 0x40) & 0xF0) == 0 && reg(ABOX_SFR + 0x510) == 0x1000010,
          "%s: SPUS0 %#x ROUTE0 %#x UAIF1 CTRL0 %#x not as found", t, reg(ABOX_SFR + 0x200), reg(ABOX_SFR + 0x40),
          reg(ABOX_SFR + 0x510));
    off_as_found(t, pd_on);
    if (pd_on)
        CHECK(F.nsmc >= 2 && F.smc[F.nsmc - 1].a1 == 1 && F.smc[F.nsmc - 1].seq > last_wr(ABOX_PMU + 0x415C),
              "%s: the domain back on with the DTZPC restore last", t);
    else
        CHECK(F.smc[F.nsmc - 1].a1 == 0, "%s: domain found off but restored on", t);
    CHECK(logged(pd_on ? "AUD domain on (as found)" : "AUD domain off (as found)"), "%s: off line", t);

    /* the extra window is gone: a later boot (a call) does not map it */
    reset(0);
    CHECK(abox_boot() == 0, "%s: plain boot", t);
    CHECK(((uint32_t *)(void *)(F.rmem + ABOX_RM_PT))[0x910] == 0, "%s: extra window left mapped", t);
    abox_off();
}

static void test_level2(void)
{
    reset(1);
    CHECK(run(2), "L2: did not finish");
    CHECK(strstr(last_line(), "PASS Calliope booted (CRL0") && strstr(last_line(), "earpiece tone played 2 s"),
          "L2: %s", last_line());
    int tr = find_pcm(0, 17);
    int ts = tr >= 0 ? find_pcm((unsigned)tr + 1, 17) : -1;
    CHECK(tr >= 0 && ts > tr && F.amp_on_seq > F.ipc[tr].seq && F.amp_off_seq && F.amp_off_seq < F.ipc[ts].seq,
          "L2: amp on after TRIGGER 1 and off before TRIGGER 0");
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
    off_as_found("L2", 1);
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
    off_as_found("boot fail", 1);

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
    off_as_found("pcm dead", 1);

    /* RDMA0 never moves and no pointers */
    reset(1);
    F.rdma_dead = 1;
    CHECK(run(1), "rdma dead: did not finish");
    CHECK(strstr(last_line(), "FAIL 3/4 measure: RDMA0 did not move through Calliope"), "rdma dead: %s", last_line());
    off_as_found("rdma dead", 1);

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
    off_as_found("abort", 1);
    CHECK((reg(ABOX_SFR + 0x40) & 0xF0) == 0, "abort: route left");
    drv_calliope_abort();                                   /* twice is safe */
    off_as_found("abort twice", 1);
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
}

/* ---- snd.c with the Calliope engine ------------------------------------------------------------------------------ */
static unsigned spk_on;
static void s_log(const char *s) { f_log(0, s); }
static uint64_t s_now(void) { return F.now; }
static int s_owns(void)
{
    int s = abox_state();
    return s == ABOX_ST_BOOTING || s == ABOX_ST_READY || s == ABOX_ST_CALL;
}
static int s_spk(int on, int level) { (void)level; spk_on = (unsigned)on; return 0; }
static int s_rcv(int on, int level) { (void)on, (void)level; return 0; }
static void s_off(void) { spk_on = 0; }
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
    printf("calliope host test: %d checks, %d failed\n", checks, fails);
    free(F.w);
    free(F.rmem);
    return fails ? 1 : 0;
}
