/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/abox.c against a fake register space and a fake Calliope.
 *
 *   cc -std=gnu11 -O1 -Wall -Wextra -Werror -I starbite/sbos/kern \
 *      starbite/sbos/kern/host/abox-test.c starbite/sbos/kern/abox.c -o /tmp/abox-test && /tmp/abox-test [-v]
 *
 * The fake models what the driver relies on: the PMU AUD domain and ABOX CPU status bits, PLL_AUD's STABLE, ABOX
 * SRAM with the IPC words, ABOX's GIC (SOFTINT consumed by the fake Calliope, IAR handing out its messages), the
 * SysMMU's fault status, abox_rmem / the VSS magic word as RAM, and VENDOR files with the phone's sizes (synthetic
 * contents). Every register write is checked against the allowed audio blocks independently of abox.c's own guard,
 * and the order of the key writes, SMCs and IPC messages is checked. Exit status 0 = all passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "abox.h"

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

/* ---- the fake hardware -------------------------------------------------------------------------------------------- */
#define RMEM_BASE 0xFB200000ULL
#define RMEM_SIZE 0x2800000ULL
#define VSS_BASE  0xF7900000ULL
#define MAGIC_PA  (VSS_BASE + 0x600000ULL)
#define HSLOTS    (1u << 16)

struct wr { uint64_t seq, pa; uint32_t v; };
struct ipc { uint64_t seq; uint32_t w[8]; };
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
    struct ipc ipc[512];
    unsigned nipc;
    struct rxm rx[32];
    unsigned nrx;
    struct { uint64_t seq, fn, a1, a2, a3; } smc[16];
    unsigned nsmc;
    unsigned violations, ram_violations;
    /* behaviour */
    int boot_ok, pll_ok, ipc_dead_after_boot, ipc_dead, log_in_dram, no_irq, missing_sram, cpu_was_on, pd_was_on;
    int pd_stuck;                   /* aud_off does not take (drivers run 7) */
    uint64_t cpu_on_at;
    int booted_once;
    char logs[4000][200];
    unsigned nlogs;
    unsigned dvfs_calls;
    uint32_t dvfs_khz[64];
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

/* what the fake firmware does when its CPU starts: a log ring, a gear request, BOOT_DONE 64 ms later */
static void calliope_start(void)
{
    /* the DRAM-ring variant also says the VSS handshake is done (abox_vss_ready) */
    const char *text = F.log_in_dram ? "Calliope boot\nVSS_init_proc\n[VSS] CP Magic Code is ready, VHM_initialize done\n"
                                     : "Calliope boot\nVSS_init_proc\n";
    uint32_t addr = F.log_in_dram ? 0x80100000u : 0x30800u, n = (uint32_t)strlen(text);
    if (F.log_in_dram) {
        uint8_t *p = F.rmem + 0x100000;
        memset(p, 0, 268);
        memcpy(p + 12, text, n);
        p[0] = (uint8_t)n, p[8] = 0, p[9] = 1;           /* writer n, reader 0, size 256 */
    } else {
        F.sram[0x30800 / 4] = n, F.sram[0x30804 / 4] = 0, F.sram[0x30808 / 4] = 256;
        memcpy((uint8_t *)F.sram + 0x3080C, text, n);
    }
    if (!F.boot_ok)
        return;
    rx_push(F.now + 20000, 1, 0x10, 0, addr, 0);              /* REPORT_LOG */
    rx_push(F.now + 30000, 1, 0x04, 1, 0xB00D, 0);            /* CHANGE_GEAR gear 1 id 0xB00D */
    rx_push(F.now + 64000, 1, 0x03, 0, 0, 0x43524C30);        /* BOOT_DONE "CRL0" */
}

static void cpu_update(void)
{
    int on = (reg(ABOX_PMU + 0x415C) & 1) && (reg(ABOX_PMU + 0x4164) >> 28 & 1);
    uint32_t was = reg(ABOX_PMU + 0x4160) & 1;
    set(ABOX_PMU + 0x4160, (uint32_t)on);
    if (on && !was) {
        F.cpu_on_at = F.now;
        set(ABOX_PMU + 0x3804, 0);
        calliope_start();
    }
}

static uint32_t f_rd32(void *c, uint64_t pa)
{
    (void)c;
    if (pa >= ABOX_SRAM && pa < ABOX_SRAM + ABOX_SRAM_SIZE) {
        if (F.no_irq && pa == ABOX_SRAM + ABOX_IPC_RX_ACK && !F.sram[ABOX_IPC_RX_ACK / 4] && F.nrx && F.rx[0].at <= F.now) {
            memcpy(&F.sram[ABOX_IPC_RX / 4], F.rx[0].w, sizeof(F.rx[0].w));
            F.sram[ABOX_IPC_RX_ACK / 4] = 1;
            memmove(F.rx, F.rx + 1, (--F.nrx) * sizeof(F.rx[0]));
        }
        return F.sram[(pa - ABOX_SRAM) / 4];
    }
    if (pa == ABOX_GICC + 0x0C) {
        if (!F.no_irq && F.nrx && F.rx[0].at <= F.now && !F.sram[ABOX_IPC_RX_ACK / 4]) {
            memcpy(&F.sram[ABOX_IPC_RX / 4], F.rx[0].w, sizeof(F.rx[0].w));
            F.sram[ABOX_IPC_RX_ACK / 4] = 1;
            memmove(F.rx, F.rx + 1, (--F.nrx) * sizeof(F.rx[0]));
            return (1u << 10) | 1u;                         /* SGI 1 from CPU 1 */
        }
        return 1023;
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
    if (pa == ABOX_PMU + 0x4000 && !(F.pd_stuck && !(v & 0xF)))
        set(ABOX_PMU + 0x4004, v & 0xF);
    if (pa == ABOX_PMU + 0x415C || pa == ABOX_PMU + 0x4164)
        cpu_update();
    if (pa == ABOX_CMU + 0x0120) {
        uint32_t x = v & ~(1u << 7) & ~(1u << 29);
        if ((v >> 31) && F.pll_ok)
            x |= 1u << 29;
        set(pa, x);
    }
    if (pa == ABOX_GICD + 0xF00) {                          /* SOFTINT: the fake Calliope takes the message */
        if (F.ipc_dead || (F.ipc_dead_after_boot && F.booted_once))
            return;
        if (F.nipc < 512) {
            F.ipc[F.nipc].seq = F.seq;
            memcpy(F.ipc[F.nipc].w, &F.sram[ABOX_IPC_TX / 4], sizeof(F.ipc[0].w));
            F.nipc++;
        }
        uint32_t *m = &F.sram[ABOX_IPC_TX / 4];
        if (m[0] == 1 && m[2] == 0x01)                       /* SUSPEND: the CPU goes to WFI */
            set(ABOX_PMU + 0x3804, 1u << 28);
        F.sram[ABOX_IPC_TX_ACK / 4] = 0;
    }
}

static int64_t f_smc(void *c, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)c;
    F.seq++;
    if (F.nsmc < 16)
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
    if (F.nlogs < 4000)
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

/* VENDOR: the phone's sizes, synthetic bytes */
static const struct { const char *name; long size; } files[] = {
    { "calliope_sram.bin", 34676 }, { "calliope_dram.bin", 2209916 }, { "calliope_iva.bin", 57728 },
    { "dsm.bin", 75024 },           { "AP_AUDIO_SLSI.bin", 99622 },   { "APBargeIn_AUDIO_SLSI.bin", 9234 },
    { "SoundBoosterParam.bin", 12456 }, { "APDV_AUDIO_SLSI.bin", 169562 }, { "APBiBF_AUDIO_SLSI.bin", 424 },
    { "dsm_tune.bin", 2384 },
};
static uint8_t fbyte(unsigned f, long i) { return (uint8_t)((i * 131u + f * 17u + (i >> 8)) ^ 0x5A); }
static long f_file(void *c, const char *path, void *dst, uint32_t cap)
{
    (void)c;
    if (strncmp(path, "firmware/", 9))
        return -1;
    for (unsigned f = 0; f < sizeof(files) / sizeof(files[0]); ++f)
        if (!strcmp(path + 9, files[f].name)) {
            if ((F.missing_sram && f == 0) || !dst || files[f].size > (long)cap)
                return -1;
            for (long i = 0; i < files[f].size; ++i)
                ((uint8_t *)dst)[i] = fbyte(f, i);
            return files[f].size;
        }
    return -1;
}
static int f_dvfs(void *c, uint32_t khz)
{
    (void)c;
    if (F.dvfs_calls < 64)
        F.dvfs_khz[F.dvfs_calls] = khz;
    F.dvfs_calls++;
    return 0;
}

static void reset(void)
{
    uint8_t *rm = F.rmem;
    struct wr *w = F.w;
    size_t capw = F.capw;
    memset(&F, 0, sizeof(F));
    F.rmem = rm, F.w = w, F.capw = capw;
    memset(F.rmem, 0, RMEM_SIZE);
    F.boot_ok = F.pll_ok = 1;
    set(ABOX_SFR + 0x0, 0x41424F58);                        /* IP_INDEX "ABOX" */
    set(ABOX_SFR + 0x4, 0x10100);
    set(ABOX_SYSMMU + 0x0, 0x4);                            /* S-Boot leaves it translating */
    set(ABOX_CMU + 0x3000, 0), set(ABOX_CMU + 0x3060, 1);   /* one Q-channel gated by hardware: left alone */
}

static void install(int with_dvfs)
{
    struct abox_hal h = { 0 };
    h.rd32 = f_rd32, h.wr32 = f_wr32, h.smc = f_smc, h.delay_us = f_delay, h.log = f_log, h.file = f_file;
    h.ram = f_ram, h.now_us = f_now;
    if (with_dvfs)
        h.dvfs_aud = f_dvfs;
    abox_hal_set(&h);
}

/* ---- query helpers ------------------------------------------------------------------------------------------------ */
static uint64_t first_wr(uint64_t pa, uint32_t mask, uint32_t want)
{
    for (size_t i = 0; i < F.nw; ++i)
        if (F.w[i].pa == pa && (F.w[i].v & mask) == want)
            return F.w[i].seq;
    return 0;
}
static uint64_t last_wr(uint64_t pa, uint32_t mask, uint32_t want)
{
    for (size_t i = F.nw; i--;)
        if (F.w[i].pa == pa && (F.w[i].v & mask) == want)
            return F.w[i].seq;
    return 0;
}
static int logged(const char *needle)
{
    for (unsigned i = 0; i < F.nlogs; ++i)
        if (strstr(F.logs[i], needle))
            return 1;
    return 0;
}
/* the index of the first IPC from `from` matching ipcid / type (pcm: w[2]) and optionally w[3] */
static int find_ipc(unsigned from, uint32_t ipcid, uint32_t type, int64_t w3)
{
    for (unsigned i = from; i < F.nipc; ++i)
        if (F.ipc[i].w[0] == ipcid && F.ipc[i].w[2] == type && (w3 < 0 || F.ipc[i].w[3] == (uint32_t)w3))
            return (int)i;
    return -1;
}
static uint32_t l1(uint32_t iova) { return ((uint32_t *)(void *)(F.rmem + ABOX_RM_PT))[iova >> 20]; }

static void common_after(const char *what)
{
    CHECK(F.violations == 0, "%s: %u register writes outside the audio blocks", what, F.violations);
    CHECK(F.ram_violations == 0, "%s: %u RAM mappings outside abox_rmem / the magic word", what, F.ram_violations);
    CHECK(!logged("REFUSED"), "%s: abox.c refused a write", what);
}

/* ---- tests -------------------------------------------------------------------------------------------------------- */
static void test_full_cycle(int log_in_dram, int no_irq)
{
    const char *t = log_in_dram ? "cycle (log ring in DRAM)" : no_irq ? "cycle (no GIC interrupt)" : "cycle";
    reset();
    F.log_in_dram = log_in_dram, F.no_irq = no_irq;
    F.magic = 0xDEADBEEF;
    install(0);

    abox_vss_prepare();
    CHECK(F.magic == 0, "%s: magic word not cleared", t);
    CHECK(F.nw == 0 && F.nsmc == 0, "%s: vss_prepare touched registers", t);

    int r = abox_boot();
    CHECK(r == 0, "%s: abox_boot returned %d (%s)", t, r, abox_why());
    CHECK(abox_state() == ABOX_ST_READY, "%s: state %d", t, abox_state());
    CHECK(abox_version() == 0x43524C30, "%s: version %#x", t, abox_version());
    CHECK(logged("Calliope is ready to sing (version CRL0)"), "%s: no ready line", t);
    CHECK(logged("abox: fw: Calliope boot") && logged("abox: fw: VSS_init_proc"), "%s: firmware log lines missing", t);
    CHECK(abox_vss_ready() == (log_in_dram ? 1 : 0), "%s: abox_vss_ready %d", t, abox_vss_ready());
    CHECK(!log_in_dram || logged("abox: VSS: Calliope says the CP's magic code is ready"), "%s: no VSS handshake line", t);
    if (no_irq)
        CHECK(logged("no interrupt at ABOX's GICC"), "%s: fallback not noted", t);
    F.booted_once = 1;

    /* power first: PMU AUD on, then the restore SMC, then the first CMU write */
    uint64_t pd = first_wr(ABOX_PMU + 0x4000, 0xF, 0xF), cmu = 0;
    for (size_t i = 0; i < F.nw && !cmu; ++i)
        if (F.w[i].pa >= ABOX_CMU && F.w[i].pa < ABOX_CMU + 0x8000)
            cmu = F.w[i].seq;
    CHECK(pd && F.nsmc >= 1 && F.smc[0].seq > pd && cmu > F.smc[0].seq, "%s: power order (pd %llu smc %llu cmu %llu)", t,
          (unsigned long long)pd, (unsigned long long)F.smc[0].seq, (unsigned long long)cmu);
    CHECK(F.smc[0].fn == 0x82000410 && F.smc[0].a1 == 1 && F.smc[0].a2 == 0x17C10204 && F.smc[0].a3 == 0,
          "%s: restore SMC args", t);
    /* clocks */
    CHECK((reg(ABOX_CMU + 0x120) & 0x03FF3F07) == (45u << 16 | 1u << 8) && (reg(ABOX_CMU + 0x120) >> 29 & 5) == 5,
          "%s: PLL_CON0 %#x", t, reg(ABOX_CMU + 0x120));
    CHECK(reg(ABOX_CMU + 0x18C) == 24319 && reg(ABOX_CMU + 0x0) == 500, "%s: PLL K / locktime", t);
    CHECK(reg(ABOX_CMU + 0x1820) == 3, "%s: DIV_CLK_AUD_PLL %u (no DVFS hook: idle ratio 3)", t, reg(ABOX_CMU + 0x1820));
    CHECK(first_wr(ABOX_CMU + 0x1820, 0xF, 3) < first_wr(ABOX_CMU + 0x120, 1u << 31, 1u << 31),
          "%s: CPU divider must be set before the PLL starts", t);
    CHECK(reg(ABOX_CMU + 0x1804) == 1 && reg(ABOX_CMU + 0x1808) == 2 && reg(ABOX_CMU + 0x1800) == 47,
          "%s: BUS / BUSP / AUDIF dividers", t);
    CHECK(reg(ABOX_CMU + 0x3000) == 2 && reg(ABOX_CMU + 0x3060) == 1 && reg(ABOX_CMU + 0x302C) == 2,
          "%s: Q-channels (DUMMY req, SYSMMU left to hardware, BCLK1 req)", t);
    CHECK(reg(ABOX_SYSREG + 0x32C) == 1, "%s: MISC_CON", t);
    /* SysMMU */
    CHECK(reg(ABOX_SYSMMU + 0x00C) == (uint32_t)((RMEM_BASE + ABOX_RM_PT) >> 12), "%s: PT_BASE %#x", t,
          reg(ABOX_SYSMMU + 0x00C));
    CHECK(reg(ABOX_SYSMMU + 0x000) == 5 && first_wr(ABOX_SYSMMU, 0xFFFFFFFF, 7) < first_wr(ABOX_SYSMMU, 0xFFFFFFFF, 5),
          "%s: SysMMU BLOCK then ENABLE", t);
    CHECK(l1(0x80000000) == (uint32_t)(RMEM_BASE >> 4 | 2) && l1(0x81100000) == (uint32_t)((RMEM_BASE + 0x1100000) >> 4 | 2) &&
          l1(0x81200000) == 0, "%s: DRAM sections", t);
    CHECK(l1(0x90000000) == (uint32_t)((RMEM_BASE + ABOX_RM_IVA) >> 4 | 2) && l1(0x90100000) == 0, "%s: IVA section", t);
    CHECK(l1(0x91400000) == (uint32_t)((RMEM_BASE + ABOX_RM_RDMA4) >> 4 | 2) &&
          l1(0x92200000) == (uint32_t)((RMEM_BASE + ABOX_RM_WDMA2) >> 4 | 2), "%s: RDMA4 / WDMA2", t);
    CHECK(l1(0xA0000000) == 0x0F790002 && l1(0xA0600000) == 0x0F7F0002 && l1(0xA0700000) == 0, "%s: VSS 7 MiB", t);
    CHECK(l1(0xA1000000) == 0x0F900002 && l1(0xA1100000) == 0x0F910002 && l1(0xA1200000) == 0, "%s: VSS PARAM 2 MiB", t);
    CHECK(l1(0xD0000000) == (uint32_t)((RMEM_BASE + ABOX_RM_DUMP) >> 4 | 2) && l1(0xD1200000) == 0, "%s: dump", t);
    CHECK(l1(0x10000000) == (uint32_t)((RMEM_BASE + ABOX_RM_PT_L2) >> 4 | 1) &&
          ((uint32_t *)(void *)(F.rmem + ABOX_RM_PT_L2))[0] == 0x01000002, "%s: CHIPID page", t);
    /* GIC */
    CHECK(reg(ABOX_GICC + 4) == 0xFF && reg(ABOX_GICD) == 3 && reg(ABOX_GICC) == 3 && reg(ABOX_GICD + 0x400) == 0x10101010 &&
          reg(ABOX_GICD + 0x49C) == 0x10101010 && !first_wr(ABOX_GICD + 0x4A0, 0, 0), "%s: GIC init", t);
    /* pins */
    CHECK((reg(ABOX_GPIO + 0x20) & 0xFFFF) == 0x2222 && (reg(ABOX_GPIO + 0x28) & 0xFFFF) == 0x1111 &&
          (reg(ABOX_GPIO + 0x00) & 0xFFFF0) == 0x22220 && (reg(ABOX_GPIO + 0x08) & 0xFFFF0) == 0x11110, "%s: pins", t);
    /* firmware: SRAM image + zero; DRAM image + ext_bins + zero */
    int sram_ok = 1, dram_ok = 1;
    for (long i = 0; i < 34676; ++i)
        sram_ok &= ((uint8_t *)F.sram)[i] == fbyte(0, i);
    for (long i = 34676; i < 0x30000; ++i)
        sram_ok &= ((uint8_t *)F.sram)[i] == 0 || (i >= 0x30800 && i < 0x30900);
    for (long i = 0; i < 2209916; i += 7)
        dram_ok &= F.rmem[i] == fbyte(1, i) || (log_in_dram && i >= 0x100000 && i < 0x100200);
    CHECK(sram_ok, "%s: SRAM image / zero fill", t);
    CHECK(dram_ok && F.rmem[0x300000] == 0 && F.rmem[0x4D0000] == fbyte(7, 0) && F.rmem[0x502000 + 100] == fbyte(3, 100) &&
          F.rmem[0x7E0000 + 99621] == fbyte(4, 99621) && F.rmem[0x601000] == fbyte(9, 0) &&
          F.rmem[ABOX_RM_IVA + 57727] == fbyte(2, 57727) && F.rmem[ABOX_RM_IVA + 57728] == 0, "%s: DRAM / IVA / ext_bins", t);
    /* the CPU only after the tables, the SRAM image and DRAM on; pads after the CPU */
    uint64_t cpu = first_wr(ABOX_PMU + 0x4164, 1u << 28, 1u << 28);
    CHECK(cpu && first_wr(ABOX_SYSMMU, 0xFFFFFFFF, 5) < cpu && last_wr(ABOX_SRAM + 0x2FFFC, 0, 0) < cpu &&
          first_wr(ABOX_SFR + 0x10, 1, 1) < cpu && first_wr(ABOX_PMU + 0x415C, 1, 1) < cpu, "%s: CPU start order", t);
    CHECK(first_wr(ABOX_PMU + 0x4170, 1u << 28, 1u << 28) > cpu && first_wr(ABOX_PMU + 0x1424, 1, 1) > cpu,
          "%s: pad retention after the CPU", t);
    /* restore IPCs after BOOT_DONE: RESUME, 9 rates, 9 formats, 2 ERAP closes, SET_MODE, SET_TYPE, CHANGED_GEAR */
    CHECK(F.nipc == 24, "%s: %u IPCs after boot (want 24)", t, F.nipc);
    if (F.nipc >= 24) {
        CHECK(F.ipc[0].w[0] == 1 && F.ipc[0].w[2] == 2, "%s: RESUME first", t);
        int ok = 1;
        for (unsigned i = 0; i < 9; ++i)
            ok &= F.ipc[1 + i].w[0] == 0xA && F.ipc[1 + i].w[2] == 1 + i && F.ipc[1 + i].w[3] == 48000;
        for (unsigned i = 0; i < 9; ++i)
            ok &= F.ipc[10 + i].w[0] == 0xA && F.ipc[10 + i].w[2] == 0x10 + i && F.ipc[10 + i].w[3] == 16 &&
                  F.ipc[10 + i].w[4] == 2;
        ok &= F.ipc[19].w[0] == 5 && F.ipc[19].w[2] == 2 && F.ipc[19].w[3] == 0 && F.ipc[20].w[3] == 1;
        ok &= F.ipc[21].w[0] == 1 && F.ipc[21].w[2] == 0x50 && F.ipc[21].w[3] == 0;
        ok &= F.ipc[22].w[0] == 1 && F.ipc[22].w[2] == 0x60 && F.ipc[22].w[3] == 0;
        ok &= F.ipc[23].w[0] == 1 && F.ipc[23].w[2] == 0x09 && F.ipc[23].w[3] == 294912000u;   /* CHANGED_GEAR, Hz */
        CHECK(ok, "%s: restore IPC contents / order", t);
        CHECK(F.ipc[0].seq > first_wr(ABOX_PMU + 0x4164, 1u << 28, 1u << 28), "%s: no IPC before the CPU", t);
    }
    CHECK((reg(ABOX_SFR + 0x204) & 0x1F) == 9 && (reg(ABOX_SFR + 0x304) >> 8 & 0x1F) == 9, "%s: MIXP / RECP format", t);
    CHECK((reg(ABOX_SFR + 0x10) & 1) == 0, "%s: DRAM-on released after boot", t);
    common_after(t);

    /* VSS */
    unsigned n0 = F.nipc;
    CHECK(!abox_vss_started(), "%s: vss_started before START_VSS", t);
    r = abox_vss_start();
    CHECK(abox_vss_started(), "%s: vss_started after START_VSS", t);
    CHECK(r == 0 && F.nipc == n0 + 1 && F.ipc[n0].w[0] == 1 && F.ipc[n0].w[2] == 0xA0, "%s: START_VSS", t);

    /* a call */
    n0 = F.nipc;
    uint64_t s0 = F.seq;
    r = abox_call(1);
    CHECK(r == 0 && abox_state() == ABOX_ST_CALL, "%s: call on %d (%s)", t, r, abox_why());
    int mode = find_ipc(n0, 1, 0x50, 2), type = find_ipc(n0, 1, 0x60, 0);
    int ro = find_ipc(n0, 2, 11, 4), rb = find_ipc(n0, 2, 20, 4), rh = find_ipc(n0, 2, 14, 4), rp = find_ipc(n0, 2, 16, 4),
        rt = find_ipc(n0, 2, 17, 4);
    int wo = find_ipc(n0, 3, 11, 2), wb = find_ipc(n0, 3, 20, 2), wh = find_ipc(n0, 3, 14, 2), wp = find_ipc(n0, 3, 16, 2),
        wt = find_ipc(n0, 3, 17, 2);
    CHECK(mode >= 0 && type > mode && ro > type && rb > ro && rh > rb && rp > rh && wo > rp && wb > wo && wh > wb &&
          wp > wh && rt > wp && wt > rt, "%s: call IPC order", t);
    if (rb >= 0 && wb >= 0 && rh >= 0 && rt >= 0 && wt >= 0 && ro >= 0) {
        CHECK(F.ipc[ro].w[1] == 4 && F.ipc[rb].w[4] == 0x91400000 && F.ipc[rb].w[5] == 1920 && F.ipc[rb].w[6] == 4,
              "%s: RDMA4 buffer", t);
        CHECK(F.ipc[wb].w[4] == 0x92200000 && F.ipc[wb].w[5] == 1920, "%s: WDMA2 buffer", t);
        CHECK(F.ipc[rh].w[4] == 48000 && F.ipc[rh].w[5] == 16 && F.ipc[rh].w[6] == 2, "%s: hw_params", t);
        CHECK(F.ipc[rt].w[4] == 1 && F.ipc[wt].w[4] == 1, "%s: trigger 1", t);
        uint64_t spk = last_wr(ABOX_SFR + 0x510, 1, 1), mic = last_wr(ABOX_SFR + 0x500, 2, 2);
        CHECK(spk > s0 && spk < F.ipc[rt].seq, "%s: UAIF1 SPK before RDMA4 trigger (playback POST)", t);
        CHECK(mic > F.ipc[wt].seq, "%s: UAIF0 MIC after WDMA2 trigger (capture PRE)", t);
    }
    CHECK((reg(ABOX_SFR + 0x40) >> 4 & 0xF) == 1 && (reg(ABOX_SFR + 0x48) & 0xF) == 1 && (reg(ABOX_SFR + 0x304) & 1) == 1 &&
          (reg(ABOX_SFR + 0x44) >> 4 & 0xF) == 8 && !(reg(ABOX_SFR + 0x300) >> 11 & 1) &&
          (reg(ABOX_SFR + 0x200) >> 16 & 0xF) == 3, "%s: routes", t);
    CHECK(reg(ABOX_SFR + 0x514) == 0x0984F000 && reg(ABOX_SFR + 0x504) == 0x0984F000, "%s: UAIF CTRL1 %#x %#x", t,
          reg(ABOX_SFR + 0x514), reg(ABOX_SFR + 0x504));
    CHECK((reg(ABOX_SFR + 0x510) & 0x5) == 0x5 && (reg(ABOX_SFR + 0x500) & 0x1E) == 0x16, "%s: UAIF CTRL0 %#x %#x", t,
          reg(ABOX_SFR + 0x510), reg(ABOX_SFR + 0x500));
    CHECK(reg(ABOX_CMU + 0x1800) == 15 && reg(ABOX_CMU + 0x1828) == 47 && reg(ABOX_CMU + 0x1824) == 47,
          "%s: AUDIF 73.728 MHz, BCLK 1.536 MHz", t);
    CHECK((reg(ABOX_SFR + 0x38) >> 2 & 1) && (reg(ABOX_SFR + 0x10) & 1), "%s: BCLK0 qch disabled, DRAM on", t);
    CHECK(first_wr(ABOX_SFR + 0x208, 1, 1) > s0, "%s: MIXP flush at start", t);
    common_after(t);

    /* poll: a gear request is answered, period pointers are counted */
    n0 = F.nipc;
    rx_push(F.now, 1, 0x04, 3, 0xCA11, 0);
    rx_push(F.now, 2, 18, 4, 0x91400780, 0);
    r = abox_poll();
    CHECK(r == 0 && F.nipc == n0 + 1 && F.ipc[n0].w[2] == 0x09, "%s: CHANGE_GEAR answered (%d, %u)", t, r, F.nipc - n0);
    CHECK(logged("first period pointer, playback"), "%s: pointer noted", t);

    /* hang up */
    n0 = F.nipc;
    s0 = F.seq;
    r = abox_call(0);
    CHECK(r == 0 && abox_state() == ABOX_ST_READY, "%s: call off %d", t, r);
    int rt0 = find_ipc(n0, 2, 17, 4), wt0 = find_ipc(n0, 3, 17, 2), rf = find_ipc(n0, 2, 15, 4), rc = find_ipc(n0, 2, 12, 4),
        wf = find_ipc(n0, 3, 15, 2), wc = find_ipc(n0, 3, 12, 2), m0 = find_ipc(n0, 1, 0x50, 0);
    CHECK(rt0 >= 0 && wt0 > rt0 && rf > wt0 && rc > rf && wf > rc && wc > wf && m0 > wc, "%s: hang-up IPC order", t);
    if (rt0 >= 0 && wt0 >= 0) {
        uint64_t micoff = last_wr(ABOX_SFR + 0x500, 2, 0);
        CHECK(F.ipc[rt0].w[4] == 0 && micoff > F.ipc[rt0].seq && micoff < F.ipc[wt0].seq,
              "%s: RDMA4 stop, then UAIF0 MIC off, then WDMA2 stop", t);
    }
    CHECK((reg(ABOX_SFR + 0x40) >> 4 & 0xF) == 0 && (reg(ABOX_SFR + 0x48) & 0xF) == 0 && (reg(ABOX_SFR + 0x304) & 1) == 0 &&
          (reg(ABOX_SFR + 0x44) >> 4 & 0xF) == 0 && !(reg(ABOX_SFR + 0x510) & 1) && !(reg(ABOX_SFR + 0x38) >> 2 & 1) &&
          !(reg(ABOX_SFR + 0x10) & 1), "%s: routes / UAIF / DRAM back", t);
    CHECK(last_wr(ABOX_SFR + 0x30C, 2, 2) > s0 && last_wr(ABOX_SFR + 0x308, 1, 1) > s0, "%s: SIFM1 / RECP flush", t);
    common_after(t);

    /* off */
    n0 = F.nipc;
    unsigned ns = F.nsmc;
    CHECK(abox_vss_started(), "%s: vss_started through the call", t);
    abox_off();
    CHECK(abox_state() == ABOX_ST_OFF, "%s: state after off", t);
    CHECK(!abox_vss_started(), "%s: vss_started after off", t);
    CHECK(find_ipc(n0, 1, 0x01, -1) >= 0, "%s: SUSPEND sent", t);
    CHECK(!(reg(ABOX_PMU + 0x4164) >> 28 & 1) && !(reg(ABOX_PMU + 0x415C) & 1) && !(reg(ABOX_PMU + 0x4160) & 1),
          "%s: CPU off", t);
    CHECK(F.nsmc == ns + 1 && F.smc[ns].a1 == 0 && F.smc[ns].a2 == 0x17C10204 &&
          F.smc[ns].seq < last_wr(ABOX_PMU + 0x4000, 0xF, 0), "%s: TZPC save SMC before AUD off", t);
    CHECK((reg(ABOX_PMU + 0x4004) & 0xF) == 0, "%s: AUD domain off", t);
    CHECK((reg(ABOX_GPIO + 0x20) & 0xFFFF) == 0 && (reg(ABOX_GPIO + 0x00) & 0xFFFF0) == 0, "%s: pins idle", t);
    CHECK(reg(ABOX_CMU + 0x3000) == 0 && reg(ABOX_CMU + 0x302C) == 0, "%s: Q-channel requests released", t);
    CHECK(reg(ABOX_SYSMMU) == 3, "%s: SysMMU blocked + disabled", t);
    common_after(t);
    CHECK(abox_poll() == 0, "%s: poll while off", t);
}

static void test_missing_file(void)
{
    reset();
    F.missing_sram = 1;
    install(0);
    int r = abox_boot();
    CHECK(r == ABOX_EFILE && abox_state() == ABOX_ST_FAILED, "missing file: %d", r);
    CHECK(F.nw == 0 && F.nsmc == 0, "missing file: hardware touched (%zu writes, %u SMCs)", F.nw, F.nsmc);
    CHECK(logged("calliope_sram.bin: MISSING"), "missing file: not said");
    abox_off();
    CHECK(F.nw == 0, "missing file: off touched hardware");
}

static void teardown_checks(const char *t)
{
    CHECK(abox_state() == ABOX_ST_FAILED, "%s: state %d", t, abox_state());
    CHECK((reg(ABOX_PMU + 0x4004) & 0xF) == 0 && !(reg(ABOX_PMU + 0x4160) & 1), "%s: domain / CPU left on", t);
    CHECK(F.nsmc >= 1 && F.smc[F.nsmc - 1].a1 == 0, "%s: no TZPC save", t);
    CHECK(abox_poll() < 0, "%s: poll should report the failure", t);
    common_after(t);
}

static void test_no_boot_done(void)
{
    reset();
    F.boot_ok = 0;
    install(0);
    int r = abox_boot();
    CHECK(r == ABOX_ETIME, "no BOOT_DONE: %d", r);
    CHECK(F.now >= 3000000 && F.now < 3600000, "no BOOT_DONE: waited %llu us", (unsigned long long)F.now);
    CHECK(logged("no BOOT_DONE within 3000 ms"), "no BOOT_DONE: not said");
    teardown_checks("no BOOT_DONE");
}

static void test_pll(void)
{
    reset();
    F.pll_ok = 0;
    install(0);
    int r = abox_boot();
    CHECK(r == ABOX_ECLOCK, "PLL: %d", r);
    CHECK(!first_wr(ABOX_PMU + 0x4164, 1u << 28, 1u << 28), "PLL: CPU started anyway");
    teardown_checks("PLL");
}

static void test_faults(void)
{
    reset();
    install(0);
    CHECK(abox_boot() == 0, "fault: boot");
    set(ABOX_SYSMMU + 0x60, 1), set(ABOX_SYSMMU + 0x70, 0xA0700000);
    int r = abox_poll();
    CHECK(r == ABOX_EFAULT && logged("SysMMU fault"), "SysMMU fault: %d", r);
    teardown_checks("SysMMU fault");

    reset();
    install(0);
    CHECK(abox_boot() == 0, "report fault: boot");
    rx_push(F.now, 1, 0xFA, 4, 0, 0);
    r = abox_poll();
    CHECK(r == ABOX_EFAULT && logged("vss error"), "REPORT_FAULT: %d", r);
    teardown_checks("REPORT_FAULT");
}

static void test_sboot_left_on(void)
{
    reset();
    set(ABOX_PMU + 0x4004, 0xF), set(ABOX_PMU + 0x4000, 0xF);
    set(ABOX_PMU + 0x415C, 1), set(ABOX_PMU + 0x4164, 1u << 28), set(ABOX_PMU + 0x4160, 1);
    set(ABOX_CMU + 0x120, (1u << 31) | (1u << 29) | (1u << 4) | (45u << 16) | (1u << 8));
    set(ABOX_CMU + 0x18C, 24319);
    install(0);
    int r = abox_boot();
    CHECK(r == 0, "S-Boot left on: %d (%s)", r, abox_why());
    uint64_t stop = first_wr(ABOX_PMU + 0x4164, 1u << 28, 0), sram = first_wr(ABOX_SRAM, 0, 0);
    CHECK(stop && stop < sram, "S-Boot left on: CPU must stop before SRAM is written");
    CHECK(F.nsmc == 0, "S-Boot left on: no restore SMC when the domain was on");
    CHECK(logged("PLL_AUD already at 1179.648 MHz"), "S-Boot left on: PLL kept");
    common_after("S-Boot left on");
    abox_off();
}

/* drivers run 7: abox_off's aud_off did not take; the next abox_boot must cycle the domain, not take it for S-Boot's */
static void test_pd_left_on(void)
{
    reset();
    install(0);
    CHECK(abox_boot() == 0, "left on: first boot %s", abox_why());
    F.pd_stuck = 1;
    abox_off();
    CHECK(logged("AUD domain STILL ON"), "left on: the stop says so");
    CHECK((reg(ABOX_PMU + 0x4004) & 0xF) == 0xF, "left on: the fake kept it on");
    F.pd_stuck = 0;
    unsigned ns = F.nsmc;
    CHECK(abox_boot() == 0, "left on: second boot %s", abox_why());
    CHECK(logged("left on by the last stop: cycled off first"), "left on: no cycle logged");
    CHECK(!logged("already on (S-Boot)"), "left on: taken for S-Boot's state");
    int restore = 0;
    for (unsigned i = ns; i < F.nsmc; ++i)
        restore += F.smc[i].a1 == 1;
    CHECK(restore == 1, "left on: one restore SMC after the cycle (%d)", restore);
    abox_off();
    /* stuck for good: the boot fails cleanly instead of booting Calliope on a stale domain */
    F.pd_stuck = 1;
    CHECK(abox_boot() == 0, "left on: third boot %s", abox_why());
    abox_off();
    CHECK(abox_boot() == ABOX_EPOWER, "left on: a domain that never goes off must fail the boot");
    F.pd_stuck = 0;
}

static void test_pd_export(void)
{
    reset();
    install(0);
    CHECK(abox_pd_on() == 0, "pd_on: %s", abox_why());
    CHECK((F.nsmc == 1 && F.smc[0].a1 == 1 && F.smc[0].a2 == 0x17C10204), "pd_on: one restore SMC after aud_on");
    CHECK(abox_pd_on() == 0 && F.nsmc == 1, "pd_on again: nothing done");
    abox_pd_off();
    CHECK(F.nsmc == 2 && F.smc[1].a1 == 0 && F.smc[1].a2 == 0x17C10204, "pd_off: the TZPC save SMC");
    abox_pd_off();
    CHECK(F.nsmc == 2, "pd_off again: nothing done");
    /* while ABOX runs, the domain belongs to abox_boot / abox_off */
    CHECK(abox_boot() == 0, "pd export: boot %s", abox_why());
    CHECK(abox_pd_on() == ABOX_ESTATE, "pd_on refused while ABOX runs");
    unsigned ns = F.nsmc;
    abox_pd_off();
    CHECK(F.nsmc == ns, "pd_off ignored while ABOX runs");
    abox_off();
}

static void test_ipc_dead(void)
{
    reset();
    F.ipc_dead_after_boot = 1;
    install(0);
    CHECK(abox_boot() == 0, "IPC dead: boot");
    F.booted_once = 1;
    int r = abox_call(1);
    CHECK(r == ABOX_EIPC, "IPC dead: call %d", r);
    teardown_checks("IPC dead");
}

static void test_dvfs_hook(void)
{
    reset();
    install(1);
    CHECK(abox_boot() == 0, "DVFS: boot");
    CHECK(!first_wr(ABOX_CMU + 0x1820, 0, 0), "DVFS: DIV_CLK_AUD_PLL written although ACPM owns it");
    CHECK(F.dvfs_calls >= 1 && F.dvfs_khz[0] == 1180000, "DVFS: boot level %u", F.dvfs_khz[0]);
    CHECK(abox_call(1) == 0, "DVFS: call");
    CHECK(F.dvfs_khz[(F.dvfs_calls - 1) & 63] == 1180000, "DVFS: call level");
    CHECK(abox_call(0) == 0, "DVFS: hang up");
    abox_off();
    CHECK(F.dvfs_khz[(F.dvfs_calls - 1) & 63] == 0, "DVFS: released at off");
    common_after("DVFS");
}

static void test_cfg(void)
{
    struct abox_cfg c;
    abox_cfg_default(&c);
    CHECK(abox_cfg_set(&c) == 0, "cfg: defaults refused");
    c.rmem_base += 0x1000;
    CHECK(abox_cfg_set(&c) == ABOX_ECFG, "cfg: unaligned accepted");
    abox_cfg_default(&c);
    c.rmem_size = 0x1000000;
    CHECK(abox_cfg_set(&c) == ABOX_ECFG, "cfg: small rmem accepted");
    abox_cfg_default(&c);
    CHECK(abox_cfg_set(&c) == 0, "cfg: defaults back");
}


/* ---- the staged boot (abox_boot_start / abox_boot_step / abox_boot_finish) ---------------------------------------- */
#define SLINES 200
static char slines[2][SLINES][200];
/* the boot's own lines ("abox: ..."), the timing line left out and "after N ms" cut (a staged wait looks every 5 ms) */
static unsigned boot_lines(char (*out)[200])
{
    unsigned n = 0;
    for (unsigned i = 0; i < F.nlogs && n < SLINES; ++i) {
        if (strncmp(F.logs[i], "abox: ", 6) || strstr(F.logs[i], "abox: boot timing"))
            continue;
        snprintf(out[n], 200, "%s", F.logs[i]);
        char *a = strstr(out[n], ") after ");
        if (a && strstr(out[n], "ready to sing"))
            a[1] = 0;
        n++;
    }
    return n;
}

static void test_staged(void)
{
    /* the blocking boot, then the same boot a stage per call with 5 ms between calls: the same lines and IPCs */
    reset();
    install(0);
    CHECK(abox_boot() == 0, "staged: blocking boot %s", abox_why());
    unsigned nb = boot_lines(slines[0]), nipc_b = F.nipc;
    CHECK(logged("abox: boot timing: ") && logged(" ms in all: files "), "staged: no timing line");
    abox_off();

    reset();
    install(0);
    CHECK(abox_boot_start() == 1 && abox_state() == ABOX_ST_BOOTING, "staged: start not pending");
    CHECK(F.nw == 0 && F.nsmc == 0, "staged: start touched the hardware");
    int r, calls = 0;
    while ((r = abox_boot_step()) > 0 && calls < 2000) {
        CHECK(abox_state() == ABOX_ST_BOOTING, "staged: state %d while pending", abox_state());
        F.now += 5000;                                      /* the caller's loop runs meanwhile */
        calls++;
        if (calls == 1)
            CHECK(logged("1/13 files: calliope_sram.bin") && !logged("calliope_dram.bin"), "staged: stage 1 not sram only");
        if (calls == 2)
            CHECK(logged("calliope_dram.bin") && !logged("calliope_iva.bin"), "staged: stage 2 not dram only");
        if (calls == 3)
            CHECK(F.nw == 0 && F.nsmc == 0, "staged: the files stages touched the hardware");
    }
    CHECK(r == 0 && abox_state() == ABOX_ST_READY && abox_version() == 0x43524C30, "staged: %d (%s)", r, abox_why());
    CHECK(calls >= 7 && calls <= 30, "staged: %d calls (6 stages + the 64 ms wait at 5 ms)", calls);
    unsigned ns = boot_lines(slines[1]);
    CHECK(nb == ns, "staged: %u lines, blocking %u", ns, nb);
    for (unsigned i = 0; i < nb && i < ns; ++i)
        CHECK(!strcmp(slines[0][i], slines[1][i]), "staged: line %u differs: '%s' / '%s'", i, slines[0][i], slines[1][i]);
    CHECK(F.nipc == nipc_b, "staged: %u IPCs, blocking %u", F.nipc, nipc_b);
    CHECK(abox_boot_step() == 0 && abox_boot_finish() == 0, "staged: step / finish after READY");
    CHECK(abox_boot() == 0 && logged("abox: boot: Calliope already runs"), "staged: the modem's boot after it");
    common_after("staged");
    abox_off();
    CHECK(abox_boot_step() == ABOX_ESTATE, "staged: step while off");

    /* abox_boot() while a staged boot is pending finishes it (a call never gets ABOX_ESTATE) */
    reset();
    install(0);
    CHECK(abox_boot_start() == 1 && abox_boot_step() == 1 && abox_boot_step() == 1, "staged: two stages");
    CHECK(abox_boot_start() == ABOX_ESTATE, "staged: a second start while pending");
    CHECK(abox_pd_on() == ABOX_ESTATE, "staged: pd_on while booting");
    CHECK(abox_boot() == 0 && abox_state() == ABOX_ST_READY, "staged: abox_boot while pending %s", abox_why());
    CHECK(logged("abox: boot: a staged boot is pending: finished here"), "staged: not said");
    CHECK(logged("13/13 restore") && !logged("Calliope already runs"), "staged: restore");
    common_after("staged finish");
    abox_off();

    /* abox_boot_finish on its own */
    reset();
    install(0);
    CHECK(abox_boot_start() == 1 && abox_boot_finish() == 0 && abox_state() == ABOX_ST_READY, "staged: finish");
    abox_off();

    /* a failure in a stage: torn down, FAILED, step keeps returning the error */
    reset();
    F.pll_ok = 0;
    install(0);
    CHECK(abox_boot_start() == 1, "staged PLL: start");
    while ((r = abox_boot_step()) > 0)
        ;
    CHECK(r == ABOX_ECLOCK && abox_boot_step() == ABOX_ECLOCK, "staged PLL: %d", r);
    teardown_checks("staged PLL");

    /* no BOOT_DONE, staged: the timeout on the HAL's clock (3 s), not on the number of calls */
    reset();
    F.boot_ok = 0;
    install(0);
    CHECK(abox_boot_start() == 1, "staged timeout: start");
    calls = 0;
    while ((r = abox_boot_step()) > 0 && calls++ < 100000)
        F.now += 20000;
    CHECK(r == ABOX_ETIME && calls > 140 && calls < 170, "staged timeout: %d after %d calls of 20 ms", r, calls);
    teardown_checks("staged timeout");
}

/* abox_off() after every stage of a pending boot: everything undone, the next boot works */
static void test_off_each_stage(void)
{
    for (int k = 0; k <= 7; ++k) {
        char t[48];
        snprintf(t, sizeof t, "off after %d stages", k);
        reset();
        install(0);
        CHECK(abox_boot_start() == 1, "%s: start", t);
        for (int i = 0; i < k; ++i)
            CHECK(abox_boot_step() == 1, "%s: step %d", t, i);
        F.nlogs = 0;
        abox_off();
        CHECK(logged("abox: stopped: "), "%s: no stopped line", t);
        CHECK(abox_state() == ABOX_ST_OFF, "%s: state %d", t, abox_state());
        CHECK((reg(ABOX_PMU + 0x4004) & 0xF) == 0 && !(reg(ABOX_PMU + 0x4160) & 1), "%s: domain / CPU left on", t);
        CHECK(k < 4 ? logged("AUD domain not touched") : logged("AUD domain off"), "%s: domain line", t);
        CHECK(abox_boot_step() == ABOX_ESTATE, "%s: step after off", t);
        CHECK(abox_boot() == 0 && abox_state() == ABOX_ST_READY, "%s: the next boot %s", t, abox_why());
        common_after(t);
        abox_off();
    }
}

/* a HAL without now_us (optional): abox_boot's wait counts its 100 us delays, as before */
static void test_no_clock(void)
{
    reset();
    F.boot_ok = 0;
    struct abox_hal h = { 0 };
    h.rd32 = f_rd32, h.wr32 = f_wr32, h.smc = f_smc, h.delay_us = f_delay, h.log = f_log, h.file = f_file;
    h.ram = f_ram;
    abox_hal_set(&h);
    int r = abox_boot();
    CHECK(r == ABOX_ETIME && F.now >= 3000000 && F.now < 3600000, "no clock: %d after %llu us", r,
          (unsigned long long)F.now);
    CHECK(!logged("abox: boot timing"), "no clock: a timing line without a clock");
    reset();
    abox_hal_set(&h);
    CHECK(abox_boot() == 0 && logged("ready to sing (version CRL0) after 64 ms"), "no clock: boot %s", abox_why());
    abox_off();
    install(0);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    F.rmem = calloc(1, RMEM_SIZE);
    if (!F.rmem)
        return 2;
    CHECK(abox_boot() == ABOX_ESTATE, "boot without a HAL");
    test_cfg();
    test_full_cycle(0, 0);
    test_full_cycle(1, 0);
    test_full_cycle(0, 1);
    test_missing_file();
    test_no_boot_done();
    test_pll();
    test_faults();
    test_sboot_left_on();
    test_pd_export();
    test_pd_left_on();
    test_ipc_dead();
    test_dvfs_hook();
    test_staged();
    test_off_each_stage();
    test_no_clock();
    printf("abox host test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
