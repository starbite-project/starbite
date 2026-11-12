/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/shub_more.c and kern/senstat.c (-DSB_HOST) against a fake sensor hub and fake registers.
 *   the hub     shub.c's API (state, starts, shub_enable_type with its one-request slot, the hooks) and a chip that
 *               turns sensors on / off and sends synthetic SSP data frames, parsed with the real shub_proto.c
 *               (shp_frame_next) exactly as shub.c's frame() does, then handed to the sample hook;
 *   survival    surv_sensors_on / _state (Last 2 % refusal);
 *   registers   a sparse MMIO map for the pins, the I2C_CMGP03 controller and its gates; a fake P9320 at 0x3B.
 * Checks: the frame lengths of every sensor starlte sends; each sample's decoding (proximity, lux, both pressure
 * units, gyroscope scale); only asked-for sensors run and they stop 10 s after the last ask; the in-call pin; a hub
 * restart re-enables; silence re-asks three times; the probe at level 0 (no write at all), level 1 (P9320 read with
 * every register back as found, the hub started, four sensors on, statistics, all off, the hub left), the firmware
 * missing from the card, an abort mid-way; the auto-brightness curve.
 * Build and run: starbite/sbos/kern/host/shub-more-test.sh (the Mac's cc). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "shub.h"
#include "shub_proto.h"
#include "survival.h"
#include "shub_more.h"
#include "drvprobe.h"

static int fails, checks, verbose;
extern const char *senstat_why;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- time and the log ---- */
static uint64_t now_ms = 50000;
uint64_t sb_ms(void) { return now_ms; }
uint64_t sb_us(void) { return now_ms * 1000; }
static char logbuf[1 << 19];
static unsigned logn;
void sb_log(const char *s)
{
    size_t n = strlen(s);
    if (logn + n + 2 < sizeof logbuf) {
        memcpy(logbuf + logn, s, n);
        logn += (unsigned)n;
        logbuf[logn++] = '\n';
        logbuf[logn] = 0;
    }
    if (verbose)
        printf("    | %s\n", s);
}
void sb_logf(const char *a, const char *b)
{
    char t[600];
    snprintf(t, sizeof t, "%s%s", a, b ? b : "(null)");
    sb_log(t);
}
void sb_logx(const char *a, uint64_t v)
{
    char t[600];
    snprintf(t, sizeof t, "%s0x%llx", a, (unsigned long long)v);
    sb_log(t);
}
static void log_clear(void) { logn = 0; logbuf[0] = 0; }

/* ---- registers ---- */
#define NMM 256
static struct { uintptr_t a; uint32_t v; } mm[NMM];
static int nmm;
static unsigned writes, bad_writes;
static uintptr_t fault_at;
static uint32_t *mmref(uintptr_t a)
{
    for (int i = 0; i < nmm; ++i)
        if (mm[i].a == a)
            return &mm[i].v;
    if (nmm == NMM) {
        printf("mmio map full\n");
        exit(2);
    }
    mm[nmm].a = a;
    mm[nmm].v = 0;
    return &mm[nmm++].v;
}
static void mmset(uintptr_t a, uint32_t v) { *mmref(a) = v; }
static uint32_t mmget(uintptr_t a) { return *mmref(a); }
uint32_t senstat_sim_rd(uintptr_t a, int *ok)
{
    *ok = a != fault_at;
    return mmget(a);
}
void senstat_sim_wr(uintptr_t a, uint32_t v)
{
    ++writes;
    int allowed = a == 0x142201C0UL || a == 0x142201C8UL || a == 0x142201E0UL || a == 0x142201E8UL ||
                  (a >= 0x14370000UL && a < 0x14370100UL);
    if (!allowed)
        ++bad_writes;
    mmset(a, v);
}
static void regs_phone(void)
{
    nmm = 0;
    writes = bad_writes = 0;
    fault_at = 0;
    mmset(0x14064084UL, 0xF);                       /* CHUB on */
    mmset(0x13A80000UL, 0x00112222);
    mmset(0x14050084UL, 1U << 6);                   /* gpa3: hall pin high = no magnet */
    mmset(0x14050080UL, 0xF0000000U);
    mmset(0x14050024UL, 1U << 2 | 1U << 5);         /* gpa0: certify hall high, AP_SLEEP */
    mmset(0x14050064UL, 0);                         /* gpa2-0: no pad */
    mmset(0x14202044UL, 0x00200000U);               /* gates open */
    mmset(0x14202048UL, 0x00200000U);
    mmset(0x14202060UL, 0x00200000U);
    mmset(0x142201C0UL, 0x0);                       /* gpm16 / 17 as S-Boot leaves them: inputs */
    mmset(0x142201C8UL, 0x1);
    mmset(0x142201E0UL, 0x0);
    mmset(0x142201E8UL, 0x1);
    mmset(0x14370000UL, 0x0);                       /* the controller untouched */
    mmset(0x14370068UL, 0x0);
}

/* ---- the I2C buses ---- */
static int pad, wpc_seen_pins_ok, i2c_bus_calls, hrm_reads;
void sb_i2c_bus(struct sb_i2c *b)
{
    ++i2c_bus_calls;
    if (b->base == 0x14370000UL) {
        senstat_sim_wr(b->base + 0x00, 0x8);
        senstat_sim_wr(b->base + 0x60, b->fs1);
        senstat_sim_wr(b->base + 0x64, b->fs2);
        senstat_sim_wr(b->base + 0x68, b->fs3);
        senstat_sim_wr(b->base + 0x40, 0x80000000U);
    }
}
int sb_i2c_read(struct sb_i2c *b, unsigned addr, uint8_t reg, uint8_t *d, unsigned len)
{
    if (b->base == 0x108E0000UL) {
        ++hrm_reads;
        return 0;
    }
    if (b->base != 0x14370000UL || addr != 0x3B)
        return 0;
    wpc_seen_pins_ok = (mmget(0x142201C0UL) & 0xf) == 3 && (mmget(0x142201E0UL) & 0xf) == 3 && (mmget(0x14370000UL) & 8);
    if (!pad || !wpc_seen_pins_ok)
        return 0;
    static const uint8_t regs[0x50] = { [0x00] = 0x20, [0x01] = 0x93, [0x02] = 0x11, [0x20] = 0x05, [0x3C] = 0x2C,
                                         [0x3D] = 0x06, [0x40] = 0x50, [0x41] = 0x06, [0x46] = 0x28, [0x48] = 0x8F };
    for (unsigned i = 0; i < len; ++i)
        d[i] = reg + i < sizeof regs ? regs[reg + i] : 0;
    return 1;
}
static uint8_t ldo38 = 0x2C;
int sb_pmic_read(uint32_t type, uint32_t reg, uint8_t *v)
{
    *v = type == 1 && reg == 0x6F ? ldo38 : type == 1 && reg == 0x6E ? 0xEC : 0;
    return 1;
}

/* ---- the hub ---- */
static struct {
    int state, slot_busy, fw_missing, step_n, left, begun;
    unsigned starts;
    shub_sample_fn sample;
    void (*poll)(void);
    int en[64];
    unsigned period[64];
    unsigned adds, removes, req_log[64][3], nreq;
    int silent[64];                        /* a sensor that never sends */
    int near;
    uint32_t lux;
    int32_t press_raw, gyro_raw;
    uint64_t t_frame;
} H;
int shub_state(void) { return H.state; }
unsigned shub_starts(void) { return H.starts; }
const char *shub_why(void)
{
    return H.state == SHUB_FAILED ? (H.fw_missing ? "the sensor hub's firmware is not on the card (/STARBITE/FW/BCM4775.BIN)"
                                                  : "the hub did not answer")
                                  : "ok";
}
void shub_set_hooks(shub_sample_fn s, void (*p)(void))
{
    H.sample = s;
    H.poll = p;
}
int shub_enable_type(unsigned type, unsigned period_ms, int on)
{
    if (H.state != SHUB_RUNNING || H.slot_busy || type > 63)
        return -1;
    H.slot_busy = 1;
    H.en[type] = on;
    H.period[type] = period_ms;
    on ? ++H.adds : ++H.removes;
    if (H.nreq < 64) {
        H.req_log[H.nreq][0] = type;
        H.req_log[H.nreq][1] = period_ms;
        H.req_log[H.nreq][2] = (unsigned)on;
        ++H.nreq;
    }
    return 0;
}
void shub_begin(int probe_only)
{
    (void)probe_only;
    H.begun = 1;
    H.state = SHUB_STARTING;
    H.step_n = 0;
    H.left = 0;
}
int shub_step(void)
{
    if (H.state != SHUB_STARTING)
        return 1;
    if (++H.step_n < 30)
        return 0;
    if (H.fw_missing) {
        H.state = SHUB_FAILED;
        return 1;
    }
    H.state = SHUB_RUNNING;
    ++H.starts;
    memset(H.en, 0, sizeof H.en);
    H.en[0] = H.en[4] = H.en[18] = 1;
    return 1;
}
unsigned shub_wait_ms(void) { return 20; }
void shub_leave(void)
{
    H.left = 1;
    H.state = SHUB_OFF;
    memset(H.en, 0, sizeof H.en);
}

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* shub.c's frame(): every record through shp_frame_next; sensors other than 0 / 4 / 18 to the hook */
static unsigned frames_parsed, frame_errors;
static void deliver(const uint8_t *f, unsigned n)
{
    struct shp_rec r;
    unsigned off = 0;
    int v;
    while ((v = shp_frame_next(f, n, &off, &r)) == 1)
        if (r.kind == SSP_REC_SENSOR && r.sensor != 0 && r.sensor != 4 && r.sensor != 18 && H.sample)
            H.sample(r.sensor, r.raw, r.raw_len, r.ts);
    if (v < 0)
        ++frame_errors;
    ++frames_parsed;
}
static unsigned rec(uint8_t *f, unsigned type, const uint8_t *d, unsigned n)
{
    f[0] = 0x37;
    f[1] = (uint8_t)type;
    memcpy(f + 2, d, n);
    memset(f + 2 + n, 0x11, 8);
    return n + 10;
}
static void chip_frame(void)
{
    uint8_t f[512], d[32];
    unsigned n = 0;
    memset(d, 0, sizeof d);
    put16(d, 31);
    put16(d + 2, (uint16_t)-57);
    put16(d + 4, 4076);
    n += rec(f + n, 0, d, 6);
    if (H.en[7] && !H.silent[7]) {
        d[0] = (uint8_t)H.near;
        put16(d + 1, H.near ? 900 : 40);
        n += rec(f + n, 7, d, 3);
    }
    if (H.en[9] && !H.silent[9]) {
        memset(d, 0, 18);
        put32(d, H.lux);
        put32(d + 4, 5000);
        put16(d + 8, 100);
        n += rec(f + n, 9, d, 18);
    }
    if (H.en[5] && !H.silent[5]) {
        put32(d, (uint32_t)H.press_raw);
        put16(d + 4, 2350);
        n += rec(f + n, 5, d, 6);
    }
    if (H.en[1] && !H.silent[1]) {
        put32(d, (uint32_t)H.gyro_raw);
        put32(d + 4, (uint32_t)-H.gyro_raw);
        put32(d + 8, 0);
        n += rec(f + n, 1, d, 12);
    }
    deliver(f, n);
}
void shub_poll(void)
{
    if (H.state != SHUB_RUNNING)
        return;
    H.slot_busy = 0;                        /* the echo came, the data phase went out */
    if (now_ms - H.t_frame >= 100) {
        H.t_frame = now_ms;
        chip_frame();
    }
    if (H.poll)
        H.poll();
}

/* ---- survival ---- */
static int surv_on, last2, surv_calls;
int surv_sensors_on(int on)
{
    ++surv_calls;
    if (!on) {
        surv_on = 0;
        shub_leave();
        return 0;
    }
    if (last2)
        return -1;
    if (surv_on && H.state != SHUB_FAILED)
        return 0;
    surv_on = 1;
    shub_begin(0);
    return 0;
}
int surv_sensors_state(void) { return surv_on ? H.state : SHUB_OFF; }

/* survival's own tick: steps a start, then polls every 50 ms */
static void tick(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 10) {
        now_ms += 10;
        if (surv_on && H.state == SHUB_STARTING)
            (void)shub_step();
        if (H.state == SHUB_RUNNING && now_ms % 50 == 0)
            shub_poll();
    }
}

static void hub_reset(void)
{
    shub_sample_fn s = H.sample;
    void (*p)(void) = H.poll;
    memset(&H, 0, sizeof H);
    H.sample = s;
    H.poll = p;
    H.lux = 250;
    H.press_raw = 1013 * 4096 + 1024;       /* 1013.25 hPa in the LPS22H's unit */
    H.gyro_raw = 1000;
    surv_on = 0;
    last2 = 0;
}

static int run_probe(int level, unsigned max_ms, int drive_survival)
{
    unsigned w = 0;
    for (unsigned t = 0; t < max_ms;) {
        if (drv_sens_probe(level, &w))
            return 1;
        if (!w)
            w = 1;
        if (drive_survival)
            tick(w < 10 ? 10 : w);
        else
            now_ms += w;
        t += w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    verbose = argc > 1;
    (void)argv;
    printf("shub more host test\n");

    /* ---- the frame lengths (shub_proto.c) ---- */
    {
        uint8_t f[600];
        unsigned n = 0;
        static const struct { uint8_t t, len; } L[] = { { 0, 6 }, { 1, 12 }, { 2, 13 }, { 3, 6 }, { 4, 8 }, { 5, 6 },
                                                       { 7, 3 }, { 9, 18 }, { 10, 2 }, { 13, 1 }, { 14, 1 }, { 15, 24 },
                                                       { 18, 4 }, { 24, 12 }, { 25, 12 }, { 29, 3 }, { 30, 2 }, { 31, 18 },
                                                       { 35, 3 } };
        uint8_t d[32];
        for (unsigned i = 0; i < 32; ++i)
            d[i] = (uint8_t)(i + 1);
        for (unsigned i = 0; i < sizeof L / sizeof L[0]; ++i)
            n += rec(f + n, L[i].t, d, L[i].len);
        f[n++] = 0x05;
        f[n++] = 1;
        f[n++] = 7;
        struct shp_rec r;
        unsigned off = 0, k = 0;
        int v, lens_ok = 1;
        while ((v = shp_frame_next(f, n, &off, &r)) == 1) {
            if (r.kind == SSP_REC_SENSOR) {
                if (r.sensor != L[k].t || r.raw_len != L[k].len || r.raw[0] != 1 || r.ts != 0x1111111111111111ULL)
                    lens_ok = 0;
                ++k;
            }
        }
        CHECK(v == 0 && off == n, "a frame with every sensor starlte sends parses to its end (%d, %u of %u)", v, off, n);
        CHECK(k == sizeof L / sizeof L[0] && lens_ok, "each record's sensor, raw bytes and timestamp (%u)", k);
    }

    /* ---- the API against a hub run by survival ---- */
    regs_phone();
    hub_reset();
    CHECK(sens_prox_near() == -1, "proximity unknown while the hub is off");
    CHECK(surv_on && H.state == SHUB_STARTING, "asking started the hub through survival");
    tick(400);
    CHECK(H.state == SHUB_RUNNING, "the hub runs");
    tick(200);
    CHECK(H.en[7] && H.period[7] == 200 && !H.en[9] && !H.en[5] && !H.en[1], "only the proximity was switched on, at 200 ms");
    H.near = 1;
    tick(300);
    CHECK(sens_prox_near() == 1, "near");
    H.near = 0;
    tick(300);
    CHECK(sens_prox_near() == 0, "far");

    uint32_t lux = 0;
    CHECK(!sens_lux(&lux), "light: not yet");
    tick(300);
    CHECK(sens_lux(&lux) && lux == 250, "light: 250 lux (%u)", lux);
    int32_t hpa = 0, tc = 0, g[3] = { 0, 0, 0 };
    (void)sens_pressure_hpa(&hpa, &tc);
    tick(300);
    CHECK(sens_pressure_hpa(&hpa, &tc) && hpa == 101325 && tc == 2350, "pressure 1013.25 hPa in 1/4096 (%d, %d)", hpa, tc);
    H.press_raw = 101325;
    tick(300);
    CHECK(sens_pressure_hpa(&hpa, &tc) && hpa == 101325, "pressure in 1/100 hPa (%d)", hpa);
    H.press_raw = 5;
    tick(300);
    tick(2500);
    CHECK(!sens_pressure_hpa(&hpa, &tc), "an implausible pressure is not reported");
    H.press_raw = 1013 * 4096;
    (void)sens_gyro_mdps(g);
    tick(300);
    CHECK(sens_gyro_mdps(g) && g[0] == 70000 && g[1] == -70000 && g[2] == 0, "gyroscope: 1000 counts = 70 dps (%d %d)", g[0], g[1]);
    CHECK(sens_running_mask() == SENS_ALL, "all four on while asked for");

    /* nobody asks for 10 s: off at the hub, one at a time */
    unsigned rem0 = H.removes;
    tick(11000);
    CHECK(H.removes - rem0 == 4 && !H.en[7] && !H.en[9] && !H.en[5] && !H.en[1], "10 s without an ask: all four removed (%u)", H.removes - rem0);
    CHECK(sens_running_mask() == 0, "nothing of ours runs");
    CHECK(H.req_log[H.nreq - 1][2] == 0 && H.req_log[H.nreq - 1][1] > 0, "a removal carries its period (Samsung's layout)");

    /* the in-call pin */
    sens_incall(1);
    tick(300);
    H.near = 1;
    tick(300);
    CHECK(sens_incall_screen_off() == 1, "in a call and near: screen off");
    tick(20000);
    CHECK(H.en[7] && sens_incall_screen_off() == 1, "the in-call proximity stays on without asks (20 s)");
    H.near = 0;
    tick(300);
    CHECK(sens_incall_screen_off() == 0, "far: screen on");
    sens_incall(0);
    tick(300);
    CHECK(!H.en[7] && sens_incall_screen_off() == 0, "call ended: proximity off at once");

    /* keep-alive: survival's 60 s idle stop does not apply while a sensor is wanted */
    int calls0 = surv_calls;
    for (int i = 0; i < 30; ++i) {
        (void)sens_lux(&lux);
        tick(1000);
    }
    CHECK(surv_calls > calls0 + 20, "survival kept alive while the light is read (%d)", surv_calls - calls0);

    /* a hub restart: ours on again */
    unsigned adds0 = H.adds;
    shub_leave();
    shub_begin(0);
    tick(400);
    (void)sens_lux(&lux);
    tick(300);
    CHECK(H.en[9] && H.adds > adds0, "after a restart the light is switched on again");

    /* silence: asked again, three times in all */
    hub_reset();
    regs_phone();
    surv_on = 1;
    shub_begin(0);
    tick(400);
    H.silent[1] = 1;
    adds0 = H.adds;
    for (int i = 0; i < 15; ++i) {
        (void)sens_gyro_mdps(g);
        tick(1000);
    }
    CHECK(H.adds - adds0 == 3, "a silent gyroscope is asked three times (%u)", H.adds - adds0);
    CHECK(!sens_gyro_mdps(g), "and stays invalid");
    H.silent[1] = 0;

    /* a wrong sample length is counted, not used */
    if (H.sample) {
        uint8_t d[3] = { 1, 0, 0 };
        H.sample(9, d, 3, 0);
    }
    CHECK(sens_running_mask() & SENS_GYRO, "(the gyro is still on)");

    /* Last 2 %: refused in plain words */
    hub_reset();
    shub_leave();
    H.state = SHUB_OFF;
    last2 = 1;
    tick(5000);
    CHECK(sens_want(SENS_LIGHT) == -1 && strstr(sens_why(), "Last 2") != 0, "Last 2 %%: refused (%s)", sens_why());
    last2 = 0;

    /* the auto-brightness curve */
    hub_reset();
    surv_on = 1;
    shub_begin(0);
    tick(400);
    static const struct { uint32_t lux; int lv; } AB[] = { { 0, 6 }, { 1, 10 }, { 10, 40 }, { 100, 90 }, { 1000, 170 },
                                                          { 10000, 255 }, { 99999, 255 }, { 55, 65 } };
    for (unsigned i = 0; i < sizeof AB / sizeof AB[0]; ++i) {
        H.lux = AB[i].lux;
        (void)sens_auto_brightness();
        tick(400);
        int lv = sens_auto_brightness();
        CHECK(lv == AB[i].lv, "auto brightness at %u lux: %d (expected %d)", AB[i].lux, lv, AB[i].lv);
    }
    sens_release(SENS_ALL);
    tick(500);
    CHECK(sens_running_mask() == 0, "released: nothing runs");

    /* ---- status parts ---- */
    regs_phone();
    CHECK(sens_hall_closed() == 0 && sens_hall_certify() == 0, "hall pins high: no magnet");
    mmset(0x14050084UL, 0);
    CHECK(sens_hall_closed() == 1, "hall pin low: cover closed");
    fault_at = 0x14050084UL;
    CHECK(sens_hall_closed() == -1, "hall unreadable: -1");
    regs_phone();
    struct sens_wpc w;
    pad = 0;
    CHECK(sens_wpc_read(&w) == 0 && !w.answered, "no pad: the P9320 does not answer");
    CHECK(wpc_seen_pins_ok, "the read happened with the pins on function 3 and the controller up");
    CHECK(bad_writes == 0, "only the two pins and the controller were written");
    CHECK(mmget(0x142201C0UL) == 0 && mmget(0x142201C8UL) == 1 && mmget(0x142201E0UL) == 0 && mmget(0x142201E8UL) == 1 &&
              mmget(0x14370000UL) == 0 && mmget(0x14370068UL) == 0 && mmget(0x14370060UL) == 0 && mmget(0x14370040UL) == 0,
          "pins and controller back as found");
    pad = 1;
    mmset(0x14050064UL, 1);
    CHECK(sens_wpc_read(&w) == 1 && w.chip_id == 0x9320 && w.rev == 0x11 && w.vout_raw == 0x62C && w.freq_khz == 0x8F,
          "on a pad: P9320 ID, revision, Vout, frequency (%x %u)", w.chip_id, w.vout_raw);
    CHECK(w.vout_mv > 4800 && w.vout_mv < 4900, "Vout about 4.85 V (%d mV)", w.vout_mv);
    CHECK(mmget(0x142201C0UL) == 0 && mmget(0x14370000UL) == 0, "and back as found");
    pad = 0;
    regs_phone();
    mmset(0x14202048UL, 0x00100000U);       /* PCLK gate held closed */
    writes = 0;
    CHECK(sens_wpc_read(&w) == -1 && writes == 0, "gate closed: refused, nothing written");
    regs_phone();
    uint8_t id = 0;
    hrm_reads = 0;
    CHECK(sens_hrm_part_id(&id) == 0 && hrm_reads == 0, "HRM rails off: the part ID is not read");
    ldo38 = 0xEC;
    CHECK(sens_hrm_part_id(&id) == 0 && hrm_reads == 0 && strstr(senstat_why, "not set up"), "HRM bus not set up: not read");
    ldo38 = 0x2C;

    /* ---- the probe ---- */
    regs_phone();
    hub_reset();
    shub_leave();
    H.state = SHUB_OFF;
    H.begun = 0;
    log_clear();
    writes = 0;
    i2c_bus_calls = 0;
    CHECK(run_probe(DRV_LVL_READ, 2000, 0), "level 0 finishes");
    CHECK(strstr(logbuf, "drvprobe: sens: PASS cover open, no wireless pad, CHUB on, HRM rails off") != 0, "level 0 PASS line");
    CHECK(writes == 0 && i2c_bus_calls == 0 && !H.begun && H.nreq == 0, "level 0 wrote nothing, started nothing");
    CHECK(strstr(logbuf, "not set up by anyone") != 0, "level 0: the P9320's bus not set up: said so");
    log_clear();
    fault_at = 0x14050024UL;
    run_probe(DRV_LVL_READ, 2000, 0);
    CHECK(strstr(logbuf, "drvprobe: sens: FAIL a status register did not read") != 0, "level 0: a faulting read fails");
    fault_at = 0;

    log_clear();
    regs_phone();
    hub_reset();
    H.state = SHUB_OFF;
    H.near = 1;
    CHECK(run_probe(DRV_LVL_ACTIVE, DRV_SENS_BUDGET_L1, 0), "level 1 finishes within its budget");
    CHECK(strstr(logbuf, "drvprobe: sens: PASS proximity saw near, light mean 250 lux, 1013 hPa") != 0, "level 1 PASS line");
    CHECK(strstr(logbuf, "pressure unit 1/4096 hPa") && strstr(logbuf, "hPa n ") && strstr(logbuf, "gyro mdps x n "),
          "level 1: statistics per type");
    CHECK(H.left && H.state == SHUB_OFF && sens_running_mask() == 0, "level 1: the hub left, nothing of ours on");
    CHECK(H.removes == 4, "level 1: the four removed again (%u)", H.removes);
    CHECK(bad_writes == 0 && mmget(0x142201C0UL) == 0 && mmget(0x14370000UL) == 0, "level 1: the P9320 look put everything back");

    log_clear();
    hub_reset();
    H.state = SHUB_OFF;
    H.fw_missing = 1;
    CHECK(run_probe(DRV_LVL_ACTIVE, DRV_SENS_BUDGET_L1, 0), "firmware missing: finishes");
    CHECK(strstr(logbuf, "not on the card (/STARBITE/FW/BCM4775.BIN): ending") &&
              strstr(logbuf, "drvprobe: sens: FAIL the sensor hub's firmware is not on the card"), "firmware missing: said so");
    CHECK(H.left && H.nreq == 0, "firmware missing: left, nothing enabled");

    log_clear();
    hub_reset();
    H.state = SHUB_OFF;
    H.silent[7] = 1;
    run_probe(DRV_LVL_ACTIVE, DRV_SENS_BUDGET_L1, 0);
    CHECK(strstr(logbuf, "drvprobe: sens: FAIL no samples from: proximity") != 0, "a silent proximity fails in plain words");
    CHECK(H.left, "and the hub is left");

    /* the hub already running (survival's): used, not left */
    log_clear();
    hub_reset();
    surv_on = 1;
    shub_begin(0);
    tick(400);
    CHECK(run_probe(DRV_LVL_ACTIVE, DRV_SENS_BUDGET_L1, 1), "level 1 on survival's hub finishes");
    CHECK(strstr(logbuf, "the hub already runs: using it") && strstr(logbuf, "drvprobe: sens: PASS"), "level 1 on survival's hub passes");
    CHECK(H.state == SHUB_RUNNING && !H.left && sens_running_mask() == 0, "survival's hub still runs, ours off");

    /* abort mid-way */
    log_clear();
    hub_reset();
    H.state = SHUB_OFF;
    unsigned wt = 0;
    for (int i = 0; i < 60; ++i) {
        (void)drv_sens_probe(DRV_LVL_ACTIVE, &wt);
        now_ms += wt ? wt : 1;
    }
    CHECK(H.state == SHUB_RUNNING, "(the probe's hub runs)");
    drv_sens_abort();
    CHECK(H.left && H.state == SHUB_OFF && sens_running_mask() == 0, "abort: the hub left, nothing of ours on");
    CHECK(run_probe(DRV_LVL_ATTENDED, 100, 0) && strstr(logbuf, "drvprobe: sens: PASS nothing to see at level 2"), "level 2: nothing");

    CHECK(frame_errors == 0, "no frame was cut short (%u of %u)", frame_errors, frames_parsed);
    printf("shub more host test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
