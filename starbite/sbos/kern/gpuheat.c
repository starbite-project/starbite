/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS heater: the GPU's share on High (docs/STARBITE-OS.md 8.1), off until Settings allows it after its own
 * confirmation (vdd_g3d is a PMIC write; libsb's Mali job path has run on the host's fake Mali only).
 *
 * Bring-up, each step checked, any failure logged and the GPU left alone (the CPU load goes on):
 * the G3D power domain on (S-Boot leaves it on; never switched here), GPU_ID a Mali-G72, PLL_G3D locked, the TMU_SUB
 * hardware trip armed, vdd_g3d (S2MPS18 BUCK6) read in its known range and set to 950 mV (the value Starbite's iOS
 * and Linux's s9p-g3d run it at; read back), libsb's GPU session, page tables, reset and cores (sb_gpu_up), then
 * ACPM domain 4 to 572 MHz (a listed level; PLL read back). The load: chains of clear-only fragment jobs over a
 * 1024 x 1024 render target on job slot 0, resubmitted from the heater's 4 Hz tick, the chain doubled until one lasts
 * at least 200 ms; a few pixels checked after each chain. Never blocks: the tick reads JOB_IRQ_RAWSTAT.
 * Stop: cores off, domain 4 back to 260 MHz, vdd_g3d back to what it was, the session closed. */
#include "sb.h"
#include "kern.h"
#include "gpualu.h"                        /* 1.2-alpha15: the compute job (tools/mesa-alujob.c) */

#define BUCK6 0x2bU
#define BUCK6_950 0x68U
#define O_SAMPLES 0x0c000U
#define O_FBD 0x10000U
#define O_JOBS 0x20000U                     /* up to 14336 fragment jobs of 64 bytes */
#define O_PIX 0x100000U                     /* the render target, 4 MiB */
#define JOBS_MAX ((O_PIX - O_JOBS) / 64U)
#define SIDE 1024U
#define VA(o) (SB_GPU_VA + (uint64_t)(o))

int gpu_heat_allowed;                       /* Settings, after its confirmation (until a restart) */
int gpu_heat_session;
uint32_t gpuheat_khz = 572000U;            /* the GPU's table step for this session (Manual picks it) */                       /* 0.6: this Extreme session (its own confirmation names the GPU) */
static struct sb_auth au;
static int on, failed_session;
static uint8_t buck_was;
static uint32_t shaders, colour;
/* 1.2-alpha15: the arithmetic-bound compute job instead of the clear jobs (H2c: the clears are memory bound, the
 * same jobs a second from 598 to 858 MHz). Its own job slot (1: compute, as Panfrost), the shader and descriptors
 * from Mesa, its result (output slot 0) checked against the same FMA loop on the CPU after each chain */
int gpuheat_alu;
static int js;                              /* the job slot this session uses */
static int alu_loaded, alu_mismatch_said;
static uint32_t alu_expect[4];
#define ALU_JOBS_MAX ((O_PIX - O_JOBS) / GPUALU_JOB_BYTES)

static void alu_expected(void)
{
    float v[4] = { 1.0f, 1.25f, 1.5f, 1.75f };  /* output slot 0: the shader's start for slot 0 */
    for (unsigned i = 0; i < GPUALU_ITER * 16U; ++i)
        for (unsigned k = 0; k < 4; ++k)
#ifndef SB_HOST
            v[k] = __builtin_fmaf(v[k], GPUALU_FMA_A, GPUALU_FMA_B);
#else
            v[k] = v[k] * GPUALU_FMA_A + GPUALU_FMA_B;
#endif
    for (unsigned k = 0; k < 4; ++k) {
        union { float f; uint32_t u; } c = { .f = v[k] };
        alu_expect[k] = c.u;
    }
}
static unsigned chain = 64;
static uint64_t t_sub, chains, faults;
static unsigned mhz;
static int buck_written;                    /* BUCK6 was raised: it must go back on every path */
static unsigned ticks_waiting;              /* heater ticks since the chain started */

/* vdd_g3d back to what it was: independent of the GPU session (a failure after the write) */
static void buck_restore(void)
{
    if (!buck_written)
        return;
    uint8_t now_v = 0;
    int ok = 1;
    if (!sb_pmic_read(1, BUCK6, &now_v)) {
        ok = 0;                                 /* not readable: say so, never claim it is back */
    } else if (now_v != buck_was) {
        ok = sb_gate_open(&au) && sb_pmic_write(1, BUCK6, buck_was, SB_TIER1);
        sb_gate_close();
    }
    buck_written = 0;
    sb_log(ok ? "heater: gpu: vdd_g3d back" : "heater: gpu: vdd_g3d NOT back (stays 950 mV until a restart)");
}

/* 1.2-alpha12: vdd_g3d held at 950 mV. H1 on the phone: ACPM's fvmap has G3D 598 MHz at 731.25 mV, and every GPU
 * session on the phone logged "vdd_g3d changed after the clock request (ACPM)": ACPM's level request puts its own
 * voltage back, so the GPU heated at ~731 mV, not 950. Now written again after the request and checked every
 * second while the GPU heats (950 mV: the value Starbite's iOS and Linux run the GPU at, read back) */
static uint64_t t_hold;
static unsigned holds;
static uint64_t jobs_done;
/* 1.2-alpha16: the GPU above ACPM's 598 MHz in Survival (H2b/H2c on the phone: every step to 858 locked and held, the
 * cycle counter followed). PLL_G3D (a 1018X: VCO 600..1200 MHz, P 1..63, M 64..1023, S 0..6, Fref 2..8 MHz; vendor
 * cal-if pll_spec.c): 598 is P 4 M 184 S 1 (VCO 1196); above 600 the clock is the VCO itself (S 0). The vendor's
 * sequence (ra_enable_pll / ra_pll_set_pmsk): the output to the oscillator (MUX_BUSY awaited), the PLL off, the lock
 * time P x 150, one write of P, M, S with MUX_SEL and ENABLE, STABLE awaited. A failed lock goes back to the 598
 * word and stops trying for the session. */
#define G3D_CON0 0x17400140UL
#define G3D_LOCK 0x17400000UL
static uint32_t w598;                       /* PLL_CON0 after ACPM's 598 MHz request */
static unsigned cur_mhz;
static int oc_failed;

static uint32_t crd(uintptr_t a)
{
#ifndef SB_HOST
    return *(volatile uint32_t *)a;
#else
    (void)a;
    return 0xa0b80411U;                     /* host: the phone's 598 word */
#endif
}

static void cwr(uintptr_t a, uint32_t v)
{
#ifndef SB_HOST
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)a;
    (void)v;
#endif
}

int gpu_pms_ok(uint32_t p, uint32_t m, uint32_t s)
{
    uint64_t fref_khz = p ? 26000U / p : 0, vco_mhz = p ? 26ULL * m / p : 0;
    return p >= 1 && p <= 63 && m >= 64 && m <= 1023 && s <= 6 && fref_khz >= 2000 && fref_khz <= 8000 &&
           vco_mhz >= 600 && vco_mhz <= 1200;
}

int gpu_pll_set(uint32_t p, uint32_t m, uint32_t s)
{
    if (!gpu_pms_ok(p, m, s))
        return 0;
    uint32_t v = crd(G3D_CON0);
    cwr(G3D_CON0, v & ~0x10U);
#ifndef SB_HOST
    for (unsigned us = 0; us < 100 && (crd(G3D_CON0) & 0x80U); ++us)
        sb_udelay(1);
#endif
    v = crd(G3D_CON0) & ~(0x80000000U | 0x10U);
    cwr(G3D_CON0, v);
    cwr(G3D_LOCK, p * 150U);
    v = (v & ~((0x3ffU << 16) | (0x3fU << 8) | 7U)) | m << 16 | p << 8 | s;
    cwr(G3D_CON0, v | 0x80000000U | 0x10U);
#ifndef SB_HOST
    for (unsigned us = 0; us < 2000; us += 5) {
        if (crd(G3D_CON0) & 0x20000000U)
            return 1;
        sb_udelay(5);
    }
    return 0;
#else
    return 1;
#endif
}

static void back_to_598(void)
{
    if (cur_mhz > 598U && w598)
        (void)gpu_pll_set((w598 >> 8) & 0x3fU, (w598 >> 16) & 0x3ffU, w598 & 7U);
    cur_mhz = 598U;
}

/* 0 at that clock, -1 refused (not running, failed before, outside the PLL) */
int gpuheat_set_mhz(unsigned mhz)
{
    if (!on || !w598 || mhz == cur_mhz)
        return mhz == cur_mhz ? 0 : -1;
    if (mhz <= 598U) {
        back_to_598();
        return 0;
    }
    if (oc_failed)
        return -1;
    uint32_t p = (w598 >> 8) & 0x3fU, m = p ? (mhz * p + 13U) / 26U : 0;
    if (!gpu_pms_ok(p, m, 0) || !gpu_pll_set(p, m, 0)) {
        oc_failed = 1;
        back_to_598();
        sb_logx("heater: gpu: the PLL did not lock above 598: back at 598 for this session, MHz asked ", mhz);
        return -1;
    }
    if (cur_mhz <= 598U)
        sb_logx("heater: gpu: PLL_G3D above ACPM's table (S 0, the vendor's sequence), MHz ", 26U * m / p);
    cur_mhz = 26U * m / p;
    return 0;
}                  /* 1.2-alpha14: fragment jobs finished this session (H2c's throughput) */

uint64_t gpuheat_jobs(void)
{
    return jobs_done;
}

static void buck_hold(void)
{
    if (!buck_written)
        return;
    uint8_t v = 0;
    if (!sb_pmic_read(1, BUCK6, &v) || v == BUCK6_950)
        return;
    int w = sb_gate_open(&au) && sb_pmic_write(1, BUCK6, BUCK6_950, SB_TIER1);
    sb_gate_close();
    if (!holds++ || !w)
        sb_logx(w ? "heater: gpu: vdd_g3d back to 950 mV after ACPM set it, was (6.25 mV steps from 300) " :
                    "heater: gpu: vdd_g3d could NOT be set back to 950 mV, reads ", v);
}

/* job slot 0 stopped and the GPU reset before its power goes (never powered off with a chain running) */
static void quiesce(void)
{
    (void)sb_gpu_wr(0x1800 + 0x80U * (unsigned)js + 0x20, 3);   /* the session's slot: HARD_STOP */
    for (unsigned us = 0; us < 5000; us += 50) {
        uint32_t j = sb_gpu_rd(0x1000);
        if (j & (0x10001U << js))
            break;
        sb_udelay(50);
    }
    (void)sb_gpu_wr(0x1004, 0xffffffffU);       /* JOB_IRQ_CLEAR */
    (void)sb_gpu_wr(0x2004, 0xffffffffU);       /* MMU_IRQ_CLEAR */
    unsigned r = 0;
    (void)sb_gpu_reset(&r);                     /* soft reset, polled (10 ms at most) */
}

static void put(uint64_t va, const uint32_t *w, unsigned n)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)SB_GPU_CPU(va);
    for (unsigned i = 0; i < n; ++i)
        d[i] = w[i];
}

int gpuheat_on(void)
{
    return on;
}

unsigned gpuheat_mhz(void)
{
    return on ? (cur_mhz > 598U ? cur_mhz : mhz) : 0;     /* 1.2-alpha16: the PLL's own clock above ACPM's table */
}

static void put(uint64_t va, const uint32_t *w, unsigned n);

static void build_alu(void)
{
    if (!alu_loaded) {
        put(VA(GPUALU_O_SHADER), gpualu_shader, sizeof(gpualu_shader) / 4U);
        put(VA(GPUALU_O_RSD), gpualu_rsd, sizeof(gpualu_rsd) / 4U);
        put(VA(GPUALU_O_TLS), gpualu_tls, sizeof(gpualu_tls) / 4U);
        alu_loaded = 1;
    }
    static const uint32_t sentinel[4] = { 0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU };
    put(VA(GPUALU_O_OUT), sentinel, 4);
    uint32_t w[GPUALU_JOB_BYTES / 4U];
    for (unsigned k = 0; k < chain; ++k) {
        for (unsigned i = 0; i < GPUALU_JOB_BYTES / 4U; ++i)
            w[i] = gpualu_job[i];
        uint64_t next = k + 1U < chain ? VA(O_JOBS + GPUALU_JOB_BYTES * (k + 1U)) : 0;
        w[4] = (w[4] & 0xffffU) | (k + 1U) << 16;   /* the job index (Mesa's header: type 4 compute) */
        w[6] = (uint32_t)next;
        w[7] = (uint32_t)(next >> 32);
        put(VA(O_JOBS + GPUALU_JOB_BYTES * k), w, GPUALU_JOB_BYTES / 4U);
    }
}

static void build(void)
{
    if (gpuheat_alu) {
        build_alu();
        return;
    }
    uint32_t d[48], j[16];
    colour = 0xff000000U | (uint32_t)((chains + 1U) * 2654435761U >> 8);
    mali_fbd(d, VA(O_SAMPLES), SIDE, SIDE, VA(O_PIX), SIDE * 4U, colour);
    put(VA(O_FBD), d, 48);
    for (unsigned k = 0; k < chain; ++k) {
        mali_fragment(j, k + 1U, k + 1U < chain ? VA(O_JOBS + 64U * (k + 1U)) : 0, SIDE, SIDE, VA(O_FBD));
        put(VA(O_JOBS + 64U * k), j, 16);
    }
}

static int submit(void)
{
    (void)sb_gpu_wr(0x1004, 0xffffffffU);  /* JOB_IRQ_CLEAR */
    t_sub = sb_ms();
    return sb_gpu_submit((unsigned)js, VA(O_JOBS), shaders);
}

static void fail(const char *why)
{
    sb_logf("heater: gpu: ", why);
    failed_session = 1;
    if (!on && !sb_gpu_is_open())
        buck_restore();                         /* a failure during the bring-up */
}

int gpuheat_start(void)
{
    struct ub u;
    ticks_waiting = 0;
    if (on || !(gpu_heat_allowed || gpu_heat_session) || failed_session)
        return on;
    if (!sb_gpu_domain()) {
        fail("the G3D power domain is off: not switched on from here");
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
    if (t.sub != 1 || (!(t.trip_armed & 2) && !heater_trip_off())) {   /* 1.1-beta2: off by Survival's switch */
        fail("the GPU's hardware heat trip (TMU_SUB) is not armed");
        return 0;
    }
    if (!sb_pmic_read(1, BUCK6, &buck_was) || buck_was < 0x20 || buck_was > 0x70) {
        fail("vdd_g3d (BUCK6) unreadable or outside its known range");
        return 0;
    }
    sb_auth_init(&au, SB_SP_PMIC, 0, 1U << 8 | BUCK6, 1, SB_TIER1, 0, "heater: vdd_g3d 950 mV");
    sb_auth_step(&au, SB_STEP_WARN);            /* the Settings confirmation: the warning ... */
    sb_auth_step(&au, SB_STEP_HELD);            /* ... and its hold */
    uint8_t now_v = 0;
    holds = 0;
    if (buck_was != BUCK6_950) {
        int w = sb_gate_open(&au) && sb_pmic_write(1, BUCK6, BUCK6_950, SB_TIER1);
        sb_gate_close();
        buck_written = 1;
        sb_udelay(1000);
        if (!w || !sb_pmic_read(1, BUCK6, &now_v) || now_v != BUCK6_950) {
            fail("vdd_g3d did not read back 950 mV");
            return 0;
        }
    }
    if (!sb_gpu_open(&au)) {
        fail("the GPU session was refused");
        return 0;
    }
    sb_gpu_tables();
    uint32_t smp[33];
    mali_samples(smp);
    put(VA(O_SAMPLES), smp, 33);
    shaders = sb_gpu_rd(0x100);
    if (!shaders)
        shaders = 0x5771777U;
    unsigned us[4];
    if (!sb_gpu_up(shaders, us)) {
        sb_gpu_power_off();
        sb_gpu_close();
        fail("the GPU did not power up");
        return 0;
    }
    uint8_t chk = 0;
    if (hw_dvfs(DVFS_GPU, sb_dvfs_level_listed(4, gpuheat_khz) ? gpuheat_khz : 572000U)) {
        sb_mdelay(2);
        mhz = (unsigned)(sb_gpu_hz(&off) / 1000000U);
    } else {
        mhz = (unsigned)(hz / 1000000U);
    }
    if (buck_written && (!sb_pmic_read(1, BUCK6, &chk) || chk != BUCK6_950))
        sb_log("heater: gpu: vdd_g3d changed after the clock request (ACPM): set again");
    buck_hold();                            /* 1.2-alpha12 */
    t_hold = sb_ms();
    w598 = 0;
    cur_mhz = mhz;
    oc_failed = 0;
    if (mhz == 598U) {
        uint32_t w = crd(G3D_CON0);
        if ((w & 0xa0000010U) == 0xa0000010U && gpu_pms_ok((w >> 8) & 0x3fU, (w >> 16) & 0x3ffU, w & 7U))
            w598 = w;                       /* enabled, locked, on the PLL: the word to come back to */
    }
    chain = gpuheat_alu ? 4 : 64;
    chains = faults = 0;
    jobs_done = 0;
    js = gpuheat_alu ? 1 : 0;
    alu_loaded = alu_mismatch_said = 0;
    if (gpuheat_alu)
        alu_expected();
    build();
    if (!submit()) {
        gpuheat_fail_stop("the first job did not start");
        return 0;
    }
    on = 1;
    ub_init(&u);
    ub_s(&u, "heater: gpu on, ");
    ub_dec(&u, mhz);
    ub_s(&u, " MHz, cores ");
    ub_hex(&u, shaders, 8);
    sb_log(u.b);
    return 1;
}

/* the 4 Hz tick: a finished chain is checked and the next one started; a fault stops the GPU's share */
void gpuheat_tick(void)
{
    if (!on)
        return;
    uint32_t j = sb_gpu_rd(0x1000), m = sb_gpu_rd(0x2000), g = sb_gpu_rd(0x0020);
    if ((j & (0x10000U << js)) || m || (g & 0x81U) || sb_ms() - t_sub > 5000U) {
        struct sb_gpu_result r;
        (void)sb_gpu_wait(1, 0, &r);            /* stops the slot, clears the interrupts */
        ++faults;
        gpuheat_fail_stop(sb_ms() - t_sub > 5000U ? "a chain did not finish in 5 s: the GPU's share stops" :
                                                    "a job faulted: the GPU's share stops");
        return;
    }
    ++ticks_waiting;
    if (sb_ms() - t_hold >= 1000U) {           /* 1.2-alpha12: ACPM may set its own voltage again */
        t_hold = sb_ms();
        buck_hold();
    }
    if (!(j & (1U << js)))
        return;                                 /* still running */
    uint64_t took = sb_ms() - t_sub;
    if (ticks_waiting <= 1)
        took = 0;                               /* done within one tick: the chain is too short */
    ticks_waiting = 0;
    volatile uint32_t *px = (volatile uint32_t *)(uintptr_t)SB_GPU_CPU(VA(O_PIX));
    if (gpuheat_alu) {
        if (px[0] == 0xffffffffU && px[1] == 0xffffffffU) {
            gpuheat_fail_stop("the compute job wrote nothing: the GPU's share stops");
            return;
        }
        if (!alu_mismatch_said && (px[0] != alu_expect[0] || px[3] != alu_expect[3])) {
            alu_mismatch_said = 1;
            struct ub u;
            ub_init(&u);
            ub_s(&u, "heater: gpu: the compute result differs from the CPU's FMA loop (logged once): GPU ");
            ub_hex(&u, px[0], 8);
            ub_s(&u, " CPU ");
            ub_hex(&u, alu_expect[0], 8);
            sb_log(u.b);
        }
    } else
    for (unsigned k = 0; k < 8; ++k)
        if (px[(k * 131071U) % (SIDE * SIDE)] != colour) {
            gpuheat_fail_stop("the render target holds the wrong colour: the GPU's share stops");
            return;
        }
    ++chains;
    jobs_done += chain;
    /* 1.2-alpha14: chains grow until one lasts 1 s (was 200 ms): the heater resubmits on its 4 Hz tick, so a 200 ms
     * chain left the GPU idle up to half the time (H2b on the phone: 1024-job chains, the GPU zone at ~50 C) */
    if (took < 1000U && chain * 2U <= (gpuheat_alu ? ALU_JOBS_MAX : JOBS_MAX))
        chain *= 2U;
    build();
    if (!submit())
        gpuheat_fail_stop("a chain did not start");
}

void gpuheat_stop(void)
{
    if (!sb_gpu_is_open()) {
        buck_restore();
        return;
    }
    quiesce();
    back_to_598();                          /* 1.2-alpha16: ACPM's own word before its 260 MHz request */
    w598 = 0;
    sb_gpu_power_off();
    (void)hw_dvfs(DVFS_GPU, 260000);
    sb_gpu_close();
    sb_log("heater: gpu off");
    buck_restore();
    if (on) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "heater: gpu ran ");
        ub_dec(&u, chains);
        ub_s(&u, " chains, last ");
        ub_dec(&u, chain);
        ub_s(&u, " jobs each");
        sb_log(u.b);
    }
    on = 0;
}

void gpuheat_fail_stop(const char *why)
{
    gpuheat_stop();
    fail(why);
    if (gpuheat_alu) {
        /* 1.2-alpha16: the compute job failed: the clear jobs once more in this session (the GPU was reset) */
        gpuheat_alu = 0;
        failed_session = 0;
        sb_log("heater: gpu: the compute job failed: the clear jobs instead for this session");
    }
}

void gpuheat_new_session(void)
{
    failed_session = 0;
}
