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
static struct sb_auth au;
static int on, failed_session;
static uint8_t buck_was;
static uint32_t shaders, colour;
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

/* job slot 0 stopped and the GPU reset before its power goes (never powered off with a chain running) */
static void quiesce(void)
{
    (void)sb_gpu_wr(0x1800 + 0x20, 3);          /* JS0 COMMAND: HARD_STOP */
    for (unsigned us = 0; us < 5000; us += 50) {
        uint32_t j = sb_gpu_rd(0x1000);
        if (j & 0x10001U)
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
    return on ? mhz : 0;
}

static void build(void)
{
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
    return sb_gpu_submit(0, VA(O_JOBS), shaders);
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
    if (on || !gpu_heat_allowed || failed_session)
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
    if (t.sub != 1 || !(t.trip_armed & 2)) {
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
    if (hw_dvfs(DVFS_GPU, 572000)) {
        sb_mdelay(2);
        mhz = (unsigned)(sb_gpu_hz(&off) / 1000000U);
    } else {
        mhz = (unsigned)(hz / 1000000U);
    }
    if (buck_written && (!sb_pmic_read(1, BUCK6, &chk) || chk != BUCK6_950))
        sb_log("heater: gpu: vdd_g3d changed after the clock request (ACPM): logged");
    chain = 64;
    chains = faults = 0;
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
    if ((j & 0x10000U) || m || (g & 0x81U) || sb_ms() - t_sub > 5000U) {
        struct sb_gpu_result r;
        (void)sb_gpu_wait(1, 0, &r);            /* stops the slot, clears the interrupts */
        ++faults;
        gpuheat_fail_stop(sb_ms() - t_sub > 5000U ? "a chain did not finish in 5 s: the GPU's share stops" :
                                                    "a job faulted: the GPU's share stops");
        return;
    }
    ++ticks_waiting;
    if (!(j & 1U))
        return;                                 /* still running */
    uint64_t took = sb_ms() - t_sub;
    if (ticks_waiting <= 1)
        took = 0;                               /* done within one tick: the chain is too short */
    ticks_waiting = 0;
    volatile uint32_t *px = (volatile uint32_t *)(uintptr_t)SB_GPU_CPU(VA(O_PIX));
    for (unsigned k = 0; k < 8; ++k)
        if (px[(k * 131071U) % (SIDE * SIDE)] != colour) {
            gpuheat_fail_stop("the render target holds the wrong colour: the GPU's share stops");
            return;
        }
    ++chains;
    if (took < 200U && chain * 2U <= JOBS_MAX)
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
}

void gpuheat_new_session(void)
{
    failed_session = 0;
}
