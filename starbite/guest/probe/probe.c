/* SPDX-License-Identifier: GPL-2.0-only
 * guest-probe P1 (docs/STARBITE-OS-LINUX-M3.md 10): the phone facts Starbite OS 0.9's guest Linux on the M3
 * cluster depends on, measured without Linux. A bare libsb payload, unattended, about 15 s, then a restart
 * into recovery (TWRP: pull pmsg). Every step logs one line "gprobe: ..." (pmsg); the last is "gprobe: RESULT".
 *
 *   1. the CPU, the GIC (lines, this core's interface bit), the M3 cluster's power state;
 *   2. the carve-out (0x8_8000_0000 .. 0x8_E400_0000): one probe read, then write / read back / restore of
 *      four words at the start of every 16 MiB (the libsb map has the high bank non-cacheable);
 *   3. doorbell candidates: SPIs no node of the S9 DT uses, from the top of the GIC's range: idle (not enabled,
 *      pending or active) -> edge, to this core, enable (reads back = a non-secure line) -> pend -> it must
 *      arrive at GICC_IAR within 2 ms -> everything restored. The first two that pass are the doorbells;
 *   4. the M3 cores, one at a time (0x100 .. 0x103), after the vendor PMU defaults (the same six fields the
 *      daily Linux writes with s9p_vendor_m3_defaults): the stub (m3stub.S) at the guest's Image address, core
 *      0x100 entered directly (does x0 carry PSCI's context id?), 0x101..0x103 through the sbshm trampoline
 *      (x0 = the DTB literal). Each reports MPIDR, EL, its GIC interface bit, then waits in WFI for the
 *      doorbell targeted at that bit, rings the host's doorbell and calls CPU_OFF;
 *   5. after the last core: AFFINITY_INFO per core and the cluster power state (PMU), the A55's own timer.
 * The watchdog: the loader arms it for the launch (catalog wdt); this image feeds it in every wait.
 */
#include "sb.h"
#include "../sbshm.h"

#define GICD SBG_S9_GICD
#define GICC SBG_S9_GICC
#define WDT1 0x10060000UL
#define LBASE SBG_S9_LINUX_BASE
#define SHM SBG_S9_SHM_BASE
#define STUB_AT (LBASE + 0x80000ULL)    /* where the Image goes (text_offset 0x80000) */
#define BLK (SHM + 0x800ULL)            /* the stub's result block, inside the header page */

extern const char m3stub_start[], m3stub_lit[], m3stub_end[];

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint8_t rdb(uintptr_t a) { return *(volatile uint8_t *)a; }
static inline void wrb(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

static void feed(void)
{
    volatile uint32_t *w = (volatile uint32_t *)WDT1;
    if (w[0] & (1U << 5))
        w[2] = w[1];
}
static void wait_ms(unsigned ms)
{
    for (unsigned i = 0; i < ms; ++i) {
        if (!(i % 50))
            feed();
        sb_mdelay(1);
    }
}

static struct ub u;
static void L(void) { sb_log(u.b); ub_init(&u); ub_s(&u, "gprobe: "); }
static void hx(uint64_t v)
{
    unsigned d = 1;
    while (d < 16 && (v >> (4 * d)))
        ++d;
    ub_hex(&u, v, d);
}

/* the S9 DT's SPIs (INTIDs), from exynos9810-starlte.dtb (mk: starbite/guest/probe/build.sh comment) */
static const uint16_t dt_used[] = {
    38, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 68, 69, 70, 71, 72, 73, 74,
    75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 141, 176,
    177, 178, 179, 199, 200, 201, 202, 214, 215, 220, 274, 275, 276, 281, 282, 285, 313, 314, 315, 428, 436, 453,
    497,
};
static int dt_uses(unsigned id)
{
    for (unsigned i = 0; i < sizeof(dt_used) / sizeof(dt_used[0]); ++i)
        if (dt_used[i] == id)
            return 1;
    return 0;
}

static uint8_t me;
static unsigned lines;

static int bit(uintptr_t base, unsigned id) { return (int)((rd(base + 4 * (id / 32)) >> (id % 32)) & 1U); }

static void gic_setup(void)
{
    /* as Starbite OS's gic_reset (kern/hw.c): every SPI off and to cpu0, PPIs off, distributor + interface on */
    lines = ((rd(GICD + 4) & 0x1fU) + 1U) * 32U;
    if (lines > 1020)
        lines = 1020;
    wr(GICD, 0);
    for (unsigned i = 32; i < lines; i += 32) {
        wr(GICD + 0x180 + i / 8, 0xffffffffU);
        wr(GICD + 0x280 + i / 8, 0xffffffffU);
    }
    for (unsigned i = 32; i < lines; i += 4) {
        wr(GICD + 0x800 + i, 0x01010101U);
        wr(GICD + 0x400 + i, 0xa0a0a0a0U);
    }
    wr(GICD + 0x180, 0xffff0000U);
    wr(GICD + 0x280, 0xffffffffU);
    for (unsigned i = 0; i < 32; i += 4)
        wr(GICD + 0x400 + i, 0xa0a0a0a0U);
    wr(GICD, 1);
    wr(GICC + 0x04, 0xffU);
    wr(GICC + 0x08, 0);
    wr(GICC + 0x00, 1);
    me = rdb(GICD + 0x800);
}

/* wait up to ms for INTID id at this core's interface (others acked and counted) */
static int iar_wait(unsigned id, unsigned us, unsigned *others)
{
    uint64_t t0 = sb_us();
    while (sb_us() - t0 < us) {
        uint32_t iar = rd(GICC + 0x0c), got = iar & 0x3ffU;
        if (got < 1020) {
            wr(GICC + 0x10, iar);
            if (got == id)
                return 1;
            ++*others;
        }
    }
    return 0;
}

static void line_restore(unsigned id, uint32_t cfg_word, uint8_t tgt)
{
    wr(GICD + 0x180 + 4 * (id / 32), 1U << (id % 32));
    wr(GICD + 0x280 + 4 * (id / 32), 1U << (id % 32));
    wr(GICD + 0xc00 + 4 * (id / 16), cfg_word);
    wrb(GICD + 0x800 + id, tgt);
}

/* step 3: one doorbell candidate. 1 usable, 0 not (u has the reason) */
static int try_line(unsigned id)
{
    if (bit(GICD + 0x100, id) || bit(GICD + 0x200, id) || bit(GICD + 0x300, id)) {
        ub_s(&u, " busy");
        return 0;
    }
    uintptr_t cfga = GICD + 0xc00 + 4 * (id / 16);
    uint32_t cfg0 = rd(cfga);
    uint8_t tg0 = rdb(GICD + 0x800 + id);
    wr(cfga, cfg0 | (2U << (2 * (id % 16))));
    wrb(GICD + 0x800 + id, me);
    wr(GICD + 0x100 + 4 * (id / 32), 1U << (id % 32));
    if (!bit(GICD + 0x100, id)) {
        ub_s(&u, " secure");
        line_restore(id, cfg0, tg0);
        return 0;
    }
    unsigned others = 0;
    wr(GICD + 0x200 + 4 * (id / 32), 1U << (id % 32));
    int got = iar_wait(id, 2000, &others);
    line_restore(id, cfg0, tg0);
    ub_s(&u, got ? " ok" : " pend-lost");
    if (others) {
        ub_s(&u, "(+");
        ub_dec(&u, others);
        ub_s(&u, " other)");
    }
    return got;
}

struct blk {
    uint64_t state, mpidr, el, x0, cpuif, intid, frq, wfis, t0, sctlr, t1, off_ret;
};

/* step 4: one M3 core. 1 passed */
static int m3_core(unsigned c, unsigned db_guest, unsigned db_host, uint64_t magic_ctx, uint64_t magic_dtb)
{
    volatile struct blk *b = (volatile struct blk *)BLK;
    volatile uint64_t *bw = (volatile uint64_t *)BLK;
    for (unsigned i = 0; i < sizeof(struct blk) / 8; ++i)
        bw[i] = 0;
    /* the stub at the Image address, its literals patched */
    uint64_t n = (uint64_t)(m3stub_end - m3stub_start), lit = (uint64_t)(m3stub_lit - m3stub_start);
    for (uint64_t i = 0; i < n; i += 4)
        *(volatile uint32_t *)(uintptr_t)(STUB_AT + i) = *(const uint32_t *)(m3stub_start + i);
    volatile uint64_t *L5 = (volatile uint64_t *)(uintptr_t)(STUB_AT + lit);
    L5[0] = BLK;
    L5[1] = GICD;
    L5[2] = GICC;
    L5[3] = db_host;
    L5[4] = 2U * sb_freq();                         /* the stub's timeout: 2 s */
    /* the sbshm trampoline (x0 = its DTB literal) */
    volatile uint32_t *tr = (volatile uint32_t *)(uintptr_t)(SHM + SBSHM_TRAMP_OFF);
    for (unsigned i = 0; i < 10; ++i)
        tr[i] = sbg_tramp[i];
    tr[SBG_TRAMP_IMG] = (uint32_t)STUB_AT;
    tr[SBG_TRAMP_IMG + 1] = (uint32_t)(STUB_AT >> 32);
    tr[SBG_TRAMP_DTB] = (uint32_t)magic_dtb;
    tr[SBG_TRAMP_DTB + 1] = (uint32_t)(magic_dtb >> 32);
    __asm__ volatile("dsb sy" ::: "memory");
    int direct = c == 0;
    uint64_t entry = direct ? STUB_AT : SHM + SBSHM_TRAMP_OFF;
    int64_t r = (int32_t)sb_psci(SB_PSCI_CPU_ON, 0x100U + c, entry, magic_ctx);
    ub_s(&u, "M3 0x10");
    ub_dec(&u, c);
    ub_s(&u, direct ? " direct" : " trampoline");
    ub_s(&u, ": CPU_ON ");
    ub_sdec(&u, r);
    if (r) {
        L();
        return 0;
    }
    uint64_t t0 = sb_ms();
    while (b->state < 1 && sb_ms() - t0 < 1000)
        wait_ms(1);
    if (b->state < 1) {
        ub_s(&u, ", no check-in in 1 s (the core is lost; the cluster stays on until the reset)");
        L();
        return 0;
    }
    uint64_t t_in = sb_ms() - t0;
    uint8_t cif = (uint8_t)b->cpuif;
    ub_s(&u, ", in after ");
    ub_dec(&u, t_in);
    ub_s(&u, " ms: MPIDR ");
    hx(b->mpidr);
    ub_s(&u, " EL");
    ub_dec(&u, (b->el >> 2) & 3U);
    ub_s(&u, " x0 ");
    ub_s(&u, b->x0 == magic_ctx ? "=context id" : b->x0 == magic_dtb ? "=DTB literal" : "other ");
    if (b->x0 != magic_ctx && b->x0 != magic_dtb)
        hx(b->x0);
    ub_s(&u, " cpuif ");
    ub_hex(&u, cif, 2);
    ub_s(&u, " CNTFRQ ");
    ub_dec(&u, b->frq);
    ub_s(&u, " SCTLR ");
    hx(b->sctlr);
    L();
    /* the doorbell to the core's own interface */
    unsigned others = 0;
    int db_ok = 0, host_ok = 0;
    if (db_guest && cif) {
        uintptr_t cfga = GICD + 0xc00 + 4 * (db_guest / 16);
        uint32_t cfg0 = rd(cfga);
        uint8_t tg0 = rdb(GICD + 0x800 + db_guest);
        wr(cfga, cfg0 | (2U << (2 * (db_guest % 16))));
        wrb(GICD + 0x800 + db_guest, cif);
        wr(GICD + 0x100 + 4 * (db_guest / 32), 1U << (db_guest % 32));
        wr(GICD + 0x200 + 4 * (db_guest / 32), 1U << (db_guest % 32));
        while (b->state < 2 && sb_ms() - t0 < 3000)
            wait_ms(1);
        db_ok = b->intid == db_guest;
        host_ok = db_host ? iar_wait(db_host, 100000, &others) : 0;
        line_restore(db_guest, cfg0, tg0);
    } else {
        while (b->state < 2 && sb_ms() - t0 < 3000)
            wait_ms(1);
    }
    wait_ms(20);
    int64_t aff = sb_smp_affinity(4 + c);
    ub_s(&u, "M3 0x10");
    ub_dec(&u, c);
    ub_s(&u, ": state ");
    ub_dec(&u, b->state);
    ub_s(&u, ", woke by INTID ");
    ub_dec(&u, b->intid);
    ub_s(&u, db_ok ? " (the doorbell)" : b->intid == 27 ? " (its 2 s timeout: no doorbell)" : "");
    ub_s(&u, " after ");
    ub_dec(&u, b->wfis);
    ub_s(&u, " WFI, ");
    if (b->t1 > b->t0 && b->frq) {
        ub_dec(&u, (b->t1 - b->t0) * 1000000U / b->frq);
        ub_s(&u, " us in the stub, ");
    }
    ub_s(&u, host_ok ? "host doorbell back ok" : "host doorbell NOT back");
    ub_s(&u, ", AFFINITY_INFO ");
    ub_sdec(&u, aff);
    if (b->state == 3) {
        ub_s(&u, ", CPU_OFF RETURNED ");
        ub_sdec(&u, (int64_t)(int32_t)b->off_ret);
    }
    L();
    return b->state == 2 && db_ok && host_ok;
}

void sb_main(void)
{
    ub_init(&u);
    ub_s(&u, "gprobe: ");
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    ub_s(&u, "guest-probe P1 (Starbite OS 0.9 app support, no Linux): EL");
    ub_dec(&u, (sb_boot.el >> 2) & 3U);
    ub_s(&u, ", MPIDR ");
    hx(mpidr);
    ub_s(&u, ", CNTFRQ ");
    ub_dec(&u, sb_freq());
    ub_s(&u, ", watchdog ");
    ub_s(&u, (rd(WDT1) & (1U << 5)) ? "armed (fed by this image)" : "NOT armed");
    L();
    feed();

    /* 1. the GIC */
    gic_setup();
    int m3_was_on = sb_m3_on();
    ub_s(&u, "GIC-400: ");
    ub_dec(&u, lines);
    ub_s(&u, " lines, TYPER ");
    ub_hex(&u, rd(GICD + 4), 8);
    ub_s(&u, ", IIDR ");
    ub_hex(&u, rd(GICD + 8), 8);
    ub_s(&u, ", this core's interface ");
    ub_hex(&u, me, 2);
    ub_s(&u, "; M3 cluster ");
    ub_s(&u, m3_was_on ? "powered" : "off");
    L();

    /* 2. the carve-out */
    unsigned ok = 0, bad = 0;
    for (uint64_t a = LBASE; a < SHM + SBG_S9_SHM_SIZE; a += 16ULL << 20) {
        uint32_t v;
        if (!sb_probe32(a, &v)) {
            if (!bad++) {
                ub_s(&u, "carve-out: read FAULT at ");
                hx(a);
                L();
            }
            continue;
        }
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)a;
        uint32_t save[4];
        for (unsigned i = 0; i < 4; ++i)
            save[i] = p[i];
        for (unsigned i = 0; i < 4; ++i)
            p[i] = 0x5b900000U ^ (uint32_t)(a >> 12) ^ (i * 0x01010101U);
        int good = 1;
        for (unsigned i = 0; i < 4; ++i)
            if (p[i] != (0x5b900000U ^ (uint32_t)(a >> 12) ^ (i * 0x01010101U)))
                good = 0;
        for (unsigned i = 0; i < 4; ++i)
            p[i] = save[i];
        good ? ++ok : ++bad;
        feed();
    }
    ub_s(&u, "carve-out 0x880000000..0x8e4000000: ");
    ub_dec(&u, ok);
    ub_s(&u, " blocks of 16 MiB ok, ");
    ub_dec(&u, bad);
    ub_s(&u, " bad");
    L();

    /* 3. doorbell candidates */
    unsigned db[2] = {0, 0}, nd = 0, tried = 0;
    ub_s(&u, "doorbell candidates:");
    for (unsigned id = lines - 1; id >= 400 && nd < 2 && tried < 24; --id) {
        if (dt_uses(id))
            continue;
        ++tried;
        ub_c(&u, ' ');
        ub_dec(&u, id);
        if (try_line(id))
            db[nd++] = id;
        feed();
    }
    L();
    ub_s(&u, "doorbells: to the guest ");
    ub_dec(&u, db[0]);
    ub_s(&u, ", to the host ");
    ub_dec(&u, db[1]);
    ub_s(&u, nd == 2 ? "" : " (NOT FOUND: the guest would poll)");
    L();

    /* 4. the M3 cores */
    int pmu = sb_m3_pmu_defaults();
    ub_s(&u, "M3 vendor PMU defaults: ");
    ub_s(&u, pmu ? "set (read back)" : "a read FAILED");
    L();
    unsigned m3ok = 0, m3run = 0;
    uint8_t cif[4] = {0};
    if (pmu) {
        for (unsigned c = 0; c < 4; ++c) {
            ++m3run;
            int r = m3_core(c, db[0], db[1], 0x53423930434f4e54ULL + c, 0x5342393044544221ULL + c);
            cif[c] = (uint8_t)((volatile struct blk *)BLK)->cpuif;
            if (r)
                ++m3ok;
            else if (((volatile struct blk *)BLK)->state < 1)
                break;                      /* a lost core: do not start the next one */
            feed();
        }
    }
    wait_ms(50);

    /* 5. after */
    ub_s(&u, "after: M3 cluster ");
    ub_s(&u, sb_m3_on() ? "STILL POWERED" : "off (PMU)");
    ub_s(&u, ", interfaces ");
    for (unsigned c = 0; c < 4; ++c) {
        ub_hex(&u, cif[c], 2);
        ub_c(&u, ' ');
    }
    ub_s(&u, "AFFINITY_INFO");
    for (unsigned c = 0; c < 4; ++c) {
        ub_c(&u, ' ');
        ub_sdec(&u, sb_smp_affinity(4 + c));
    }
    L();
    int pass = ok > 0 && !bad && nd == 2 && m3ok == 4;
    ub_s(&u, "RESULT ");
    ub_s(&u, pass ? "PASS" : "FAIL");
    ub_s(&u, ": carve-out ");
    ub_s(&u, bad ? "BAD" : "ok");
    ub_s(&u, ", doorbells ");
    ub_dec(&u, db[0]);
    ub_c(&u, '/');
    ub_dec(&u, db[1]);
    ub_s(&u, ", M3 cores ");
    ub_dec(&u, m3ok);
    ub_c(&u, '/');
    ub_dec(&u, m3run ? 4 : 0);
    ub_s(&u, "; restarting into recovery in 5 s");
    L();
    wait_ms(5000);
    sb_reboot(SB_INFORM_RECOVERY);
}
