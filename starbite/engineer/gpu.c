/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Lab GPU tests (v2.1): the Mali-G72 MP18 without any OS (libsb mali.c, mali_desc.c),
 * after the recipe Starbite's iOS port ran on this phone (NOTES.md 2026-10-28: jobs, clears, a
 * triangle and full-frame presents at 260 / 572 MHz, vdd_g3d 950 mV).
 *
 *   Power up      vdd_g3d (BUCK6) to 950 mV (tier 1: the known-good value), soft reset, L2 / the
 *                 18 shader cores / the tiler on, Mali-LPAE tables in DRAM bank 2, AS0
 *   Identify      GPU_ID, cores, L2, MMU, job slots, register sanity (read twice, IRQ masks written
 *                 and read back), the GPU's cycle counter against the CPU's timer (its real clock),
 *                 the GPU timestamp against the CPU's counter
 *   Job manager   WRITE_VALUE on every slot that takes it (a fragment job on a slot that does not),
 *                 every value type, a chain of 128 jobs (all outputs checked, jobs/s), the job
 *                 interrupt seen pending at the GIC, a deliberate MMU fault reported and recovered
 *   Fill          64 clear-only fragment jobs paint an 8 x 8 colour pattern into a 256 x 256 buffer,
 *                 one job at a time and then as one chain, every pixel checked on the CPU; then a
 *                 512 x 512 pattern straight into the screen, read back from the scanout
 *   Stress        full-buffer fragment jobs (JS0) and 64-job WRITE_VALUE chains (JS1) back to back,
 *                 sampled pixels and every value checked each round; Mpixel/s, jobs/s, the GPU's
 *                 TMU zone, vdd_g3d and the clock; stops on Power, the time, an error, a fault or
 *                 the temperature limit (or none: the same "no limit" warning as the CPU tests)
 *   GPU clock     ACPM DVFS (domain g3d = 4) to a level of the vendor table: tier 3; 260 MHz is
 *                 S-Boot's, 572 MHz ran under iOS at 950 mV, the others never ran here
 * Every failure shows the job slot status, JOB / MMU / GPU IRQ_RAWSTAT, AS0 FAULTSTATUS and
 * FAULTADDRESS, GPU_FAULTSTATUS and the job's exception status. Leaving the GPU page powers the
 * cores down and puts vdd_g3d back.
 */
#include "eng.h"

/* the data area (GPU VA SB_GPU_VA + offset; CPU: SB_GPU_CPU) */
#define O_JOBS 0x00000U                     /* 256 WRITE_VALUE jobs of 64 bytes */
#define O_OUT 0x04000U                      /* their outputs, 8 bytes each */
#define O_FRAG 0x08000U                     /* 256 fragment jobs */
#define O_SAMPLES 0x0c000U                  /* the sample-position table */
#define O_FBD 0x10000U                      /* 256 framebuffers of 256 bytes */
#define O_PIX 0x100000U                     /* pixels, up to 4 MiB */
#define VA(o) (SB_GPU_VA + (uint64_t)(o))

#define BUCK6 0x2bU
#define BUCK6_950 0x68U
#define MARK 0xdeadbeefU

static struct sb_auth g_auth;               /* the confirmed power-up: covers the BUCK6 writes back */
static uint8_t g_buck_was;
static int g_buck_known;
static uint32_t g_shaders = 0x5771777U;
static int g_chain_ok = -1;                 /* fragment chains: -1 untested, 0 failed, 1 work */

#define G_N 5
static const char *const g_name[G_N] = { "GPU power-up", "GPU identification", "GPU job manager", "GPU fill pattern",
                                         "GPU stress" };
static char g_detail[G_N][96];
static int g_pass[G_N] = { -1, -1, -1, -1, -1 };

static void g_result(int i, int pass, const char *detail)
{
    g_pass[i] = pass;
    sb_strcpy(g_detail[i], sizeof(g_detail[i]), detail);
    result_line(g_name[i], pass, detail);
}

static volatile uint32_t *cpu32(uint64_t va)
{
    return (volatile uint32_t *)(uintptr_t)SB_GPU_CPU(va);
}

static void put(uint64_t va, const uint32_t *w, unsigned n)
{
    volatile uint32_t *d = cpu32(va);
    for (unsigned i = 0; i < n; ++i)
        d[i] = w[i];
}

static unsigned popcount(uint32_t x)
{
    unsigned n = 0;
    for (; x; x &= x - 1)
        ++n;
    return n;
}

static int buck6(uint8_t *v)
{
    return sb_pmic_read(1, BUCK6, v);
}

static void ub_mv(struct ub *u, uint8_t vsel)
{
    ub_dec(u, (300000U + vsel * 6250U) / 1000U);
    ub_s(u, ".");
    ub_dec(u, ((300000U + vsel * 6250U) % 1000U) / 100U);
    ub_s(u, " mV");
}

static unsigned clock_mhz(int *locked)
{
    int unl = 0, off = 0;
    uint64_t hz = sb_gpu_hz(&off);
    (void)sb_pll_hz(0x17400140UL, &unl);
    if (locked)
        *locked = !off && hz && !unl;
    return (unsigned)(hz / 1000000U);
}

/* the fault registers of a failed job, for a page and the log */
static void ub_fault(struct ub *u, const struct sb_gpu_result *r)
{
    if (r->timeout)
        ub_s(u, "TIMEOUT ");
    ub_s(u, "JS");
    ub_dec(u, r->js);
    ub_s(u, " status ");
    ub_hex(u, r->js_status, 2);
    ub_c(u, ' ');
    ub_s(u, sb_gpu_exc_name(r->js_status));
    ub_s(u, ", irq job ");
    ub_hex(u, r->jirq, 8);
    ub_s(u, " mmu ");
    ub_hex(u, r->mirq, 4);
    ub_s(u, " gpu ");
    ub_hex(u, r->girq, 4);
    if (r->mirq || r->as_fault) {
        ub_s(u, ", AS0 fault ");
        ub_hex(u, r->as_fault, 8);
        ub_c(u, ' ');
        ub_s(u, sb_gpu_exc_name(r->as_fault));
        ub_s(u, " at ");
        ub_hex(u, r->as_addr, 9);
    }
    if (r->gpu_fault) {
        ub_s(u, ", GPU fault ");
        ub_hex(u, r->gpu_fault, 8);
        ub_s(u, " at ");
        ub_hex(u, r->gpu_addr, 9);
    }
}

/* ---- power ------------------------------------------------------------------------------------- */

static int g_up_us[4];

static int domain_on(void)
{
    if (sb_gpu_domain())
        return 1;
    /* S-Boot leaves the domain on; if it is off: PMU EMBEDDED_G3D CONFIGURATION = 0xF, then the
     * G3D TZPC restored through EL3 (SMC 0x82000410(1, 0x17410204, 0), the S9+ Linux port's
     * s9p-g3d-pd), or the first GPU access raises an SError. Tier 3 (PMU), never run here. */
    struct sb_auth au;
    const char *why;
    struct action a = { "Power the G3D domain on", "PMU",
                        { "PMU 0x14064040 (EMBEDDED_G3D_CONFIGURATION) = 0xF, then wait for STATUS 0xF",
                          "Then SMC 0x82000410(1, 0x17410204, 0): the G3D TZPC, as the S9+ Linux port does after a "
                          "power cycle", "Never run from Engineer OS: S-Boot normally leaves the domain on", 0 },
                        "a reboot (S-Boot sets the domain up again)", "off", "on", "", 0, SB_SP_MEM, 0, 0,
                        0x14064040ULL, 4 };
    a.tier = poke_tier(0x14064040ULL, &why);
    if (!lab_confirm(&a, &au) || !lab_poke32(0x14064040ULL, 0xfU, &au))
        return 0;
    for (unsigned i = 0; i < 1000 && !sb_gpu_domain(); ++i)
        sb_udelay(10);
    int64_t r = sb_psci(0x82000410ULL, 1, 0x17410204ULL, 0);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "G3D domain ");
    ub_s(&u, sb_gpu_domain() ? "on" : "STILL OFF");
    ub_s(&u, ", TZPC SMC returned ");
    ub_sdec(&u, r);
    audit_line(u.b);
    lab_major_result("Power the G3D domain on", "PMU", u.b);
    return sb_gpu_domain();
}

static int gpu_power_up(void)
{
    struct ub u;
    char now[32], hap0[96];
    uint8_t was = 0;
    int locked = 0;
    if (sb_gpu_is_up())
        return 1;
    if (!domain_on())
        return 0;
    uint32_t id = 0;
    if (!sb_probe32(SB_GPU, &id) || (id >> 16) != 0x6221U) {
        ub_init(&u);
        ub_s(&u, "GPU_ID ");
        ub_hex(&u, id, 8);
        ub_s(&u, " is not a Mali-G72 (0x6221xxxx)");
        (void)dialog_danger("GPU not found", u.b, "Nothing was written.", "OK");
        return 0;
    }
    unsigned mhz = clock_mhz(&locked);
    if (!locked) {
        (void)dialog_danger("GPU clock not running", "PLL_G3D is off or not locked.",
                            "GPU clock (tier 3) can ask ACPM for 260 MHz first.", "OK");
        return 0;
    }
    if (!buck6(&was)) {
        (void)dialog_danger("vdd_g3d unreadable", "ACPM did not answer for BUCK6.", "Nothing was written.", "OK");
        return 0;
    }
    g_shaders = sb_gpu_rd(0x100);
    if (!g_shaders)
        g_shaders = 0x5771777U;
    const char *why;
    int known = was >= 0x20 && was <= 0x70;
    ub_init(&u);
    ub_s(&u, "vdd_g3d (BUCK6) ");
    ub_mv(&u, was);
    ub_s(&u, " -> 950.0 mV: the value Linux s9p-g3d and Starbite's iOS run the GPU at");
    sb_strcpy(hap0, sizeof(hap0), u.b);
    ub_init(&u);
    ub_hex(&u, was, 2);
    sb_strcpy(now, sizeof(now), u.b);
    struct action a = { "Power up the GPU", "GPU",
                        { hap0, "Soft reset; L2, the shader cores and the tiler powered on (polled)",
                          "Page tables + 8 MiB of test data in DRAM bank 2 at 0x8F0000000 (nothing else uses "
                          "it); the GPU sees only that and the screen",
                          known ? "Jobs run only when you start a test; leaving the GPU page powers it down"
                                : "BUCK6 holds a value Starbite never saw for vdd_g3d: tier 3" },
                        "Power down (or leaving the page): cores off, vdd_g3d back; a reboot resets all",
                        now, "0x68 (950 mV)", "", 0, SB_SP_PMIC, 0, 0, 1U << 8 | BUCK6, 1 };
    a.tier = pmic_tier(1, BUCK6, BUCK6_950, &why);
    if (!known && a.tier < TIER3)
        a.tier = TIER3;
    (void)mhz;
    if (!lab_confirm(&a, &g_auth))
        return 0;
    g_buck_was = was;
    g_buck_known = 1;
    if (was != BUCK6_950 && !lab_pmic(1, BUCK6, BUCK6_950, &g_auth)) {
        (void)dialog_danger("vdd_g3d not set", "The BUCK6 write failed (see the audit log).", "The GPU stays off.", "OK");
        return 0;
    }
    uint8_t now_v = 0;
    sb_udelay(1000);                        /* the ramp: well under 1 ms */
    if (!buck6(&now_v) || now_v != BUCK6_950) {
        (void)dialog_danger("vdd_g3d not at 950 mV", "BUCK6 reads back another value.", "The GPU stays off.", "OK");
        return 0;
    }
    if (!sb_gpu_open(&g_auth)) {
        (void)dialog_danger("GPU session refused", "The confirmation did not cover the power-up.", "", "OK");
        return 0;
    }
    sb_gpu_tables();
    uint32_t smp[33];
    mali_samples(smp);
    put(VA(O_SAMPLES), smp, 33);
    unsigned us[4] = { 0, 0, 0, 0 };
    int ok = sb_gpu_up(g_shaders, us);
    for (int i = 0; i < 4; ++i)
        g_up_us[i] = (int)us[i];
    ub_init(&u);
    ub_s(&u, ok ? "reset " : "FAILED: reset ");
    ub_dec(&u, us[0]);
    ub_s(&u, " us, L2 ");
    ub_dec(&u, us[1]);
    ub_s(&u, " us, ");
    ub_dec(&u, popcount(g_shaders));
    ub_s(&u, " cores ");
    ub_dec(&u, us[2]);
    ub_s(&u, " us, tiler ");
    ub_dec(&u, us[3]);
    ub_s(&u, " us, ");
    ub_dec(&u, clock_mhz(0));
    ub_s(&u, " MHz, 950 mV");
    g_result(0, ok, u.b);
    audit_line(u.b);
    if (!ok) {
        sb_gpu_power_off();
        (void)dialog_danger("GPU power-up failed", u.b, "The cores are off again.", "OK");
    }
    return ok;
}

static void gpu_power_down(void)
{
    if (!sb_gpu_is_open())
        return;
    sb_gpu_power_off();
    sb_gpu_close();
    uint8_t now = 0;
    if (g_buck_known && buck6(&now) && now != g_buck_was) {
        const char *why;
        int t = pmic_tier(1, BUCK6, g_buck_was, &why);
        int ok = 0;
        if (t <= g_auth.tier)
            ok = lab_pmic(1, BUCK6, g_buck_was, &g_auth);
        if (!ok) {
            struct sb_auth au;
            char v[16];
            struct ub u;
            ub_init(&u);
            ub_hex(&u, g_buck_was, 2);
            sb_strcpy(v, sizeof(v), u.b);
            struct action a = { "vdd_g3d back to S-Boot's value", "GPU",
                                { "BUCK6 back to the value it had before the GPU tests", 0, 0, 0 },
                                "the next OS sets vdd_g3d itself", "0x68", v, "", t, SB_SP_PMIC, 0, 0,
                                1U << 8 | BUCK6, 1 };
            if (lab_confirm(&a, &au))
                ok = lab_pmic(1, BUCK6, g_buck_was, &au);
        }
        audit_line(ok ? "GPU powered down, vdd_g3d back to S-Boot's value" : "GPU powered down, vdd_g3d left at 950 mV");
    } else {
        audit_line("GPU powered down");
    }
}

static int need_up(void)
{
    if (sb_gpu_is_up())
        return 1;
    if (!dialog("Power up the GPU first?", "The tests need the GPU powered (vdd_g3d 950 mV, cores on).",
                "Tier 1: a warning and a slide.", "POWER UP"))
        return 0;
    return gpu_power_up();
}

/* After a failed job: reset, cores, AS0 (the tables are still there). */
static int recover(void)
{
    unsigned us[4];
    int ok = sb_gpu_up(g_shaders, us);
    audit_line(ok ? "GPU reset after a failed job: up again" : "GPU reset after a failed job FAILED");
    return ok;
}

/* ---- (a) identification -------------------------------------------------------------------------- */

static void fill_ident(void)
{
    struct ub u;
    uint32_t id = sb_gpu_rd(0x000);
    kv_hdr("GPU_ID");
    ub_init(&u);
    ub_hex(&u, id, 8);
    ub_s(&u, (id >> 16) == 0x6221U ? "  Mali-G72" : "  NOT a Mali-G72");
    ub_s(&u, " r");
    ub_dec(&u, (id >> 12) & 0xfU);
    ub_c(&u, 'p');
    ub_dec(&u, (id >> 4) & 0xfU);
    kv_addc("Product", u.b, (id >> 16) == 0x6221U ? C_GREEN : C_RED);
    uint32_t sh = sb_gpu_rd(0x100), l2 = sb_gpu_rd(0x120), ti = sb_gpu_rd(0x110);
    ub_init(&u);
    ub_hex(&u, sh, 8);
    ub_s(&u, "  ");
    ub_dec(&u, popcount(sh));
    ub_s(&u, popcount(sh) == 18 ? " cores (MP18)" : " cores (an MP18 has 18)");
    kv_addc("Shader cores", u.b, popcount(sh) == 18 ? C_L1 : C_ORANGE);
    ub_init(&u);
    ub_hex(&u, l2, 8);
    ub_s(&u, ", tiler ");
    ub_hex(&u, ti, 8);
    kv_add("L2 / tiler present", u.b);
    uint32_t l2f = sb_gpu_rd(0x004);
    ub_init(&u);
    ub_hex(&u, l2f, 8);
    ub_s(&u, ": line ");
    ub_dec(&u, 1U << (l2f & 0xffU));
    ub_s(&u, " B, ");
    ub_dec(&u, (1U << ((l2f >> 16) & 0xffU)) >> 10);
    ub_s(&u, " KiB per slice");
    kv_add("L2_FEATURES", u.b);
    uint32_t mmu = sb_gpu_rd(0x014);
    ub_init(&u);
    ub_hex(&u, mmu, 8);
    ub_s(&u, ": ");
    ub_dec(&u, mmu & 0xffU);
    ub_s(&u, "-bit VA, ");
    ub_dec(&u, (mmu >> 8) & 0xffU);
    ub_s(&u, "-bit PA");
    kv_add("MMU_FEATURES", u.b);
    kv_hex("AS_PRESENT", sb_gpu_rd(0x018), 8);
    uint32_t js = sb_gpu_rd(0x01c);
    kv_hex("JS_PRESENT", js, 8);
    for (unsigned n = 0; n < 16; ++n)
        if (js & (1U << n)) {
            uint32_t f = sb_gpu_rd(0x0c0 + 4U * n);
            struct ub k;
            ub_init(&k);
            ub_s(&k, "  JS");
            ub_dec(&k, n);
            ub_s(&k, " features");
            ub_init(&u);
            ub_hex(&u, f, 8);
            ub_s(&u, (f & (1U << 9)) ? " fragment" : "");
            ub_s(&u, (f & (1U << 2)) ? " write-value" : "");
            ub_s(&u, (f & (1U << 4)) ? " compute" : "");
            ub_s(&u, (f & (1U << 7)) ? " tiler" : "");
            ub_s(&u, (f & (1U << 5)) ? " vertex" : "");
            kv_add(k.b, u.b);
        }
    kv_dec("THREAD_MAX_THREADS", sb_gpu_rd(0x0a0), "");
    kv_hex("CORE / TILER / MEM", (uint64_t)sb_gpu_rd(0x008) << 32 | sb_gpu_rd(0x00c), 16);
    kv_hex("COHERENCY_FEATURES", sb_gpu_rd(0x300), 8);
    kv_hdr("STATE");
    kv_hex("GPU_STATUS", sb_gpu_rd(0x034), 8);
    kv_hex("Shader / L2 ready", (uint64_t)sb_gpu_rd(0x140) << 32 | sb_gpu_rd(0x160), 16);
    kv_hex("GPU_IRQ_RAWSTAT", sb_gpu_rd(0x020), 8);
    kv_hdr("RESULT");
    kv_addc("Identification", g_detail[1][0] ? g_detail[1] : "not run", g_pass[1] > 0 ? C_GREEN : g_pass[1] == 0 ? C_RED : C_L3);
}

static void gpu_ident(void)
{
    struct ub u;
    uint32_t id = 0;
    if (!sb_gpu_domain() || !sb_probe32(SB_GPU, &id)) {
        g_result(1, 0, "G3D domain off or the GPU does not answer");
        info_screen("GPU identification", fill_ident, 0);
        return;
    }
    unsigned bad = 0;
    static const unsigned ro[] = { 0x000, 0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c, 0x0a0, 0x100, 0x110, 0x120 };
    for (unsigned i = 0; i < sizeof(ro) / sizeof(ro[0]); ++i)
        bad += sb_gpu_rd(ro[i]) != sb_gpu_rd(ro[i]);
    int g72 = (id >> 16) == 0x6221U, mp18 = popcount(sb_gpu_rd(0x100)) == 18;
    int slots = (sb_gpu_rd(0x01c) & 3U) == 3U, as0 = sb_gpu_rd(0x018) & 1U;
    ub_init(&u);
    ub_s(&u, g72 ? "Mali-G72" : "not a G72");
    ub_s(&u, mp18 ? " MP18" : " (core count differs)");
    if (bad) {
        ub_s(&u, ", ");
        ub_dec(&u, bad);
        ub_s(&u, " ID registers unstable");
    }
    int pass = g72 && !bad && slots && as0;
    if (sb_gpu_is_up()) {
        /* IRQ masks: written and read back (interrupts are not wired to the CPU here) */
        int rw = sb_gpu_wr(0x1008, 0x00070007U) && sb_gpu_rd(0x1008) == 0x00070007U && sb_gpu_wr(0x1008, 0) &&
                 sb_gpu_rd(0x1008) == 0 && sb_gpu_wr(0x2008, 1) && sb_gpu_rd(0x2008) == 1 && sb_gpu_wr(0x2008, 0);
        ub_s(&u, rw ? ", IRQ masks r/w ok" : ", IRQ mask read-back FAILED");
        pass &= rw;
        /* the real clock: the GPU's cycle counter over 20 ms of the CPU's timer */
        sb_gpu_cycles(1);
        uint64_t c0 = sb_gpu_cycle_count(), g0 = sb_gpu_timestamp(), t0 = sb_ticks();
        sb_mdelay(20);
        uint64_t c1 = sb_gpu_cycle_count(), g1 = sb_gpu_timestamp(), t1 = sb_ticks();
        sb_gpu_cycles(0);
        uint64_t mhz = t1 > t0 ? (c1 - c0) * sb_freq() / (t1 - t0) / 1000000U : 0;
        ub_s(&u, ", cycle counter ");
        if (c1 > c0) {
            ub_dec(&u, mhz);
            ub_s(&u, " MHz (PLL ");
            ub_dec(&u, clock_mhz(0));
            ub_s(&u, ")");
        } else {
            ub_s(&u, "did not run");
        }
        ub_s(&u, g1 > g0 && (g1 - g0) * 10U >= (t1 - t0) * 9U && (g1 - g0) * 10U <= (t1 - t0) * 11U
                     ? ", timestamp = CPU counter"
                     : ", timestamp differs from the CPU counter");
    } else {
        ub_s(&u, " (power up for the r/w and clock checks)");
    }
    g_result(1, pass, u.b);
    info_screen("GPU identification", fill_ident, 0);
}

/* ---- (b) the job manager ---------------------------------------------------------------------------- */

#define JM_LINES 24
static char jm_key[JM_LINES][48], jm_line[JM_LINES][170];
static uint32_t jm_col[JM_LINES];
static unsigned jm_n;

/* one step: a short name and what happened (wrapped on the page), also in the log */
static void jm_kv(const char *k, const char *s, uint32_t c)
{
    if (jm_n < JM_LINES) {
        sb_strcpy(jm_key[jm_n], sizeof(jm_key[0]), k);
        sb_strcpy(jm_line[jm_n], sizeof(jm_line[0]), s);
        jm_col[jm_n++] = c;
    }
    struct ub u;
    ub_init(&u);
    ub_s(&u, "gpu: ");
    ub_s(&u, k);
    ub_s(&u, ": ");
    ub_s(&u, s);
    sb_log(u.b);
}

static void fill_jm(void)
{
    kv_bigc("job manager", g_pass[2] > 0 ? "PASS" : g_pass[2] == 0 ? "FAIL" : "-",
            g_pass[2] > 0 ? C_GREEN : g_pass[2] == 0 ? C_RED : C_L3);
    kv_add("Result", g_detail[2]);
    kv_hdr("STEPS");
    for (unsigned i = 0; i < jm_n; ++i)
        kv_addc(jm_key[i], jm_line[i], jm_col[i]);
    kv_note("Interrupts are not used (polled), but the job IRQ line is checked once at the GIC (SPI 282, "
            "pending bit). The deliberate fault writes to an unmapped GPU address: the MMU must report a "
            "translation fault and the GPU must come back after a reset.");
}

static void fill_busy(void)
{
    kv_hdr("RUNNING");
    kv_add("GPU", "jobs are running (bounded waits; hold Power to stop between steps)");
}

/* One WRITE_VALUE job at slot index k: output at O_OUT + 8 k. */
static void wv(unsigned k, unsigned index, uint64_t next, unsigned type, uint64_t imm)
{
    uint32_t w[16];
    mali_write_value(w, index, next, VA(O_OUT + 8U * k), type, imm);
    put(VA(O_JOBS + 64U * k), w, 16);
}

static volatile uint64_t *out64(unsigned k)
{
    return (volatile uint64_t *)(uintptr_t)SB_GPU_CPU(VA(O_OUT + 8U * k));
}

/* A clear-only fragment job (frag slot f, framebuffer slot b) of w x h at GPU VA base. */
static void frag(unsigned f, unsigned b, uint64_t next, unsigned w, unsigned h, uint64_t base, uint32_t stride,
                 uint32_t colour)
{
    uint32_t d[48], j[16];
    mali_fbd(d, VA(O_SAMPLES), w, h, base, stride, colour);
    put(VA(O_FBD + 256U * b), d, 48);
    mali_fragment(j, f + 1U, next, w, h, VA(O_FBD + 256U * b));
    put(VA(O_FRAG + 64U * f), j, 16);
}

static int jm_check(int ok, const struct sb_gpu_result *r, const char *what, const char *detail, int *all)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, ok ? "ok, " : "FAIL, ");
    ub_dec(&u, r->us);
    ub_s(&u, " us");
    if (detail && detail[0]) {
        ub_s(&u, ", ");
        ub_s(&u, detail);
    }
    if (!ok || !r->ok) {
        ub_s(&u, "; ");
        ub_fault(&u, r);
    }
    jm_kv(what, u.b, ok ? C_GREEN : C_RED);
    if (!ok)
        *all = 0;
    return ok;
}

static void gpu_jm(void)
{
    struct sb_gpu_result r;
    struct ub u;
    int all = 1;
    jm_n = 0;
    if (!need_up())
        return;
    show_now("GPU job manager", fill_busy);
    lab_stop_reset();
    uint32_t aff = g_shaders, js = sb_gpu_rd(0x01c);
    int wv_slot = -1;
    /* 1. every slot */
    for (unsigned n = 0; n < 16 && !lab_stop_check(); ++n) {
        if (!(js & (1U << n)))
            continue;
        uint32_t f = sb_gpu_rd(0x0c0 + 4U * n);
        ub_init(&u);
        ub_s(&u, "JS");
        ub_dec(&u, n);
        if (f & (1U << 2)) {
            uint64_t want = 0x5354415242495445ULL ^ n;
            *out64(0) = 0xa5a5a5a55a5a5a5aULL;
            wv(0, 1, 0, MALI_WV_IMM64, want);
            int ok = sb_gpu_run(n, VA(O_JOBS), aff, 200000, &r) && *out64(0) == want;
            ub_s(&u, " WRITE_VALUE");
            jm_check(ok, &r, u.b, "the value checked", &all);
            if (ok && wv_slot <= 0)             /* prefer a slot other than JS0 (the fragment slot) */
                wv_slot = (int)n;
            if (!r.ok)
                (void)recover();
        } else if (f & (1U << 9)) {
            volatile uint32_t *px = cpu32(VA(O_PIX));
            for (unsigned i = 0; i < 16 * 16; ++i)
                px[i] = MARK;
            frag(0, 0, 0, 16, 16, VA(O_PIX), 64, 0xff112233U);
            int ok = sb_gpu_run(n, VA(O_FRAG), aff, 200000, &r);
            unsigned good = 0;
            for (unsigned i = 0; i < 16 * 16; ++i)
                good += px[i] == 0xff112233U;
            ok = ok && good == 256;
            ub_s(&u, " fragment 16x16");
            jm_check(ok, &r, u.b, "this slot takes no WRITE_VALUE; 256 pixels checked", &all);
            if (!r.ok)
                (void)recover();
        } else {
            struct ub v;
            ub_init(&v);
            ub_s(&v, "features ");
            ub_hex(&v, f, 8);
            ub_s(&v, ": no job type tested here");
            jm_kv(u.b, v.b, C_L2);
        }
    }
    if (wv_slot < 0) {
        g_result(2, 0, "no job slot ran a WRITE_VALUE job");
        info_screen("GPU job manager", fill_jm, 0);
        return;
    }
    unsigned s = (unsigned)wv_slot;
    /* 2. every value type, as one chain of 6 */
    for (unsigned k = 0; k < 6; ++k)
        *out64(k) = 0xa5a5a5a55a5a5a5aULL;
    uint64_t t0 = sb_ticks();
    wv(0, 1, VA(O_JOBS + 64), MALI_WV_ZERO, 0);
    wv(1, 2, VA(O_JOBS + 128), MALI_WV_IMM8, 0x11);
    wv(2, 3, VA(O_JOBS + 192), MALI_WV_IMM16, 0x2222);
    wv(3, 4, VA(O_JOBS + 256), MALI_WV_IMM32, 0x33333333U);
    wv(4, 5, VA(O_JOBS + 320), MALI_WV_IMM64, 0x4444444444444444ULL);
    wv(5, 6, 0, MALI_WV_TIMESTAMP, 0);
    int ok = sb_gpu_run(s, VA(O_JOBS), aff, 200000, &r);
    uint64_t t1 = sb_ticks();
    uint64_t ts = *out64(5);
    int types = ok && *out64(0) == 0 && *out64(1) == 0xa5a5a5a55a5a5a11ULL && *out64(2) == 0xa5a5a5a55a5a2222ULL &&
                *out64(3) == 0xa5a5a5a533333333ULL && *out64(4) == 0x4444444444444444ULL;
    int ts_ok = ts >= t0 && ts <= t1;
    jm_check(types, &r, "Value types (one chain of 6)", "zero, imm8 / 16 / 32 / 64 with the bytes around kept", &all);
    ub_init(&u);
    ub_dec(&u, ts);
    ub_s(&u, ts_ok ? " (inside the CPU's window)" : " (outside the CPU's window: another clock)");
    jm_kv("System timestamp job", u.b, ts_ok ? C_GREEN : C_ORANGE);
    if (!r.ok)
        (void)recover();
    /* 3. a chain of 128 */
    for (unsigned k = 0; k < 128; ++k) {
        *out64(k) = 0;
        wv(k, k + 1U, k < 127 ? VA(O_JOBS + 64U * (k + 1U)) : 0, MALI_WV_IMM64, 0x1000000000000000ULL + k * 0x10001ULL);
    }
    ok = sb_gpu_run(s, VA(O_JOBS), aff, 500000, &r);
    unsigned good = 0;
    for (unsigned k = 0; k < 128; ++k)
        good += *out64(k) == 0x1000000000000000ULL + k * 0x10001ULL;
    ub_init(&u);
    ub_dec(&u, good);
    ub_s(&u, "/128 values right, ");
    ub_dec(&u, r.us ? 128U * 1000000U / r.us : 0);
    ub_s(&u, " jobs/s");
    jm_check(ok && good == 128, &r, "A chain of 128 WRITE_VALUE", u.b, &all);
    if (!r.ok)
        (void)recover();
    /* 4. the job interrupt at the GIC (GICD_ISPENDR of SPI 282 = INTID 314: 0x10101224 bit 26) */
    sb_gpu_wr(0x1004, 0xffffffffU);
    sb_gpu_wr(0x1008, 1U << s);
    *out64(0) = 0;
    wv(0, 1, 0, MALI_WV_IMM64, 7);
    uint32_t pend = 0, after = 0;
    if (sb_gpu_submit(s, VA(O_JOBS), aff)) {
        uint64_t w0 = sb_ticks();
        while (!(sb_gpu_rd(0x1000) & ((1U << s) | (1U << (s + 16)))) && sb_ticks() - w0 < sb_freq() / 5U)
            ;
        sb_udelay(5);
        pend = *(volatile uint32_t *)0x10101224UL;
        (void)sb_gpu_wait(1U << s, 1000, &r);
        sb_gpu_wr(0x1008, 0);
        sb_udelay(5);
        after = *(volatile uint32_t *)0x10101224UL;
    }
    ub_init(&u);
    ub_s(&u, (pend >> 26) & 1U ? "pending while the job was done" : "NOT seen pending");
    ub_s(&u, (after >> 26) & 1U ? ", still pending after the clear" : ", gone after the clear");
    jm_kv("Job IRQ line at the GIC", u.b, (pend >> 26) & 1U ? C_GREEN : C_ORANGE);
    /* 5. a deliberate MMU fault, then the recovery */
    uint32_t w[16];
    mali_write_value(w, 1, 0, SB_GPU_HOLE_VA, MALI_WV_IMM64, 1);
    put(VA(O_JOBS), w, 16);
    ok = sb_gpu_run(s, VA(O_JOBS), aff, 100000, &r);
    int fault_seen = !ok && (r.mirq || (r.jirq >> 16));
    ub_init(&u);
    ub_s(&u, fault_seen ? "reported: " : ok ? "NOT reported (the job passed): " : "not reported (timeout): ");
    ub_fault(&u, &r);
    jm_kv("Deliberate fault (a write to GPU VA 0x10000000)", u.b, fault_seen ? C_GREEN : C_RED);
    int rec = recover();
    *out64(0) = 0;
    wv(0, 1, 0, MALI_WV_IMM64, 0x600dU);
    int back = rec && sb_gpu_run(s, VA(O_JOBS), aff, 200000, &r) && *out64(0) == 0x600dU;
    jm_kv("Recovery", back ? "soft reset, cores, AS0: a job runs again" : "the GPU did NOT come back after the reset",
          back ? C_GREEN : C_RED);
    all = all && fault_seen && back;
    ub_init(&u);
    ub_s(&u, all ? "every slot, every type, 128-job chain, fault recovery" : "see the steps");
    ub_s(&u, "; WRITE_VALUE slot JS");
    ub_dec(&u, s);
    g_result(2, all, u.b);
    info_screen("GPU job manager", fill_jm, 0);
}

/* ---- (c) the fill pattern -------------------------------------------------------------------------- */

static uint32_t palette(unsigned i)
{
    static const uint32_t base[8] = { 0xffe6194bU, 0xff3cb44bU, 0xffffe119U, 0xff4363d8U,
                                      0xfff58231U, 0xff911eb4U, 0xff46f0f0U, 0xfff032e6U };
    uint32_t c = base[i % 8U];
    unsigned shade = (i / 8U) % 8U;                 /* darker by row */
    uint32_t r = (c >> 16) & 0xffU, g = (c >> 8) & 0xffU, b = c & 0xffU;
    r = r * (8U - shade / 2U) / 8U;
    g = g * (8U - shade / 2U) / 8U;
    b = b * (8U - shade / 2U) / 8U;
    return 0xff000000U | r << 16 | g << 8 | b | (i & 1U);
}

static uint32_t swap_rb(uint32_t c)
{
    return (c & 0xff00ff00U) | (c >> 16 & 0xffU) | (c & 0xffU) << 16;
}

static char fill_lines[8][170];
static unsigned fill_n;

static void fill_note(const char *s)
{
    if (fill_n < 8)
        sb_strcpy(fill_lines[fill_n++], sizeof(fill_lines[0]), s);
    sb_logf("gpu: ", s);
}

/* Paint a grid of n x n blocks of bs pixels at GPU VA base (stride bytes); chained or not. */
static int paint(uint64_t base, uint32_t stride, unsigned n, unsigned bs, int chained, struct sb_gpu_result *r,
                 uint64_t *us)
{
    unsigned jobs = n * n;
    *us = 0;
    for (unsigned j = 0; j < jobs; ++j) {
        unsigned bx = j % n, by = j / n;
        uint64_t b = base + (uint64_t)by * bs * stride + (uint64_t)bx * bs * 4U;
        frag(j, j, chained && j + 1 < jobs ? VA(O_FRAG + 64U * (j + 1U)) : 0, bs, bs, b, stride, palette(j));
    }
    if (chained) {
        int ok = sb_gpu_run(0, VA(O_FRAG), g_shaders, 1000000, r);
        *us = r->us;
        return ok;
    }
    for (unsigned j = 0; j < jobs; ++j) {
        if (!sb_gpu_run(0, VA(O_FRAG + 64U * j), g_shaders, 200000, r))
            return 0;
        *us += r->us;
    }
    return 1;
}

/* Every pixel of the grid against its block's colour: mismatches, the first one, R/B swaps. */
static unsigned verify(volatile uint32_t *px, uint32_t stride_px, unsigned n, unsigned bs, unsigned *swapped,
                       char *first)
{
    unsigned bad = 0;
    *swapped = 0;
    first[0] = 0;
    for (unsigned y = 0; y < n * bs; ++y)
        for (unsigned x = 0; x < n * bs; ++x) {
            uint32_t want = palette((y / bs) * n + x / bs), got = px[(uint64_t)y * stride_px + x];
            if (got == want)
                continue;
            ++bad;
            *swapped += got == swap_rb(want);
            if (!first[0]) {
                struct ub u;
                ub_init(&u);
                ub_s(&u, "first at (");
                ub_dec(&u, x);
                ub_c(&u, ',');
                ub_dec(&u, y);
                ub_s(&u, "): ");
                ub_hex(&u, got, 8);
                ub_s(&u, " want ");
                ub_hex(&u, want, 8);
                sb_strcpy(first, 64, u.b);
            }
        }
    return bad;
}

static void fill_fill(void)
{
    kv_bigc("fill pattern", g_pass[3] > 0 ? "PASS" : g_pass[3] == 0 ? "FAIL" : "-",
            g_pass[3] > 0 ? C_GREEN : g_pass[3] == 0 ? C_RED : C_L3);
    kv_add("Result", g_detail[3]);
    kv_hdr("STEPS");
    for (unsigned i = 0; i < fill_n; ++i)
        kv_add("", fill_lines[i]);
    kv_note("Clear-only fragment jobs (no shaders): each block is its own framebuffer with its own clear "
            "colour. The CPU checks every pixel; a red / blue swap is reported as a descriptor channel-order "
            "issue, not a GPU fault.");
}

static int paint_and_check(const char *what, uint64_t base, uint32_t stride, unsigned n, unsigned bs, int chained)
{
    struct sb_gpu_result r;
    struct ub u;
    char first[64];
    volatile uint32_t *px = cpu32(base);
    uint32_t spx = stride / 4U;
    for (unsigned y = 0; y < n * bs; ++y)
        for (unsigned x = 0; x < n * bs; ++x)
            px[(uint64_t)y * spx + x] = MARK;
    uint64_t us = 0;
    int ran = paint(base, stride, n, bs, chained, &r, &us);
    unsigned sw = 0, bad = verify(px, spx, n, bs, &sw, first);
    ub_init(&u);
    ub_s(&u, what);
    ub_s(&u, ran && !bad ? ": ok, " : ": FAIL, ");
    ub_dec(&u, (uint64_t)n * n * bs * bs - bad);
    ub_c(&u, '/');
    ub_dec(&u, (uint64_t)n * n * bs * bs);
    ub_s(&u, " px, ");
    ub_dec(&u, us);
    ub_s(&u, " us");
    fill_note(u.b);
    if (!ran) {
        ub_init(&u);
        ub_s(&u, "  ");
        ub_fault(&u, &r);
        fill_note(u.b);
        (void)recover();
    } else if (bad) {
        ub_init(&u);
        ub_s(&u, "  ");
        ub_s(&u, first);
        if (sw == bad)
            ub_s(&u, " (every one R/B swapped)");
        fill_note(u.b);
    }
    return ran && !bad;
}

static void gpu_fill(void)
{
    struct ub u;
    fill_n = 0;
    if (!need_up())
        return;
    show_now("GPU fill pattern", fill_busy);
    int single = paint_and_check("256x256, 64 jobs one by one", VA(O_PIX), 256U * 4U, 8, 32, 0);
    int chain = paint_and_check("256x256, 64 jobs as one chain", VA(O_PIX), 256U * 4U, 8, 32, 1);
    g_chain_ok = chain;
    /* on the screen: 512 x 512 at (464, 1216) (16-aligned), straight into the scanout */
    const unsigned px0 = 464, py0 = 1216;
    uint64_t base = SB_GPU_PANEL_VA + (uint64_t)py0 * SB_FB_W * 4U + px0 * 4U;
    ui_fill(0, UI_VIEW_Y0, UI_W, UI_VIEW_Y1 - UI_VIEW_Y0, C_BG);
    ui_text_c(F_ROW, UI_W / 2, UI_VIEW_Y0 + 200, "The Mali-G72 draws the square below", C_L1);
    ui_text_c(F_SMALL, UI_W / 2, UI_VIEW_Y0 + 280, "64 fragment jobs, no CPU drawing; any key or tap returns",
              C_L2);
    ui_present();
    int panel = paint_and_check("screen 512x512, 64 jobs", base, SB_FB_W * 4U, 8, 64, 0);
    ui_text_c(F_ROW, UI_W / 2, py0 + 512 + 120, panel ? "read back from the scanout: every pixel right"
                                                       : "read back from the scanout: WRONG pixels",
              panel ? C_GREEN : C_RED);
    ui_dirty(0, py0 + 512 + 40, UI_W, 120);
    ui_present();
    ui_input_reset();
    lab_stop_reset();
    for (uint64_t t0 = sb_ms(); sb_ms() - t0 < 8000;) {
        struct ui_ev e;
        ui_poll(&e, 50);
        if (e.type != EV_NONE && e.type != EV_DRAG && e.type != EV_PRESS)
            break;
    }
    ui_input_reset();
    ui_dirty_all();
    E.redraw = 1;
    ub_init(&u);
    ub_s(&u, single ? "single jobs ok" : "single jobs FAIL");
    ub_s(&u, chain ? ", chain ok" : ", chain FAIL");
    ub_s(&u, panel ? ", screen ok" : ", screen FAIL");
    g_result(3, single && panel, u.b);
    info_screen("GPU fill pattern", fill_fill, 0);
}

/* ---- (d) stress ---------------------------------------------------------------------------------- */

static const unsigned gs_dur_s[] = { 30, 60, 300, 900, 3600, 0 };
static const char *const gs_dur_name[] = { "30 s", "1 min", "5 min", "15 min", "60 min", "until stopped" };
static const unsigned gs_side[] = { 512, 1024 };
static int gs_d = 1, gs_sz = 1, gs_limit = 3;
static uint64_t gs_t0, gs_frags, gs_wv, gs_px, gs_rounds, gs_bad;
static unsigned gs_mpx, gs_jobs_s, gs_mhz, gs_mv10;
static int gs_stop, gs_peak, gs_gpu_c;
static struct sb_gpu_result gs_fault;
static char gs_detail[110];
static struct sb_temps GT;

static void fill_gs_live(void)
{
    struct ub u;
    uint64_t s = (sb_ms() - gs_t0) / 1000U;
    ub_init(&u);
    ub_dec(&u, s / 60U);
    ub_c(&u, ':');
    ub_dec2(&u, (unsigned)(s % 60U));
    kv_bigc(gs_bad ? "ERRORS FOUND" : "running, no errors", u.b, gs_bad ? C_RED : C_GREEN);
    ub_init(&u);
    ub_dec(&u, gs_mpx);
    ub_s(&u, " Mpixel/s, ");
    ub_dec(&u, gs_jobs_s);
    ub_s(&u, " jobs/s");
    kv_add("Throughput", u.b);
    ub_init(&u);
    ub_dec(&u, gs_rounds);
    ub_s(&u, " rounds, ");
    ub_dec(&u, gs_frags);
    ub_s(&u, " fragment + ");
    ub_dec(&u, gs_wv);
    ub_s(&u, " write-value jobs");
    kv_add("Done", u.b);
    ub_init(&u);
    if (gs_gpu_c > -100) {
        ub_sdec(&u, gs_gpu_c);
        ub_s(&u, " " U_DEG "C GPU zone, hottest ");
        ub_sdec(&u, GT.hottest);
        ub_s(&u, " " U_DEG "C");
    } else {
        ub_s(&u, "GPU zone: no valid sample");
    }
    kv_addc("Temperature", u.b, gs_gpu_c >= 85 ? C_RED : gs_gpu_c >= 65 ? C_ORANGE : C_L1);
    ub_init(&u);
    ub_dec(&u, gs_mhz);
    ub_s(&u, " MHz, ");
    ub_dec(&u, gs_mv10 / 10U);
    ub_c(&u, '.');
    ub_dec(&u, gs_mv10 % 10U);
    ub_s(&u, " mV");
    kv_add("Clock, vdd_g3d", u.b);
    kv_addc("Stop at", lab_limit_label(gs_limit), gs_limit == LIMIT_NONE ? C_ORANGE : C_L1);
    lab_chart_kv(0, 420);
    lab_chart_kv(1, 420);
    kv_note("Hold Power to stop. Each round: one full-buffer fragment job on JS0 and a chain of 64 WRITE_VALUE "
            "jobs on JS1 at the same time; 64 pixels and every value checked.");
}

static void fill_gs_result(void)
{
    struct ub u;
    kv_bigc(gs_stop == 2 ? "stopped at the temperature limit" : gs_stop == 1 ? "stopped by hand" :
            gs_stop == 4 ? "a job faulted" : gs_stop == 5 ? "wrong results" : "finished",
            g_pass[4] > 0 ? "PASS" : "FAIL", g_pass[4] > 0 ? C_GREEN : C_RED);
    kv_add("Result", gs_detail);
    ub_init(&u);
    ub_sdec(&u, gs_peak);
    ub_s(&u, " " U_DEG "C (GPU zone)");
    kv_add("Peak", u.b);
    if (gs_stop == 4) {
        ub_init(&u);
        ub_fault(&u, &gs_fault);
        kv_addc("Fault", u.b, C_RED);
    }
    lab_chart_kv(0, 420);
    lab_chart_kv(1, 420);
}

static void gs_sample(void)
{
    uint8_t v = 0;
    static unsigned n;
    sb_temps_read(&GT);
    gs_gpu_c = GT.gpu;
    if (gs_gpu_c > gs_peak)
        gs_peak = gs_gpu_c;
    gs_mhz = clock_mhz(0);
    if (n++ % 5 == 0 && buck6(&v))
        gs_mv10 = (300000U + v * 6250U) / 100U;
}

/* The stress loop: dur seconds (0 until stopped); page: the result page and the GPU results list.
 * 1 pass, 0 fail, -1 not run or stopped by hand. */
static int gs_run(unsigned dur, int page)
{
    struct sb_gpu_result r;
    struct ub u;
    if (!sb_gpu_is_up())
        return -1;
    unsigned side = gs_side[gs_sz], ws = 0;
    uint32_t stride = side * 4U, jsp = sb_gpu_rd(0x01c);
    for (unsigned n = 1; n < 16 && !ws; ++n)        /* a slot besides JS0 that takes WRITE_VALUE */
        if ((jsp & (1U << n)) && (sb_gpu_rd(0x0c0 + 4U * n) & (1U << 2)))
            ws = n;
    sb_temps_read(&GT);
    if (!GT.sub && gs_limit != LIMIT_NONE &&
        !dialog_danger("GPU stress", "The GPU's temperature sensor (TMU_SUB) is off: no limit can work.",
                       "Run it only while you watch the phone.", "RUN ANYWAY"))
        return -1;
    lab_chart_reset(0, "GPU ZONE (" U_DEG "C)", C_ORANGE, dur ? dur : 3600);
    lab_chart_reset(1, "MPIXEL PER SECOND", C_GREEN, dur ? dur : 3600);
    gs_frags = gs_wv = gs_px = gs_rounds = gs_bad = 0;
    gs_mpx = gs_jobs_s = 0;
    gs_stop = 0;
    gs_peak = -100;
    gs_mv10 = 9500;
    gs_sample();
    lab_live_begin("GPU stress");
    gs_t0 = sb_ms();
    uint64_t next = sb_ms() + 1000, px_last = 0, jobs_last = 0;
    unsigned blind = 0;
    uint32_t seed = 0x13579bdfU;
    volatile uint32_t *px = cpu32(VA(O_PIX));
    while (!gs_stop) {
        uint32_t colour = 0xff000000U | (uint32_t)(gs_rounds * 2654435761U >> 8);
        frag(0, 0, 0, side, side, VA(O_PIX), stride, colour);
        for (unsigned k = 0; k < 64; ++k)
            wv(k, k + 1U, k < 63 ? VA(O_JOBS + 64U * (k + 1U)) : 0, MALI_WV_IMM64, gs_rounds << 8 | k);
        int ok;
        if (ws)                                 /* both slots at once */
            ok = sb_gpu_submit(0, VA(O_FRAG), g_shaders) && sb_gpu_submit(ws, VA(O_JOBS), g_shaders) &&
                 sb_gpu_wait(1U | 1U << ws, 500000, &r);
        else                                    /* one slot takes both: one after the other */
            ok = sb_gpu_run(0, VA(O_FRAG), g_shaders, 500000, &r) && sb_gpu_run(0, VA(O_JOBS), g_shaders, 500000, &r);
        if (!ok) {
            gs_fault = r;
            gs_stop = 4;
            (void)recover();
            break;
        }
        for (unsigned k = 0; k < 64; ++k)
            gs_bad += *out64(k) != (gs_rounds << 8 | k);
        for (unsigned k = 0; k < 64; ++k) {
            seed = seed * 1664525U + 1013904223U;
            gs_bad += px[(seed >> 8) % (side * side)] != colour;
        }
        ++gs_rounds;
        ++gs_frags;
        gs_wv += 64;
        gs_px += (uint64_t)side * side;
        if (gs_bad) {
            gs_stop = 5;
            break;
        }
        if (lab_live_poll(fill_gs_live, 250))
            gs_stop = 1;
        if (sb_ms() >= next) {
            next += 1000;
            gs_mpx = (unsigned)((gs_px - px_last) / 1000000U);
            gs_jobs_s = (unsigned)(gs_frags + gs_wv - jobs_last);
            px_last = gs_px;
            jobs_last = gs_frags + gs_wv;
            gs_sample();
            lab_chart_push(0, gs_gpu_c);
            lab_chart_push(1, (int32_t)gs_mpx);
            int hot = GT.hottest > gs_gpu_c ? GT.hottest : gs_gpu_c;
            blind = hot <= -100 && (GT.top || GT.sub) ? blind + 1 : 0;
            if (gs_limit != LIMIT_NONE && (hot >= lab_limit_c(gs_limit) || blind >= 3)) {
                if (blind >= 3)
                    audit_line("GPU stress: no valid TMU sample for 3 s: stopped (the limit cannot see)");
                gs_stop = 2;
            }
        }
        if (dur && sb_ms() - gs_t0 >= dur * 1000U)
            gs_stop = 3;
    }
    lab_live_end();
    uint64_t secs = (sb_ms() - gs_t0) / 1000U;
    ub_init(&u);
    ub_dec(&u, gs_rounds);
    ub_s(&u, " rounds in ");
    ub_dec(&u, secs);
    ub_s(&u, " s, ");
    ub_dec(&u, secs ? gs_px / 1000000U / secs : 0);
    ub_s(&u, " Mpx/s, ");
    ub_dec(&u, gs_bad);
    ub_s(&u, " errors, peak ");
    ub_sdec(&u, gs_peak);
    ub_s(&u, " C");
    if (gs_stop == 2)
        ub_s(&u, " (limit)");
    if (gs_stop == 4)
        ub_s(&u, ", FAULT");
    sb_strcpy(gs_detail, sizeof(gs_detail), u.b);
    int pass = !gs_bad && gs_stop != 4 && gs_rounds > 0;
    if (!page)                              /* -2: the temperature limit, not an error */
        return gs_stop == 1 ? -1 : gs_stop == 2 && !gs_bad ? -2 : pass && gs_stop == 3;
    g_result(4, pass, u.b);
    info_screen("GPU stress result", fill_gs_result, 0);
    return pass;
}

static void gpu_stress_run(void)
{
    if (need_up())
        (void)gs_run(gs_dur_s[gs_d], 1);
}

/* ---- overclocking (oc.c) ------------------------------------------------------------------------------ */

/* Before a G3D step: the GPU powered at its present clock (the tier 1 power-up, vdd_g3d 950 mV), so
 * the step's own ACPM voltage is the last voltage written. 1 up. */
int lab_oc_gpu_prepare(void)
{
    return need_up();
}

int lab_oc_gpu_up(void)
{
    return sb_gpu_is_up();
}

/* The stress rounds for secs at the clock ACPM set (vdd_g3d is not touched): 1 pass, 0 fail, -1 not
 * run. detail: the summary (rounds, errors, the fault). */
int lab_oc_gpu(unsigned secs, char *detail, unsigned cap)
{
    int sz = gs_sz, lim = gs_limit;
    gs_sz = 0;                              /* 512 x 512: a round stays short at any clock */
    if (gs_limit == LIMIT_NONE || gs_limit < 5)
        gs_limit = 5;                       /* 95 C for this run (as the CPU test); never "no limit" */
    int r = gs_run(secs, 0);
    gs_sz = sz;
    gs_limit = lim;
    struct ub u;
    ub_init(&u);
    ub_s(&u, gs_detail);
    if (gs_stop == 4) {
        ub_s(&u, ": ");
        ub_fault(&u, &gs_fault);
    }
    sb_strcpy(detail, cap, u.b);
    return r;
}

static void build_gs(void)
{
    struct ub u;
    kv_hdr("OPTIONS (POWER CHANGES)");
    row_value("Duration", gs_dur_name[gs_d], 0);
    ub_init(&u);
    ub_dec(&u, gs_side[gs_sz]);
    ub_s(&u, " x ");
    ub_dec(&u, gs_side[gs_sz]);
    ub_s(&u, " per fragment job");
    row_value("Buffer", u.b, 0);
    row_value("Stop at", lab_limit_label(gs_limit), gs_limit == LIMIT_NONE ? R_RED : 0);
    kv_hdr("RUN");
    row_add("Start", "hold Power to stop", R_WARN);
    row_add("Last result", g_pass[4] >= 0 ? g_detail[4] : "not run", g_pass[4] >= 0 ? 0 : R_DIS);
    kv_note("The limit watches the GPU's TMU zone (TMU_SUB probe 1) and every other probe. The clock and the "
            "voltage stay what GPU clock set (260 MHz at 950 mV unless you changed it).");
}

static int act_gs(int i)
{
    switch (i) {
    case 0: gs_d = (gs_d + 1) % 6; break;
    case 1: gs_sz = (gs_sz + 1) % 2; break;
    case 2: gs_limit = lab_limit_next(gs_limit); break;
    case 3: gpu_stress_run(); break;
    case 4: info_screen("GPU stress result", fill_gs_result, 0); break;
    default: break;
    }
    return 0;
}

/* ---- the GPU clock (ACPM DVFS, tier 3) -------------------------------------------------------------- */

static void gpu_clock(void)
{
    static const char *const lbl[GPU_NLEVELS] = {
        "260 MHz (S-Boot's; jobs ran here)", "299 MHz (vendor table, never run here)",
        "338 MHz (vendor table, never run here)", "455 MHz (vendor table, never run here)",
        "546 MHz (vendor table, never run here)", "572 MHz (ran under Starbite's iOS at 950 mV)",
    };
    int cur = 0, mhz = (int)clock_mhz(0);
    for (int i = 0; i < GPU_NLEVELS; ++i)
        if ((int)(gpu_levels_khz[i] / 1000U) == mhz)
            cur = i;
    int pick = ask_pick("GPU clock", lbl, GPU_NLEVELS, cur);
    if (pick < 0)
        return;
    unsigned khz = gpu_levels_khz[pick];
    const char *why;
    struct sb_auth au;
    char tgt[16], nowv[24], newv[24];
    struct ub u;
    ub_init(&u);
    ub_dec(&u, (uint64_t)mhz);
    ub_s(&u, " MHz");
    sb_strcpy(nowv, sizeof(nowv), u.b);
    ub_init(&u);
    ub_dec(&u, khz / 1000U);
    ub_s(&u, " MHz");
    sb_strcpy(newv, sizeof(newv), u.b);
    sb_strcpy(tgt, sizeof(tgt), "G3D");
    struct action a = { "GPU clock (ACPM DVFS)", tgt,
                        { "ACPM DVFS request: domain g3d (4), a level of the vendor table (exynos9810.dtsi)",
                          "ACPM sets PLL_G3D and, from its own table, may change vdd_g3d: checked afterwards",
                          "Not locked afterwards: the request goes back to 260 MHz", 0 },
                        "GPU clock 260 MHz, or a reboot", nowv, newv, "", 0, SB_SP_DVFS, 0, 0, 4, 1 };
    a.tier = dvfs_tier(4, khz, &why);
    if (!lab_confirm(&a, &au) || !sb_gate_open(&au))
        return;
    int sent = sb_acpm_dvfs(4, khz, a.tier);
    sb_udelay(1000);
    int locked = 0;
    unsigned after = clock_mhz(&locked);
    if (sent && !locked)
        (void)sb_acpm_dvfs(4, 260000, a.tier);
    sb_gate_close();
    ub_init(&u);
    ub_s(&u, sent ? "ACPM answered; PLL_G3D " : "ACPM did NOT answer; PLL_G3D ");
    ub_dec(&u, after);
    ub_s(&u, locked ? " MHz locked" : " MHz NOT locked: asked for 260 MHz again");
    audit_line(u.b);
    lab_major_result("GPU clock (ACPM DVFS)", "G3D", u.b);
    uint8_t v = 0;
    if (sb_gpu_is_open() && buck6(&v) && v != BUCK6_950) {
        struct ub m;
        ub_init(&m);
        ub_s(&m, "ACPM set vdd_g3d to ");
        ub_mv(&m, v);
        ub_s(&m, "; jobs need 950 mV here.");
        if (dialog("vdd_g3d changed", m.b, "Put it back to 950 mV (tier 1)?", "950 mV"))
            (void)lab_pmic(1, BUCK6, BUCK6_950, &g_auth);
    }
    ui_toast(u.b, sent && locked ? C_GREEN : C_RED, 1500);
}

/* ---- the page ------------------------------------------------------------------------------------------ */

static void build_gpu(void)
{
    struct ub u;
    uint8_t v = 0;
    int locked = 0;
    struct sb_temps t;
    kv_hdr("STATE");
    kv_addc("G3D domain", sb_gpu_domain() ? "on" : "OFF", sb_gpu_domain() ? C_L1 : C_ORANGE);
    ub_init(&u);
    ub_dec(&u, clock_mhz(&locked));
    ub_s(&u, locked ? " MHz (PLL_G3D locked)" : " MHz (PLL_G3D NOT locked)");
    kv_addc("Clock", u.b, locked ? C_L1 : C_ORANGE);
    ub_init(&u);
    if (buck6(&v)) {
        ub_mv(&u, v);
        ub_s(&u, v == BUCK6_950 ? " (BUCK6 0x68)" : " (jobs need 950 mV)");
    } else {
        ub_s(&u, "ACPM did not answer");
    }
    kv_add("vdd_g3d", u.b);
    sb_temps_read(&t);
    ub_init(&u);
    if (t.gpu > -100) {
        ub_sdec(&u, t.gpu);
        ub_s(&u, " " U_DEG "C (TMU_SUB probe 1)");
    } else {
        ub_s(&u, "no valid sample");
    }
    kv_add("GPU temperature", u.b);
    kv_addc("GPU", sb_gpu_is_up() ? "powered: L2, shader cores, tiler, AS0" : "not powered", sb_gpu_is_up() ? C_GREEN : C_L2);
    kv_hdr("TESTS");
    row_add(sb_gpu_is_up() ? "Power down the GPU" : "Power up the GPU",
            sb_gpu_is_up() ? "cores off, vdd_g3d back" : "tier 1: vdd_g3d 950 mV, reset, cores, page tables",
            sb_gpu_is_up() ? 0 : R_WARN);
    row_add("Identification and registers", g_pass[1] >= 0 ? g_detail[1] : "GPU_ID, cores, L2, MMU, slots, clock", 0);
    row_add("Job manager test", g_pass[2] >= 0 ? g_detail[2] : "WRITE_VALUE on every slot, chains, IRQ, a fault", 0);
    row_add("Fill pattern test", g_pass[3] >= 0 ? g_detail[3] : "fragment jobs paint a pattern, also on the screen", 0);
    row_add("GPU stress", g_pass[4] >= 0 ? g_detail[4] : "jobs back to back: throughput, temperature, limit", R_WARN);
    row_add("GPU clock", "ACPM DVFS to a level of the vendor table (tier 3)", R_RED);
    kv_note("Without any OS: the recipe Starbite's iOS port ran on this phone (260 MHz, 950 mV). Interrupts are "
            "polled. Leaving this page powers the GPU down.");
}

static int act_gpu(int i)
{
    switch (i) {
    case 0:
        if (sb_gpu_is_up())
            gpu_power_down();
        else
            (void)gpu_power_up();
        break;
    case 1: gpu_ident(); break;
    case 2: gpu_jm(); break;
    case 3: gpu_fill(); break;
    case 4: list_screen("GPU stress", build_gs, act_gs); break;
    case 5: gpu_clock(); break;
    default: break;
    }
    return 0;
}

void lab_gpu(void)
{
    list_screen("GPU tests", build_gpu, act_gpu);
    gpu_power_down();
}
