/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/cam.c and cam_isp.c against a fake phone (cam-test.sh builds and runs it; -v traces
 * the driver's log).
 *
 * The fake models what the driver relies on: the GPIO banks (the torch's pin on the reset's bank), the USI mode words,
 * CMU_PERIC1 / CMU_TOP gates and muxes, the PMU's ISPPRE domain (its blocks read as faults and lose their registers
 * while it is off) and the DTZPC SMCs, the two I2C buses (a controller answers only once its mode, pins and gates are
 * right), the S2MPB02 as a register file that logs every supply switching on or off, the S5K3H1 (answers only with all
 * five supplies, the clock, MCLK on its pin and the reset high; streams only with the mode loaded), and CSIS1 + its
 * WDMA, which write a synthetic colour-bar frame to the frame pointer's address every 33.3 ms while everything on the
 * way is set up. The 32 MiB at 0xD8000000 are host memory.
 *
 * Checked: the ISP reproduces the colour bars (both raw formats), the rotations and the mirror, white balance, BMP;
 * every write lands in the camera's own registers (and only its own bits of shared GPIO banks: the torch's pin is never
 * touched); the S2MPB02 gets only the camera's six registers; supplies come on as BUCK2, VDDAF, VDDA, VDDIO, VDDD and go
 * off as VDDAF, VDDD, VDDA, VDDIO, BUCK2, always with the reset low, MCLK off and the bus pins off; the probe at levels
 * 0 (no write, no I2C write), 1 and 2; a failure injected at each step ends in FAIL with everything as found; an abort
 * at every point of the start-up leaves everything as found. Exit status 0 = all passed. */
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cam.h"

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

/* ---- addresses (the fake's own copy) ------------------------------------------------------------------------------ */
#define GPG2 0x104300C0ULL
#define GPP5 0x10830020ULL
#define GPP6 0x10830040ULL
#define GPC1 0x10830080ULL
#define USI16 0x1081102CULL
#define USI20 0x1081103CULL
#define CMUP 0x10800000ULL
#define CMUT 0x1A240000ULL
#define PMU 0x14060000ULL
#define CMU_ISPPRE 0x16200000ULL
#define SYSREG_ISPPRE 0x16210000ULL
#define CSIS 0x16240000ULL
#define DPHY 0x16180000ULL
#define WDMA 0x162C0000ULL
#define CDMA 0x162F0000ULL
#define HS_PMIC 0x10920000ULL
#define HS_SENS 0x108F0000ULL

/* ---- the register file -------------------------------------------------------------------------------------------- */
#define HSLOTS (1u << 14)
struct kv {
    uint64_t pa;
    uint32_t v;
    int used;
};

static struct {
    struct kv r[HSLOTS];
    uint64_t now;
    uint8_t *ram;
    uint8_t pmic[256], pmic_ptr;
    uint8_t sens[65536];
    uint16_t sens_ptr;
    int dom_on, smc_restored, streaming;
    unsigned ptr, frames, sens_frames;
    uint64_t next_frame;
    unsigned violations, torch_touch, offdom_rd, offdom_wr, pmic_bad_wr, pmic_writes, i2c_writes, i2c_msgs, i2c_resets,
        dma_outside, smc_order, rail_order, show_calls, show_bad, wr_count;
    char events[64][48];
    unsigned nev;
    unsigned rail_ctx_bad;
    /* faults */
    int f_pmic_nack, f_pmic_id, f_ldo_stuck, f_sensor_nack, f_dom_stuck, f_smc_refuse, f_noframes, f_dmaerr, f_noram,
        f_pmicbus_up, f_dpu_off;
    char log[6000][420];
    unsigned nlog;
    struct { uint64_t fn, a1, a2; } smc[32];
    unsigned nsmc;
} F;

static uint16_t *bars;                      /* 1632 x 1224 RAW16 GRBG colour bars */

static unsigned slot(uint64_t pa)
{
    unsigned h = (unsigned)((pa * 0x9E3779B97F4A7C15ULL) >> 50) & (HSLOTS - 1);
    while (F.r[h].used && F.r[h].pa != pa)
        h = (h + 1) & (HSLOTS - 1);
    return h;
}
static uint32_t reg(uint64_t pa)
{
    unsigned h = slot(pa);
    return F.r[h].used ? F.r[h].v : 0;
}
static void set(uint64_t pa, uint32_t v)
{
    unsigned h = slot(pa);
    F.r[h].used = 1, F.r[h].pa = pa, F.r[h].v = v;
}

static int in_isp(uint64_t pa)
{
    return (pa >= CMU_ISPPRE && pa < CMU_ISPPRE + 0x8000) || (pa >= SYSREG_ISPPRE && pa < SYSREG_ISPPRE + 0x1000) ||
           (pa >= CSIS && pa < CSIS + 0x1000) || (pa >= DPHY && pa < DPHY + 0x3000) || (pa >= WDMA && pa < WDMA + 0x1000) ||
           (pa >= CDMA && pa < CDMA + 0x1000);
}

/* the ISPPRE blocks at their reset values (domain power-on) or gone (off) */
static void isp_reset(void)
{
    for (unsigned i = 0; i < HSLOTS; ++i)
        if (F.r[i].used && in_isp(F.r[i].pa))
            F.r[i].used = 0;
    /* re-hash: removing entries from open addressing breaks chains: rebuild the table */
    struct kv *old = malloc(sizeof(F.r));
    memcpy(old, F.r, sizeof(F.r));
    memset(F.r, 0, sizeof(F.r));
    for (unsigned i = 0; i < HSLOTS; ++i)
        if (old[i].used)
            set(old[i].pa, old[i].v);
    free(old);
    if (F.dom_on)
        set(CSIS, 0x05010000U);             /* CSIS_VERSION */
}

/* ---- the sensor's power and the supplies -------------------------------------------------------------------------- */
static int rail(unsigned which)             /* 0 BUCK2, 1 LDO5 VDDD, 2 LDO6 VDDIO, 3 LDO13 VDDA, 4 LDO16 VDDAF */
{
    static const uint8_t r[5] = { 0x0D, 0x22, 0x23, 0x2A, 0x2D }, v[5] = { 0, 0x24, 0x30, 0x58, 0x58 };
    if (!which)
        return (F.pmic[0x0D] & 0xC0) == 0xC0 && F.pmic[0x0E] == 0x90;
    return (F.pmic[r[which]] & 0x80) && (F.pmic[r[which]] & 0x7F) == v[which];
}
static int reset_high(void)
{
    return ((reg(GPG2) >> 20) & 0xF) == 1 && (reg(GPG2 + 4) >> 5) & 1;
}
static int mclk_out(void)
{
    return ((reg(GPC1) >> 20) & 0xF) == 2 && (reg(CMUT + 0x201C) & 0x300000) == 0x300000 && !(reg(CMUT + 0x101C) & 1);
}
static int sbus_pins(void)
{
    return ((reg(GPP5) >> 8) & 0xFF) == 0x33;
}
static int sensor_powered(void)
{
    for (unsigned i = 0; i < 5; ++i)
        if (!rail(i))
            return 0;
    return mclk_out() && reset_high();
}

static int gate_closed(uint32_t g)
{
    return (g & 0x300000) == 0x100000;
}
static int bus_ok(unsigned bus)
{
    if (bus == CAM_BUS_PMIC)
        return reg(USI20) == 4 && (reg(GPP6) & 0xFF) == 0x22 && !gate_closed(reg(CMUP + 0x2048));
    return reg(USI16) == 4 && sbus_pins() && !gate_closed(reg(CMUP + 0x2050)) && !gate_closed(reg(CMUP + 0x2098)) &&
           !gate_closed(reg(CMUP + 0x20EC)) && !gate_closed(reg(CMUP + 0x20F0));
}

static void rail_event(const char *what)
{
    if (F.nev < 64)
        snprintf(F.events[F.nev++], 48, "%s", what);
    if (reset_high() || mclk_out() || sbus_pins())
        F.rail_ctx_bad++;
}

/* ---- the frames --------------------------------------------------------------------------------------------------- */
static int stream_path_ok(void)
{
    return F.streaming && F.dom_on && F.smc_restored && (reg(CSIS + 0x004) & 1) && (reg(CSIS + 0x704) & 1) &&
           ((reg(CSIS + 0x704) >> 1) & 0xF) == 3 && ((reg(CSIS + 0x040) >> 2) & 0x3F) == 0x2B &&
           reg(CSIS + 0x044) == (1224u << 16 | 1632u) && (reg(WDMA) & 1) && (reg(WDMA + 0x40) & 0x3FFF) == 1632 &&
           (reg(CMUT + 0x209C) & 0x300000) == 0x300000 && (reg(CMU_ISPPRE + 0x100) & 0x10) &&
           (reg(CMU_ISPPRE + 0x3024) & 2) && (reg(SYSREG_ISPPRE + 0x500) & 4) && (reg(PMU + 0x730) & 1) &&
           sensor_powered();
}

static void tick(void)
{
    while (F.streaming && F.now >= F.next_frame) {
        F.next_frame += 33333;
        if (!stream_path_ok() || F.f_noframes)
            continue;
        F.sens_frames++;
        if (F.f_dmaerr) {
            set(WDMA + 0x408, reg(WDMA + 0x408) | (1u << 12));
            continue;
        }
        uint32_t pa = reg(WDMA + 0x10 + 4 * F.ptr);
        if (pa < CAM_MEM_PA || (uint64_t)pa + 1632u * 1224u * 2u > CAM_MEM_PA + CAM_MEM_SIZE) {
            F.dma_outside++;
        } else {
            memcpy(F.ram + (pa - CAM_MEM_PA), bars, 1632u * 1224u * 2u);
            F.frames++;
        }
        set(WDMA + 0x408, reg(WDMA + 0x408) | (1u << 4) | (1u << 8));
        set(CSIS + 0x100, reg(CSIS + 0x100) + 1);
        F.ptr = (F.ptr + 1) & 7;
    }
}

/* ---- the HAL ------------------------------------------------------------------------------------------------------ */
static int allowed_write(uint64_t pa, uint32_t old, uint32_t v)
{
    struct { uint64_t pa; uint32_t mine; } shared[] = {
        { GPG2, 0xFu << 20 }, { GPG2 + 4, 1u << 5 }, { GPG2 + 8, 0xFu << 20 }, { GPG2 + 0xC, 0 },
        { GPC1, 0xFu << 20 }, { GPC1 + 4, 1u << 5 }, { GPC1 + 8, 0xFu << 20 },
        { GPP5, 0xFF00 }, { GPP5 + 8, 0xFF00 }, { GPP6, 0xFF }, { GPP6 + 8, 0xFF },
    };
    for (unsigned i = 0; i < sizeof(shared) / sizeof(shared[0]); ++i)
        if (pa == shared[i].pa) {
            if (pa == GPG2 + 4 && ((old ^ v) & (1u << 4)))
                F.torch_touch++;
            return ((old ^ v) & ~shared[i].mine) == 0;
        }
    static const uint64_t one[] = { USI16, USI20, CMUP + 0x2050, CMUP + 0x2098, CMUP + 0x20EC, CMUP + 0x20F0,
                                    CMUP + 0x303C, CMUT + 0x101C, CMUT + 0x1818, CMUT + 0x201C, CMUT + 0x3008,
                                    CMUT + 0x1094, CMUT + 0x1890, CMUT + 0x209C, PMU + 0x730, PMU + 0x4048,
                                    PMU + 0x4730, CMU_ISPPRE + 0x100, SYSREG_ISPPRE + 0x500 };
    for (unsigned i = 0; i < sizeof(one) / sizeof(one[0]); ++i)
        if (pa == one[i])
            return 1;
    if ((pa >= CMU_ISPPRE + 0x3010 && pa <= CMU_ISPPRE + 0x3070 && pa != CMU_ISPPRE + 0x303C) ||
        (pa >= CSIS && pa < CSIS + 0x1000) || (pa >= DPHY && pa < DPHY + 0x3000) || (pa >= WDMA && pa < WDMA + 0x1000) ||
        (pa >= CDMA && pa < CDMA + 0x40))
        return 1;
    return 0;
}

static int h_rd32(void *c, uint64_t pa, uint32_t *v)
{
    (void)c;
    tick();
    if (in_isp(pa) && !F.dom_on) {
        F.offdom_rd++;
        *v = 0;
        return 0;
    }
    if (pa == CMU_ISPPRE + 0x303C) {        /* aborted on the phone (an earlier port's notes) */
        F.offdom_rd++;
        *v = 0;
        return 0;
    }
    if (pa == WDMA + 0x30) {
        *v = (F.ptr << 2) | (reg(WDMA) & 1);
        return 1;
    }
    *v = reg(pa);
    return 1;
}

static void h_wr32(void *c, uint64_t pa, uint32_t v)
{
    (void)c;
    uint32_t old = reg(pa);
    F.wr_count++;
    if (!allowed_write(pa, old, v)) {
        F.violations++;
        printf("  (write to 0x%llx: 0x%x -> 0x%x not allowed)\n", (unsigned long long)pa, old, v);
    }
    if (in_isp(pa) && !F.dom_on) {
        F.offdom_wr++;
        return;
    }
    if (pa == WDMA + 0x408 || pa == CSIS + 0x014 || pa == CSIS + 0x01C) {
        set(pa, old & ~v);                  /* write 1 to clear */
        return;
    }
    set(pa, v);
    if (pa == PMU + 0x4048) {
        int on = (v & 0xF) == 0xF;
        if (F.f_dom_stuck && on)
            return;
        if (!on && F.dom_on && F.smc_restored)
            F.smc_order++;                  /* off without the DTZPC save */
        if (on != F.dom_on) {
            F.dom_on = on;
            set(PMU + 0x404C, on ? 0xF : 0);
            isp_reset();
        }
    } else if (pa == CSIS + 0x004 && (v & 2)) {
        set(pa, v & ~2u);                   /* the reset is done at once */
    } else if (pa == WDMA + 0x414 && (v & 1)) {
        set(WDMA + 0x408, reg(WDMA + 0x408) | (1u << 16));
    }
}

static void h_i2c_init(void *c, unsigned bus, uint32_t fs1, uint32_t fs2, uint32_t fs3, int reset)
{
    (void)c;
    if (reset) {
        uint64_t b = bus == CAM_BUS_PMIC ? HS_PMIC : HS_SENS;
        F.i2c_resets++;
        set(b + 0x60, fs1), set(b + 0x64, fs2), set(b + 0x68, fs3);     /* libsb's sb_i2c_bus writes them */
    }
}

static void pmic_write(uint8_t r, uint8_t v)
{
    static const uint8_t ok[] = { 0x0D, 0x0E, 0x22, 0x23, 0x2A, 0x2D };
    int fine = 0;
    for (unsigned i = 0; i < sizeof(ok); ++i)
        fine |= r == ok[i];
    if (!fine)
        F.pmic_bad_wr++;
    F.pmic_writes++;
    if (F.f_ldo_stuck && r == 0x2A)
        return;
    int before[5], after[5];
    for (unsigned i = 0; i < 5; ++i)
        before[i] = rail(i);
    F.pmic[r] = v;
    static const char *nm[5] = { "BUCK2", "VDDD", "VDDIO", "VDDA", "VDDAF" };
    for (unsigned i = 0; i < 5; ++i) {
        after[i] = rail(i);
        if (before[i] != after[i]) {
            char e[48];
            snprintf(e, sizeof(e), "%s %s", nm[i], after[i] ? "on" : "off");
            rail_event(e);
        }
    }
}

static int h_i2c(void *c, unsigned bus, unsigned addr, int rx, uint8_t *b, unsigned n, int stop)
{
    (void)c, (void)stop;
    tick();
    F.i2c_msgs++;
    if (!rx)
        F.i2c_writes += n > (bus == CAM_BUS_PMIC ? 1u : 2u);
    if (!bus_ok(bus))
        return 0;
    if (bus == CAM_BUS_PMIC) {
        if (addr != 0x59 || F.f_pmic_nack)
            return 0;
        if (rx) {
            for (unsigned i = 0; i < n; ++i)
                b[i] = F.pmic[(uint8_t)(F.pmic_ptr + i)];
        } else {
            F.pmic_ptr = b[0];
            for (unsigned i = 1; i < n; ++i)
                pmic_write((uint8_t)(b[0] + i - 1), b[i]);
        }
        return 1;
    }
    if (addr != 0x35 || F.f_sensor_nack || !sensor_powered())
        return 0;
    if (rx) {
        for (unsigned i = 0; i < n; ++i) {
            uint16_t a = (uint16_t)(F.sens_ptr + i);
            b[i] = a == 0x0005 ? (F.streaming ? (uint8_t)(F.sens_frames % 255u) : 0xFF) : F.sens[a];
        }
    } else {
        if (n < 2)
            return 0;
        F.sens_ptr = (uint16_t)(b[0] << 8 | b[1]);
        for (unsigned i = 2; i < n; ++i)
            F.sens[(uint16_t)(F.sens_ptr + i - 2)] = b[i];
        if (n >= 3 && F.sens_ptr == 0x0100) {
            int on = b[2] == 1 && F.sens[0x034C] == 0x06 && F.sens[0x034D] == 0x60 && F.sens[0x0136] == 0x1A;
            if (on && !F.streaming)
                F.next_frame = F.now + 33333;
            F.streaming = on;
        }
    }
    return 1;
}

static int64_t h_smc(void *c, uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    (void)c, (void)a3;
    if (F.nsmc < 32)
        F.smc[F.nsmc].fn = fn, F.smc[F.nsmc].a1 = a1, F.smc[F.nsmc].a2 = a2, F.nsmc++;
    if (fn != 0x82000410ULL || a2 != 0x16210004ULL)
        return -1;
    if (a1 == 1) {
        if (F.f_smc_refuse)
            return -1;
        if (!F.dom_on)
            F.smc_order++;
        F.smc_restored = 1;
    } else {
        if (!F.dom_on)
            F.smc_order++;
        F.smc_restored = 0;
    }
    return 0;
}

static void h_delay(void *c, uint32_t us)
{
    (void)c;
    F.now += us;
    tick();
}

static uint64_t h_now(void *c)
{
    (void)c;
    F.now += 1;
    tick();
    return F.now;
}

static void h_log(void *c, const char *s)
{
    (void)c;
    if (verbose)
        printf("    %s\n", s);
    if (F.nlog < 6000)
        snprintf(F.log[F.nlog++], 420, "%s", s);
}

static void *h_ram(void *c, uint64_t pa, uint64_t len)
{
    (void)c;
    if (F.f_noram || pa < CAM_MEM_PA || pa + len > CAM_MEM_PA + CAM_MEM_SIZE)
        return NULL;
    return F.ram + (pa - CAM_MEM_PA);
}

static void h_show(void *c, const uint32_t *px, unsigned w, unsigned h, unsigned stride)
{
    (void)c;
    F.show_calls++;
    if (w != 360 || h != 480 || stride < w || !px)
        F.show_bad++;
}

/* ---- the phone as found ------------------------------------------------------------------------------------------- */
static void phone_reset(void)
{
    uint8_t *ram = F.ram;
    memset(&F, 0, sizeof(F));
    F.ram = ram;
    memset(F.ram, 0x5A, CAM_MEM_SIZE);
    F.now = 1000000;
    set(GPG2, (1u << 16));                  /* the torch's pin an output; the reset an input */
    set(GPG2 + 4, (1u << 4));               /* the torch on, for the test: its bit must never change */
    set(GPG2 + 8, (1u << 20) | 0x3);
    set(GPC1, (1u << 20) | 0x2);            /* MCLK1 an output (fimc_is_mclk1_out) */
    set(GPC1 + 8, (1u << 20));
    set(GPP5, 0x22);                        /* gpp5-0/1 someone else's */
    set(GPP6, 0x0);
    set(USI16, 0);
    set(USI20, 0);
    set(CMUP + 0x2050, 0x00100000);         /* the sensor bus's gates closed: opened and closed again */
    set(CMUP + 0x2098, 0x00100000);
    set(CMUP + 0x20EC, 0);
    set(CMUP + 0x20F0, 0);
    set(CMUT + 0x101C, 1);                  /* CIS_CLK1 on the PLL, divided: put back as found */
    set(CMUT + 0x1818, 3);
    set(CMUT + 0x201C, 0x00100000);
    set(CMUT + 0x1094, 2);
    set(CMUT + 0x1890, 0xF);
    set(CMUT + 0x209C, 0x00100000);
    set(PMU + 0x4024, 0xF);                 /* the display's domain on */
    set(PMU + 0x4730, 1);
    memset(F.pmic, 0x11, sizeof(F.pmic));
    F.pmic[0x1D] = 0x90;
    F.pmic[0x00] = 0x0C;
    F.pmic[0x0D] = 0x08, F.pmic[0x0E] = 0x88;   /* BUCK2 off at another voltage: may be set to 1.3 V */
    F.pmic[0x22] = 0x20, F.pmic[0x23] = 0x30, F.pmic[0x2A] = 0x58, F.pmic[0x2D] = 0x10;
    set(HS_PMIC + 0x68, 0);
    set(HS_SENS + 0x68, 0x00050000);
    F.sens[0] = 0x30, F.sens[1] = 0x81, F.sens[2] = 0xA0;   /* a model ID and revision for the test */
}

static void pmicbus_up_as_torch(void)
{
    set(USI20, 4);
    set(GPP6, 0x22);
    set(HS_PMIC + 0x60, 0x01F0FF00);
    set(HS_PMIC + 0x64, 0x030003E0);
    set(HS_PMIC + 0x68, 0x00210000);
}

/* a copy of everything that must come back as found */
struct snapshot {
    struct kv r[HSLOTS];
    uint8_t pmic[256];
};
static struct snapshot before;

static void take(struct snapshot *s)
{
    memcpy(s->r, F.r, sizeof(F.r));
    memcpy(s->pmic, F.pmic, 256);
}

static int skip_cmp(uint64_t pa)
{
    /* the ISPPRE blocks (powered off again), the HSI2C controllers' own registers, and the S2MPB02's bus, which is
     * left set up as the torch driver leaves it */
    return in_isp(pa) || (pa >= HS_PMIC && pa < HS_PMIC + 0x1000) || (pa >= HS_SENS && pa < HS_SENS + 0x1000) ||
           pa == USI20 || pa == GPP6 || pa == GPP6 + 8 || pa == PMU + 0x404C;
}

static unsigned as_found(const char *t)
{
    unsigned bad = 0;
    for (unsigned i = 0; i < HSLOTS; ++i) {
        if (!before.r[i].used || skip_cmp(before.r[i].pa))
            continue;
        uint32_t now = reg(before.r[i].pa);
        if (now != before.r[i].v) {
            bad++;
            printf("  %s: 0x%llx is 0x%x, found 0x%x\n", t, (unsigned long long)before.r[i].pa, now, before.r[i].v);
        }
    }
    for (unsigned i = 0; i < HSLOTS; ++i)
        if (F.r[i].used && !skip_cmp(F.r[i].pa)) {
            int was = 0;
            for (unsigned k = 0; k < HSLOTS && !was; ++k)
                was = before.r[k].used && before.r[k].pa == F.r[i].pa;
            if (!was && F.r[i].v) {
                bad++;
                printf("  %s: 0x%llx is 0x%x, was never set\n", t, (unsigned long long)F.r[i].pa, F.r[i].v);
            }
        }
    for (unsigned i = 0; i < 256; ++i)
        if (F.pmic[i] != before.pmic[i]) {
            bad++;
            printf("  %s: S2MPB02 0x%02x is 0x%02x, found 0x%02x\n", t, i, F.pmic[i], before.pmic[i]);
        }
    if (F.dom_on) {
        bad++;
        printf("  %s: the ISPPRE domain is on\n", t);
    }
    if (F.streaming) {
        bad++;
        printf("  %s: the sensor still streams\n", t);
    }
    return bad;
}

static int logged(const char *needle)
{
    for (unsigned i = 0; i < F.nlog; ++i)
        if (strstr(F.log[i], needle))
            return 1;
    return 0;
}

static const char *result_line(void)
{
    for (unsigned i = F.nlog; i-- > 0;)
        if (strstr(F.log[i], "drvprobe: cam: PASS") || strstr(F.log[i], "drvprobe: cam: FAIL"))
            return F.log[i];
    return "";
}

static void hal_install(void)
{
    struct cam_hal h = { 0, h_rd32, h_wr32, h_i2c_init, h_i2c, h_smc, h_delay, h_now, h_log, h_ram, h_show };
    cam_hal_set(&h);
}

/* runs the probe to its end; 1 PASS, 0 FAIL, -1 never ended */
static int run_probe(int level)
{
    unsigned w = 0;
    for (unsigned i = 0; i < 200000; ++i) {
        int r = drv_cam_probe(level, &w);
        if (r == 1)
            return strstr(result_line(), "PASS") ? 1 : 0;
        F.now += w ? (uint64_t)w * 1000u : 50u;
        tick();
    }
    return -1;
}

static void common_checks(const char *t)
{
    CHECK(F.violations == 0, "%s: %u writes outside the camera's registers / bits", t, F.violations);
    CHECK(F.torch_touch == 0, "%s: the torch's pin was changed", t);
    CHECK(F.offdom_wr == 0, "%s: %u writes to the ISPPRE blocks with the domain off", t, F.offdom_wr);
    CHECK(F.offdom_rd == 0, "%s: %u reads of the ISPPRE blocks with the domain off", t, F.offdom_rd);
    CHECK(F.pmic_bad_wr == 0, "%s: a write to an S2MPB02 register that is not the camera's", t);
    CHECK(F.dma_outside == 0, "%s: the WDMA was given an address outside the camera's memory", t);
    CHECK(F.smc_order == 0, "%s: the SMCs and the domain in the wrong order", t);
    CHECK(F.rail_ctx_bad == 0, "%s: a supply switched while the reset was high, MCLK out or the bus pins on", t);
    unsigned bad = as_found(t);
    CHECK(bad == 0, "%s: %u things not as found", t, bad);
}

/* the supplies' order: on BUCK2 VDDAF VDDA VDDIO VDDD, off VDDAF VDDD VDDA VDDIO BUCK2 (those that were switched) */
static void order_checks(const char *t, int expect_on)
{
    static const char *on[] = { "BUCK2 on", "VDDAF on", "VDDA on", "VDDIO on", "VDDD on" };
    static const char *off[] = { "VDDAF off", "VDDD off", "VDDA off", "VDDIO off", "BUCK2 off" };
    unsigned k = 0, j = 0;
    for (unsigned i = 0; i < F.nev; ++i) {
        if (strstr(F.events[i], " on")) {
            while (k < 5 && strcmp(on[k], F.events[i]))
                k++;
            CHECK(k < 5, "%s: supply event '%s' out of order", t, F.events[i]);
        } else {
            while (j < 5 && strcmp(off[j], F.events[i]))
                j++;
            CHECK(j < 5, "%s: supply event '%s' out of order", t, F.events[i]);
        }
    }
    if (expect_on)
        CHECK(F.nev >= 10, "%s: only %u supply events", t, F.nev);
}

/* ---- the ISP tests ------------------------------------------------------------------------------------------------ */
static const uint8_t bar_rgb[8][3] = { { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 0, 1, 0 },
                                       { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, 0 } };

/* GRBG: row 0 G R, row 1 B G; level per channel in 1/1000 of full scale (above black), per bar */
static void make_bars(uint16_t *p, unsigned w, unsigned h, const unsigned lvl[8][3])
{
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            unsigned b = x * 8u / w, ch;
            if (!(y & 1))
                ch = (x & 1) ? 0 : 1;
            else
                ch = (x & 1) ? 1 : 2;
            p[(size_t)y * w + x] = (uint16_t)(64u + 959u * lvl[b][ch] / 1000u);
        }
}

static void pack10(const uint16_t *src, unsigned w, unsigned h, uint8_t *dst, unsigned stride)
{
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; x += 4) {
            uint8_t *g = dst + (size_t)y * stride + x / 4 * 5;
            g[4] = 0;
            for (unsigned k = 0; k < 4; ++k) {
                uint16_t v = src[(size_t)y * w + x + k];
                g[k] = (uint8_t)(v >> 2);
                g[4] |= (uint8_t)((v & 3) << (2 * k));
            }
        }
}

static int near(unsigned a, unsigned b, unsigned tol)
{
    return a + tol >= b && b + tol >= a;
}

/* a stand-in for kern_next.c's band workers: the bands in a scrambled order */
static void fake_par(void (*fn)(void *, int), void *arg, int n)
{
    for (int i = n - 1; i >= 0; i -= 2) fn(arg, i);
    for (int i = n - 2; i >= 0; i -= 2) fn(arg, i);
}
static void test_isp(void)
{
    const unsigned W = 1632, H = 1224;
    unsigned full[8][3], half[8][3];
    for (unsigned b = 0; b < 8; ++b)
        for (unsigned c = 0; c < 3; ++c)
            full[b][c] = bar_rgb[b][c] * 1000u, half[b][c] = bar_rgb[b][c] * 500u;
    uint16_t *raw = malloc((size_t)W * H * 2);
    uint8_t *packed = malloc((size_t)W * 5 / 4 * H);
    uint32_t *out = malloc(816u * 816u * 4u), *out2 = malloc(816u * 816u * 4u);

    /* the colour bars at full and half level, both raw formats, white balance from grey world */
    for (unsigned pass = 0; pass < 2; ++pass) {
        make_bars(raw, W, H, pass ? half : full);
        struct cam_raw r = { (const uint8_t *)raw, W, H, W * 2, CAM_RAW16, CAM_BAYER_GRBG, 64 };
        struct cam_stats s;
        struct cam_wb wb;
        cam_isp_stats(&r, 4, &s);
        cam_isp_wb_grey(&s, 64, &wb);
        CHECK(near(wb.r, 256, 8) && near(wb.b, 256, 8), "bars: grey world gains %u %u (256 expected)", wb.r, wb.b);
        CHECK(s.over == 0 && s.max <= 1023, "bars: stats over %u max %u", s.over, s.max);
        struct cam_out o = { out, 320, 240, 320, CAM_ROT0 };
        cam_isp_render(&r, &wb, &o, 0, 240);
        unsigned want = pass ? 180u : 255u;     /* 255 x sqrt(0.5) */
        for (unsigned b = 0; b < 8; ++b) {
            uint32_t p = out[120 * 320 + (2 * b + 1) * 320 / 16];
            unsigned R = (p >> 16) & 0xFF, G = (p >> 8) & 0xFF, B = p & 0xFF;
            CHECK(near(R, bar_rgb[b][0] * want, 8) && near(G, bar_rgb[b][1] * want, 8) &&
                      near(B, bar_rgb[b][2] * want, 8) && (p >> 24) == 0xFF,
                  "bars level %u: bar %u is %u %u %u, expected %u %u %u", pass, b, R, G, B, bar_rgb[b][0] * want,
                  bar_rgb[b][1] * want, bar_rgb[b][2] * want);
        }
        /* the same from MIPI-packed RAW10 */
        pack10(raw, W, H, packed, W * 5 / 4);
        struct cam_raw rp = { packed, W, H, W * 5 / 4, CAM_RAW10P, CAM_BAYER_GRBG, 64 };
        struct cam_out o2 = { out2, 320, 240, 320, CAM_ROT0 };
        cam_isp_render(&rp, &wb, &o2, 0, 240);
        CHECK(!memcmp(out, out2, 320u * 240u * 4u), "bars level %u: RAW10 packed and RAW16 differ", pass);
        /* rendering in pieces gives the same picture */
        memset(out2, 0, 320u * 240u * 4u);
        struct cam_out o3 = { out2, 320, 240, 320, CAM_ROT0 };
        for (unsigned y = 0; y < 240; y += 37)
            cam_isp_render(&r, &wb, &o3, y, y + 37);
        CHECK(!memcmp(out, out2, 320u * 240u * 4u), "bars level %u: rendering in pieces differs", pass);
    }

    /* a colour cast: R at half its level everywhere; grey world brings the white bar back to white */
    {
        unsigned tint[8][3];
        for (unsigned b = 0; b < 8; ++b)
            tint[b][0] = full[b][0] / 2, tint[b][1] = full[b][1] / 2, tint[b][2] = full[b][2] / 2;
        for (unsigned b = 0; b < 8; ++b)
            tint[b][0] /= 2;
        make_bars(raw, W, H, tint);
        struct cam_raw r = { (const uint8_t *)raw, W, H, W * 2, CAM_RAW16, CAM_BAYER_GRBG, 64 };
        struct cam_stats s;
        struct cam_wb wb, one = { 256, 256, 256 };
        cam_isp_stats(&r, 4, &s);
        cam_isp_wb_grey(&s, 64, &wb);
        CHECK(near(wb.r, 512, 12), "cast: the red gain is %u, 512 expected", wb.r);
        struct cam_out o = { out, 320, 240, 320, CAM_ROT0 };
        cam_isp_render(&r, &wb, &o, 0, 240);
        uint32_t p = out[120 * 320 + 10];
        unsigned R = (p >> 16) & 0xFF, G = (p >> 8) & 0xFF, B = p & 0xFF;
        CHECK(near(R, G, 6) && near(B, G, 6) && near(G, 180, 8), "cast: the white bar is %u %u %u after balance", R, G,
              B);
        cam_isp_render(&r, &one, &o, 0, 240);
        p = out[120 * 320 + 10];
        CHECK(((p >> 16) & 0xFF) + 30 < ((p >> 8) & 0xFF), "cast: without balance the red is not lower (%x)", p);
    }

    /* the rotations and the mirror: a red patch in the sensor's top-left corner, green elsewhere */
    {
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x) {
                int red = x < W / 4 && y < H / 4;
                unsigned ch = !(y & 1) ? ((x & 1) ? 0u : 1u) : ((x & 1) ? 1u : 2u);
                unsigned v = red ? (ch == 0 ? 1000u : 0u) : (ch == 1 ? 1000u : 0u);
                raw[(size_t)y * W + x] = (uint16_t)(64u + 959u * v / 1000u);
            }
        struct cam_raw r = { (const uint8_t *)raw, W, H, W * 2, CAM_RAW16, CAM_BAYER_GRBG, 64 };
        struct cam_wb one = { 256, 256, 256 };
        /* where the patch must land: 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left */
        struct { unsigned xf, corner; } t[] = {
            { CAM_ROT0, 0 }, { CAM_ROT90, 1 }, { CAM_ROT180, 2 }, { CAM_ROT270, 3 },
            { CAM_ROT0 | CAM_MIRROR, 1 }, { CAM_ROT90 | CAM_MIRROR, 0 }, { CAM_ROT270 | CAM_MIRROR, 2 },
        };
        for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); ++i) {
            unsigned ow = (t[i].xf & 1) ? 240 : 320, oh = (t[i].xf & 1) ? 320 : 240;
            struct cam_out o = { out, ow, oh, ow, t[i].xf };
            cam_isp_render(&r, &one, &o, 0, oh);
            unsigned cx[4] = { 5, ow - 6, ow - 6, 5 }, cy[4] = { 5, 5, oh - 6, oh - 6 };
            for (unsigned c = 0; c < 4; ++c) {
                uint32_t p = out[cy[c] * ow + cx[c]];
                int red = ((p >> 16) & 0xFF) > 200 && ((p >> 8) & 0xFF) < 40;
                CHECK(red == (c == t[i].corner), "xform %u: corner %u is %06x (red expected at corner %u)", t[i].xf, c,
                      p & 0xFFFFFF, t[i].corner);
            }
        }
    }

    /* ---- v2 (Face Unlock round 2): the same orientation as v1 for every xform, a flat field, white balance from the
     * near-grey pixels, the ROI's statistics, the noise against v1, the time ---- */
    {
        struct cam_raw r = { (const uint8_t *)raw, W, H, W * 2, CAM_RAW16, CAM_BAYER_GRBG, 64 };   /* (the red patch) */
        struct cam_wb one = { 256, 256, 256 };
        struct cam_tune tu;
        cam_tune_default(&tu);
        for (unsigned xf = 0; xf < 8; ++xf) {
            unsigned ow = (xf & 1) ? 240 : 320, oh = (xf & 1) ? 320 : 240;
            struct cam_out o1 = { out, ow, oh, ow, xf }, o2 = { out2, ow, oh, ow, xf };
            cam_isp_render(&r, &one, &o1, 0, oh);
            cam_isp_process(&r, &one, &tu, &o2, 0, 0, cam_isp_rows(&o2), 0);
            unsigned bad = 0;
            for (unsigned y = 4; y < oh; y += 8)
                for (unsigned x = 4; x < ow; x += 8) {
                    int r1 = ((out[y * ow + x] >> 16) & 0xFF) > 128, r2 = ((out2[y * ow + x] >> 16) & 0xFF) > 128;
                    /* away from the patch's edge (the area resample blends one pixel there) */
                    int edge = 0;
                    for (int dy = -2; dy <= 2 && !edge; dy += 4)
                        for (int dx = -2; dx <= 2 && !edge; dx += 4) {
                            int yy = (int)y + dy, xx = (int)x + dx;
                            if (yy >= 0 && xx >= 0 && yy < (int)oh && xx < (int)ow &&
                                (((out[yy * ow + xx] >> 16) & 0xFF) > 128) != r1)
                                edge = 1;
                        }
                    bad += !edge && r1 != r2;
                }
            CHECK(bad == 0, "v2 xform %u: the red patch differs from v1 at %u points", xf, bad);
        }
        /* a flat grey field: flat out without lens shading; the corners 1.4x brighter (linear) with the default */
        for (size_t i = 0; i < (size_t)W * H; ++i)
            raw[i] = 64 + 300;
        struct cam_out o = { out, 360, 480, 360, CAM_ROT270 };
        tu.lsc_corner = 256;
        struct cam_pstats ps;
        memset(&ps, 0, sizeof ps);
        cam_isp_process(&r, &one, &tu, &o, 0, 0, cam_isp_rows(&o), &ps);
        unsigned lo = 255, hi = 0;
        for (size_t i = 0; i < 360u * 480u; ++i) {
            unsigned g = (out[i] >> 8) & 0xFF;
            lo = g < lo ? g : lo, hi = g > hi ? g : hi;
        }
        CHECK(hi - lo <= 1 && ps.n == 360u * 480u, "v2 flat field: green %u..%u over %u pixels", lo, hi, ps.n);
        uint32_t ymean = (uint32_t)(ps.ysum / ps.n);
        CHECK(near(ymean, 300u * 4095u / 959u, 8), "v2 flat field: luma %u (12-bit; %u expected)", ymean, 300u * 4095u / 959u);
        cam_tune_default(&tu);
        cam_isp_process(&r, &one, &tu, &o, 0, 0, cam_isp_rows(&o), 0);
        unsigned mid = (out[240 * 360 + 180] >> 8) & 0xFF, corner = (out[2 * 360 + 2] >> 8) & 0xFF;
        CHECK(corner > mid + 15, "v2 lens shading: the corner %u brighter than the middle %u", corner, mid);
        /* a cast: red at half; the near-grey white balance finds 2x (a grey field is all near grey once balanced) */
        for (size_t y = 0; y < H; ++y)
            for (size_t x = 0; x < W; ++x)
                raw[y * W + x] = (uint16_t)(64 + ((!(y & 1) && (x & 1)) ? 200 : 400));   /* GRBG: R at (odd x, even y) */
        struct cam_wb wb2 = { 512, 256, 256 };
        memset(&ps, 0, sizeof ps);
        cam_isp_process(&r, &wb2, &tu, &o, 0, 0, cam_isp_rows(&o), &ps);
        CHECK(ps.gn > ps.n / 2 && near((unsigned)(ps.gsum[1] * 256 / ps.gsum[0]), 512, 30),
              "v2 near grey: %u of %u pixels, G/R %u (512 expected)", ps.gn, ps.n,
              ps.gn ? (unsigned)(ps.gsum[1] * 256 / ps.gsum[0]) : 0);
        /* the ROI: a box in the output's pixels gathers only its own luma */
        for (size_t i = 0; i < (size_t)W * H; ++i)
            raw[i] = 64 + 100;
        for (size_t y = 0; y < H / 2; ++y)                  /* the sensor's top half brighter */
            for (size_t x = 0; x < W; ++x)
                raw[y * W + x] = 64 + 800;
        tu.lsc_corner = 256;
        struct cam_roi roi = { 1, 0, 0, 100, 100 };         /* ROT270: the output's top-left = the sensor's top-right */
        memset(&ps, 0, sizeof ps);
        cam_isp_process(&r, &one, &tu, &o, &roi, 0, cam_isp_rows(&o), &ps);
        CHECK(ps.rn == 100u * 100u && near((unsigned)(ps.rsum / ps.rn), 800u * 4095u / 959u, 60),
              "v2 ROI: %u pixels, luma %u (the bright half expected)", ps.rn, ps.rn ? (unsigned)(ps.rsum / ps.rn) : 0);
        /* noise: a grey field with +-40 noise; v2's averaging must leave less of it than v1's one cell a pixel */
        unsigned seed = 1;
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            seed = seed * 1103515245u + 12345u;
            raw[i] = (uint16_t)(64 + 300 + (int)((seed >> 16) % 81) - 40);
        }
        struct cam_out o1 = { out2, 360, 480, 360, CAM_ROT270 };
        cam_isp_render(&r, &one, &o1, 0, 480);
        /* (the same tone and no colour matrix: the averaging alone; the matrix and the S curve add contrast) */
        tu.lsc_corner = 256, tu.tone = 0;
        for (int k = 0; k < 9; ++k) tu.ccm[k] = (int16_t)(k % 4 == 0 ? 1024 : 0);
        cam_isp_process(&r, &one, &tu, &o, 0, 0, cam_isp_rows(&o), 0);
        double v1s = 0, v2s = 0, m1 = 0, m2 = 0;
        for (size_t i = 0; i < 360u * 480u; ++i) m1 += (out2[i] >> 8) & 0xFF, m2 += (out[i] >> 8) & 0xFF;
        m1 /= 360.0 * 480, m2 /= 360.0 * 480;
        for (size_t i = 0; i < 360u * 480u; ++i) {
            double a = ((out2[i] >> 8) & 0xFF) - m1, b = ((out[i] >> 8) & 0xFF) - m2;
            v1s += a * a, v2s += b * b;
        }
        v1s = sqrt(v1s / (360.0 * 480)), v2s = sqrt(v2s / (360.0 * 480));
        CHECK(v2s * 1.4 < v1s, "v2 noise: sd %.2f against v1's %.2f (8-bit green)", v2s, v1s);
        /* four bands (the band workers) give the very same picture and statistics as one */
        {
            struct cam_pstats p1, p4;
            cam_tune_default(&tu);
            memset(&p1, 0, sizeof p1);
            memset(&p4, 0, sizeof p4);
            cam_isp_set_par(0);
            struct cam_out oa = { out, 360, 480, 360, CAM_ROT270 | CAM_MIRROR }, ob = { out2, 360, 480, 360, CAM_ROT270 | CAM_MIRROR };
            struct cam_roi rr = { 1, 50, 60, 120, 140 };
            struct cam_wb w3 = { 300, 256, 420 };
            cam_isp_process(&r, &w3, &tu, &oa, &rr, 0, cam_isp_rows(&oa), &p1);
            cam_isp_set_par(fake_par);
            cam_isp_process(&r, &w3, &tu, &ob, &rr, 0, cam_isp_rows(&ob), &p4);
            cam_isp_set_par(0);
            CHECK(!memcmp(out, out2, 360u * 480u * 4u) && !memcmp(&p1, &p4, sizeof p1),
                  "v2 in four bands: the same picture and statistics as one band");
        }
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int k = 0; k < 10; ++k) cam_isp_process(&r, &one, &tu, &o, 0, 0, cam_isp_rows(&o), &ps);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 10;
        printf("cam-test: v2 360 x 480 from 1632 x 1224: %.2f ms a frame on this host (cached memory)\n", ms);
    }

    /* BMP */
    {
        uint32_t px[6] = { 0xFF102030, 0xFF405060, 0xFF708090, 0xFFA0B0C0, 0xFFD0E0F0, 0xFF010203 };
        uint8_t bmp[200];
        uint32_t n = cam_isp_bmp(px, 3, 2, 3, bmp, sizeof(bmp));
        CHECK(n == 54 + 12 * 2 && n == cam_isp_bmp_size(3, 2), "bmp: length %u", n);
        CHECK(bmp[0] == 'B' && bmp[1] == 'M' && bmp[10] == 54 && bmp[14] == 40 && bmp[18] == 3 && bmp[22] == 2 &&
                  bmp[28] == 24, "bmp: header");
        /* bottom-up: the file's first row is the picture's last; BGR */
        CHECK(bmp[54] == 0xC0 && bmp[55] == 0xB0 && bmp[56] == 0xA0 && bmp[54 + 9] == 0 && bmp[54 + 12] == 0x30 &&
                  bmp[54 + 13] == 0x20 && bmp[54 + 14] == 0x10, "bmp: pixels");
        CHECK(cam_isp_bmp(px, 3, 2, 3, bmp, 60) == 0, "bmp: a small buffer must be refused");
    }

    /* stats: known values, a fill shows as above 10 bits */
    {
        for (size_t i = 0; i < (size_t)W * H; ++i)
            raw[i] = (uint16_t)((i / W) & 1 ? ((i & 1) ? 300 : 200) : ((i & 1) ? 400 : 300));   /* G R / B G */
        raw[0] = 0xA5A5;
        struct cam_raw r = { (const uint8_t *)raw, W, H, W * 2, CAM_RAW16, CAM_BAYER_GRBG, 64 };
        struct cam_stats s;
        cam_isp_stats(&r, 1, &s);
        CHECK(s.mean[0] == 400 && s.mean[3] == 200 && near(s.mean[1], 300, 1) && s.mean[2] == 300 && s.over == 1 &&
                  s.n == 816u * 612u, "stats: %u %u %u %u over %u n %u", s.mean[0], s.mean[1], s.mean[2], s.mean[3],
              s.over, s.n);
    }
    free(raw), free(packed), free(out), free(out2);
}

/* ---- the driver tests --------------------------------------------------------------------------------------------- */
static void test_level0(void)
{
    phone_reset();
    take(&before);
    int r = run_probe(0);
    CHECK(r == 1, "level 0: %s", result_line());
    CHECK(F.wr_count == 0 && F.i2c_msgs == 0 && F.i2c_resets == 0 && F.nsmc == 0,
          "level 0 must not write: %u writes, %u I2C messages, %u controller resets, %u SMCs", F.wr_count, F.i2c_msgs,
          F.i2c_resets, F.nsmc);
    CHECK(logged("not set up yet"), "level 0: the S2MPB02's bus down should be said");
    common_checks("level 0");

    phone_reset();
    pmicbus_up_as_torch();
    take(&before);
    r = run_probe(0);
    CHECK(r == 1, "level 0 (bus up): %s", result_line());
    CHECK(F.wr_count == 0 && F.i2c_writes == 0 && F.i2c_resets == 0 && F.i2c_msgs > 0,
          "level 0 (bus up): %u writes, %u I2C writes, %u resets, %u messages", F.wr_count, F.i2c_writes, F.i2c_resets,
          F.i2c_msgs);
    CHECK(logged("S2MPB02: 0x1D 0x90"), "level 0 (bus up): the S2MPB02's registers should be logged");
    common_checks("level 0 (bus up)");
}

static void test_level1_good(void)
{
    phone_reset();
    take(&before);
    int r = run_probe(1);
    CHECK(r == 1, "level 1: %s", result_line());
    CHECK(logged("THE SENSOR ANSWERS at 0x35: model ID 0x3081"), "level 1: the ID should be logged");
    CHECK(logged("the last frame (slot statistics"), "level 1: the frame's statistics should be logged");
    CHECK(F.frames >= 4, "level 1: only %u frames", F.frames);
    CHECK(F.nsmc == 2 && F.smc[0].a1 == 1 && F.smc[1].a1 == 0, "level 1: the SMCs (%u)", F.nsmc);
    order_checks("level 1", 1);
    common_checks("level 1");
    /* the bars as the driver saw them: each channel on in four bars of eight: means near (4 x 1023 + 4 x 64) / 8 */
    {
        unsigned m[4] = { 0, 0, 0, 0 }, got = 0;
        for (unsigned i = 0; i < F.nlog && !got; ++i) {
            const char *l = strstr(F.log[i], "the last frame (slot statistics, every 4th cell): R ");
            if (l)
                got = sscanf(strchr(l, ':') + 2, "R %u Gr %u Gb %u B %u", &m[0], &m[1], &m[2], &m[3]) == 4;
        }
        CHECK(got && near(m[0], 543, 12) && near(m[1], 543, 12) && near(m[2], 543, 12) && near(m[3], 543, 12),
              "level 1: the bars' statistics %u %u %u %u (about 543 each expected)", m[0], m[1], m[2], m[3]);
    }

    /* the supplies found on at the right voltages are left on; the torch's bus found up is used as it is */
    phone_reset();
    pmicbus_up_as_torch();
    F.pmic[0x0D] = 0xC8, F.pmic[0x0E] = 0x90, F.pmic[0x23] = 0xB0;
    take(&before);
    r = run_probe(1);
    CHECK(r == 1, "level 1 (rails on): %s", result_line());
    CHECK(rail(0) && rail(2), "level 1 (rails on): BUCK2 / VDDIO found on must stay on");
    order_checks("level 1 (rails on)", 0);
    common_checks("level 1 (rails on)");
}

struct inject {
    const char *name;
    int *flag;
    const char *why;                        /* in the FAIL line */
    int no_pmic_write;
};

static void test_level1_faults(void)
{
    struct inject t[] = {
        { "S2MPB02 silent", &F.f_pmic_nack, "power chip does not answer", 1 },
        { "S2MPB02 wrong ID", &F.f_pmic_id, "not the expected one", 1 },
        { "VDDA does not read back", &F.f_ldo_stuck, "did not switch on", 0 },
        { "sensor silent", &F.f_sensor_nack, "front camera does not answer", 0 },
        { "ISPPRE stuck off", &F.f_dom_stuck, "power domain did not come on", 0 },
        { "SMC refused", &F.f_smc_refuse, "secure monitor refused", 0 },
        { "no frames", &F.f_noframes, "No picture", 0 },
        { "DMA errors", &F.f_dmaerr, "No picture", 0 },
        { "no memory", &F.f_noram, "memory is not available", 1 },
        { "display domain off", &F.f_dpu_off, "display's is off", 1 },
        { "a rail at another voltage", &F.f_pmicbus_up, "another voltage", 1 },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); ++i) {
        phone_reset();
        *t[i].flag = 1;
        if (F.f_pmic_id)
            F.pmic[0x1D] = 0x91;
        if (F.f_dpu_off)
            set(PMU + 0x4024, 0);
        if (F.f_pmicbus_up)
            F.pmic[0x2A] = 0xD0;            /* VDDA on at 2.0 V: someone else's */
        take(&before);
        int r = run_probe(1);
        const char *l = result_line();
        CHECK(r == 0 && strstr(l, t[i].why), "%s: expected FAIL '%s', got '%s'", t[i].name, t[i].why, l);
        if (t[i].no_pmic_write)
            CHECK(F.pmic_writes == 0, "%s: %u S2MPB02 writes (none expected)", t[i].name, F.pmic_writes);
        order_checks(t[i].name, 0);
        common_checks(t[i].name);
    }
}

static void test_level2(void)
{
    phone_reset();
    take(&before);
    int r = run_probe(2);
    CHECK(r == 1, "level 2: %s", result_line());
    CHECK(F.show_calls >= 250 && F.show_bad == 0, "level 2: %u pictures shown (%u bad)", F.show_calls, F.show_bad);
    CHECK(logged("the still: 612 x 816, BMP 1498230 bytes"), "level 2: the still (612 x 816 BMP) should be logged");
    CHECK(logged("frames a second"), "level 2: the frame rate should be logged");
    order_checks("level 2", 1);
    common_checks("level 2");
}

/* the API: a picture and a still by hand, the arguments, an abort at every step of the start-up */
static void test_api(void)
{
    phone_reset();
    take(&before);
    struct cam_req bad = { CAM_FRONT, 900, 100, 0, 1, 0, 0 }, bad2 = { 2, 64, 48, 0, 1, 0, 0 };
    CHECK(cam_open(&bad) == CAM_EARG && cam_open(&bad2) == CAM_EARG, "api: bad requests must be refused");
    CHECK(F.wr_count == 0, "api: a refused open wrote");
    struct cam_req q = { CAM_FRONT, 160, 120, CAM_ROT0, 0, 500, 0x20 };
    CHECK(cam_open(&q) == 0 && cam_open(&q) == CAM_ESTATE, "api: open, and a second open refused");
    struct cam_pic p;
    unsigned seq = 0;
    for (unsigned i = 0; i < 400 && seq < 5; ++i) {
        CHECK(cam_poll() >= 0, "api: poll failed: %s", cam_why());
        F.now += 5000;
        tick();
        if (cam_frame(&p))
            seq = p.seq;
    }
    CHECK(seq >= 5 && cam_state() == CAM_ST_RUNNING, "api: %u pictures, state %d (%s)", seq, cam_state(), cam_why());
    if (seq) {
        /* the white bar is the first eighth of the picture */
        uint32_t px = p.px[60 * p.stride + 5];
        CHECK(((px >> 16) & 0xFF) > 240 && ((px >> 8) & 0xFF) > 240 && (px & 0xFF) > 240, "api: the white bar is %06x",
              px & 0xFFFFFF);
    }
    CHECK(cam_still() == 0 && cam_still() == CAM_ESTATE, "api: one still at a time");
    struct cam_shot s;
    for (unsigned i = 0; i < 400 && !cam_still_get(&s); ++i) {
        (void)cam_poll();
        F.now += 5000;
        tick();
    }
    CHECK(cam_still_get(&s) && s.w == 816 && s.h == 612 && s.bmp_len == cam_isp_bmp_size(816, 612) && s.bmp[0] == 'B',
          "api: the still");
    unsigned fr = F.frames;
    for (unsigned i = 0; i < 100; ++i) {
        (void)cam_poll();
        F.now += 5000;
        tick();
    }
    CHECK(F.frames > fr + 10, "api: the stream goes on after the still (%u frames)", F.frames - fr);
    cam_close();
    CHECK(cam_state() == CAM_ST_OFF, "api: closed");
    common_checks("api");

    /* an abort after every number of steps of the start-up and the first frames */
    for (unsigned n = 0; n < 40; ++n) {
        phone_reset();
        take(&before);
        CHECK(cam_open(&q) == 0, "abort %u: open", n);
        for (unsigned i = 0; i < n; ++i) {
            (void)cam_poll();
            F.now += 3000;
            tick();
        }
        cam_abort();
        char t[32];
        snprintf(t, sizeof(t), "abort after %u", n);
        CHECK(cam_state() == CAM_ST_OFF, "%s: state %d", t, cam_state());
        order_checks(t, 0);
        common_checks(t);
    }
    drv_cam_abort();
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    F.ram = malloc(CAM_MEM_SIZE);
    bars = malloc(1632u * 1224u * 2u);
    {
        unsigned lvl[8][3];
        for (unsigned b = 0; b < 8; ++b)
            for (unsigned c = 0; c < 3; ++c)
                lvl[b][c] = bar_rgb[b][c] * 1000u;
        make_bars(bars, 1632, 1224, lvl);
    }
    hal_install();
    test_isp();
    printf("cam-test: the image pipeline: %d checks, %d failed\n", checks, fails);
    test_level0();
    test_level1_good();
    test_level1_faults();
    test_level2();
    test_api();
    printf("cam-test: %d checks, %d failed\n", checks, fails);
    free(F.ram);
    free(bars);
    return fails ? 1 : 0;
}
