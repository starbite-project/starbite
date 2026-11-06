/* SPDX-License-Identifier: GPL-2.0-only
 * The CMGP ADC (samsung,exynos-adc-v3 at 0x14230000) and the thermistors on it, polled; and the
 * sample filter every sensor reading in libsb goes through (sb_sample_filter).
 *
 * Registers (vendor drivers/iio/adc/exynos_adc.c, ADC_V2 / ADC_V3): CON1 +0x00 (bit 2 soft
 * reset, bit 1 soft non-reset, bit 0 start), CON2 +0x04 ([6:4] C_TIME, [3:0] channel), V3 DAT
 * +0x08 ([11:0] value; bit 31 a flag the driver defines but does not use), INT_EN +0x10, INT_ST
 * +0x14 (write 1 clears), VER +0x20. The kernel's v3 sequence: CON1 = soft reset, CON1 =
 * non-reset, CON2 = C_TIME(6), INT_EN = 1, then CON2 channel, CON1 |= start, wait for the
 * interrupt, and only then (in the ISR) DAT & 0xfff, INT_ST = 1; afterwards C_TIME cleared and
 * INT_EN = 0.
 *
 * v2.1 fix (on the phone every thermistor read raw 0 now and then: -20 C on the PA table, 90 C on
 * the others): v2.0 polled "read DAT, then test INT_ST" and returned the DAT it had read BEFORE it
 * saw INT_ST set, so a conversion that finished between the two reads gave the register's value
 * from before the conversion, 0 after the soft reset. It also accepted DAT bit 31 as "done", which
 * the vendor driver never relies on (if that bit means "busy", it returns 0 too). Now INT_ST bit 0
 * is polled alone (2 ms at most) and DAT is read only after it is seen set, as the kernel's ISR
 * does. If INT_ST never comes, DAT is read once after the 2 ms (the conversion is long over by
 * then) and used only if its bit 31 is set; that path is counted ("completion not signalled") so
 * it shows on the sensor page. On top of that the thermistors take the median of 3 conversions and
 * reject raw 0 and full scale (4095), counting every rejected sample, so a sensor that really reads
 * 0 (shorted) or 4095 (open) still shows: as "no valid sample" with its raw value.
 *
 * Clock: CMU_CMGP gate ADC_CMGP PCLK_S0 0x14202018 (bit 20 manual, bit 21 on, as the other CMGP
 * gates). Engineer OS never opens it: a closed gate is reported and the ADC is not touched. S-Boot
 * reads the battery thermistor through this ADC at every boot (temp_adc in its log), so it should
 * be on. Linux shares the ADC with the sensor hub (CHUB) through a handshake in SYSREG_CMGP; CHUB
 * does not run before an OS starts it, so there is no one to share with here.
 * The thermistor tables are the vendor DT's (device/dtb/starlte.dts sec_thermistor@0/2/10 and the
 * battery node's temp_table_adc / data; channels: battery 4, charger 3, USB 1, AP 0, Wi-Fi 2,
 * PA 10).
 */
#include "sb.h"

#define ADC 0x14230000UL
#define CON1 0x00
#define CON2 0x04
#define DAT 0x08
#define INT_EN 0x10
#define INT_ST 0x14

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

/* ---- the sample filter --------------------------------------------------------------------------- */

int sb_sample_filter(int (*get)(void *ctx), void *ctx, int lo, int hi, unsigned want, unsigned max,
                     unsigned *rejected, int *last_bad)
{
    int acc[8];
    unsigned n = 0, rej = 0;
    if (want < 1)
        want = 1;
    if (want > 8)
        want = 8;
    for (unsigned i = 0; i < max && n < want; ++i) {
        int v = get(ctx);
        if (v == SB_NO_SAMPLE || v < lo || v > hi) {
            ++rej;
            if (last_bad)
                *last_bad = v;
            continue;
        }
        unsigned k = n++;                       /* insertion sort: acc stays ascending */
        while (k && acc[k - 1] > v) {
            acc[k] = acc[k - 1];
            --k;
        }
        acc[k] = v;
    }
    if (rejected)
        *rejected = rej;
    if (!n)
        return SB_NO_SAMPLE;
    if (n & 1U)
        return acc[n / 2];
    long s = (long)acc[n / 2 - 1] + acc[n / 2];  /* even: the mean of the middle two, rounded down */
    return (int)(s >= 0 ? s / 2 : (s - 1) / 2);
}

/* ---- one conversion ------------------------------------------------------------------------------ */

#ifdef SB_HOST
int host_adc(unsigned ch);              /* the host preview's fake converter (starbite/host/host.c) */
#endif

static struct sb_adc_stat adc_st[12];

const struct sb_adc_stat *sb_adc_stats(unsigned ch)
{
    return ch < 12 ? &adc_st[ch] : 0;
}

int sb_adc_read(unsigned ch, const char **why)
{
    const char *w = 0;
    int v = -1;
    if (ch < 12)
        ++adc_st[ch].conversions;
#ifdef SB_HOST
    if (why)
        *why = 0;
    return host_adc(ch);
#endif
    if (ch > 11) {
        w = "no such channel";
    } else if ((rd(0x14202018UL) & 0x00300000U) == 0x00100000U) {
        w = "ADC clock gated (CMU_CMGP 0x14202018): not touched";
    } else {
        wr(ADC + CON1, 1U << 2);
        wr(ADC + CON1, 1U << 1);
        wr(ADC + CON2, 6U << 4);
        wr(ADC + INT_ST, 1);
        wr(ADC + INT_EN, 1);
        wr(ADC + CON2, (rd(ADC + CON2) & ~0xfU) | ch);
        __asm__ volatile("dsb sy" : : : "memory");
        wr(ADC + CON1, rd(ADC + CON1) | 1U);
        uint64_t t0 = sb_ticks(), lim = sb_freq() / 500U;         /* 2 ms */
        int done = 0;
        for (;;) {
            if (rd(ADC + INT_ST) & 1U) {
                done = 1;
                break;
            }
            if (sb_ticks() - t0 > lim)
                break;
        }
        uint32_t d = rd(ADC + DAT);             /* read after INT_ST: the finished conversion */
        if (done) {
            v = (int)(d & 0xfffU);
        } else if (d & 0x80000000U) {
            v = (int)(d & 0xfffU);
            ++adc_st[ch].flag_only;
        } else {
            w = "conversion timed out (no INT_ST, no DAT flag in 2 ms)";
            ++adc_st[ch].timeouts;
        }
        wr(ADC + INT_ST, 1);
        wr(ADC + CON2, rd(ADC + CON2) & ~(7U << 4));
        wr(ADC + INT_EN, 0);
    }
    if (why)
        *why = w;
    return v;
}

struct adc_ctx {
    unsigned ch;
    const char *why;
};

static int adc_get(void *v)
{
    struct adc_ctx *c = v;
    const char *w = 0;
    int r = sb_adc_read(c->ch, &w);
    if (r < 0) {
        if (w)
            c->why = w;
        return SB_NO_SAMPLE;
    }
    return r;
}

/* A filtered conversion: the median of 3 accepted conversions out of 6 at most; raw 0 and full
 * scale (4095: a shorted / open input or a conversion read too early) are rejected and counted.
 * -1 when none was accepted (*why says why). */
int sb_adc_filtered(unsigned ch, const char **why, unsigned *rejected, int *last_bad)
{
    struct adc_ctx c = { ch, 0 };
    unsigned rej = 0;
    int bad = SB_NO_SAMPLE;
    int v = sb_sample_filter(adc_get, &c, 1, 4094, 3, 6, &rej, &bad);
    if (ch < 12) {
        adc_st[ch].rejected += rej;
        if (rej && bad != SB_NO_SAMPLE)
            adc_st[ch].last_bad = bad;
        ++adc_st[ch].readings;
    }
    if (rejected)
        *rejected = rej;
    if (last_bad)
        *last_bad = bad == SB_NO_SAMPLE ? -1 : bad;
    if (why)
        *why = v == SB_NO_SAMPLE ? (c.why ? c.why : "every sample was 0 or full scale") : 0;
    return v == SB_NO_SAMPLE ? -1 : v;
}

/* Tables with the ADC value rising (temperature falling or rising): interpolate between the two
 * neighbours; clamp outside. */
int sb_therm_c10(int adc, const int *ta, const int *tc, unsigned n)
{
    if (!n)
        return 0;
    if (adc <= ta[0])
        return tc[0];
    if (adc >= ta[n - 1])
        return tc[n - 1];
    for (unsigned i = 1; i < n; ++i)
        if (adc <= ta[i])
            return tc[i - 1] + (tc[i] - tc[i - 1]) * (adc - ta[i - 1]) / (ta[i] - ta[i - 1]);
    return tc[n - 1];
}

/* sec_thermistor@0 (AP) and @2 (Wi-Fi) */
static const int ap_adc[23] = { 0x113, 0x144, 0x17c, 0x1c2, 0x210, 0x26b, 0x2d9, 0x358, 0x3eb, 0x489, 0x541, 0x609,
                                0x6e6, 0x7bc, 0x8ae, 0x999, 0xa7d, 0xb55, 0xc1e, 0xccf, 0xd62, 0xddf, 0xe4b };
/* the battery node's temp / usb / chg tables */
static const int bat_adc[23] = { 0x113, 0x147, 0x181, 0x1c3, 0x217, 0x272, 0x2e1, 0x360, 0x3f5, 0x4ba, 0x551, 0x612,
                                 0x6e9, 0x7d7, 0x8bf, 0x9ac, 0xa8e, 0xb6c, 0xc2c, 0xcde, 0xd76, 0xdec, 0xe5c };
static const int std_c10[23] = { 900, 850, 800, 750, 700, 650, 600, 550, 500, 450, 400, 350,
                                 300, 250, 200, 150, 100, 50, 0, -50, -100, -150, -200 };
/* sec_thermistor@10 (PA): -20.0 C at the first value, then 1 C per entry up to 90.0 C */
static const int pa_adc[111] = {
    0x147, 0x15b, 0x16f, 0x184, 0x19a, 0x1b1, 0x1c8, 0x1e2, 0x1fb, 0x216, 0x232, 0x24f, 0x26d, 0x28b, 0x2ac,
    0x2cd, 0x2ee, 0x312, 0x335, 0x35a, 0x380, 0x3a7, 0x3cf, 0x3f8, 0x421, 0x44c, 0x477, 0x4a3, 0x4cf, 0x4fd,
    0x52b, 0x559, 0x588, 0x5b7, 0x5e7, 0x617, 0x648, 0x679, 0x6aa, 0x6db, 0x70c, 0x73d, 0x76e, 0x79f, 0x7d0,
    0x800, 0x830, 0x860, 0x890, 0x8bf, 0x8ed, 0x91c, 0x94a, 0x975, 0x9a3, 0x9ce, 0x9f9, 0xa24, 0xa4d, 0xa76,
    0xa9f, 0xac6, 0xaed, 0xb11, 0xb38, 0xb5a, 0xb7e, 0xba0, 0xbc3, 0xbe2, 0xc02, 0xc22, 0xc40, 0xc5d, 0xc7b,
    0xc96, 0xcb1, 0xccd, 0xce6, 0xcff, 0xd18, 0xd2f, 0xd45, 0xd5c, 0xd71, 0xd85, 0xd9a, 0xdae, 0xdc0, 0xdd2,
    0xde5, 0xdf5, 0xe07, 0xe17, 0xe24, 0xe34, 0xe42, 0xe52, 0xe60, 0xe6b, 0xe79, 0xe84, 0xe92, 0xe9d, 0xea8,
    0xeb1, 0xebd, 0xec6, 0xed1, 0xeda, 0xee4,
};
static int pa_c10[111];

int sb_therm_convert(unsigned table, int adc)
{
    if (!pa_c10[110])
        for (unsigned i = 0; i < 111; ++i)
            pa_c10[i] = -200 + 10 * (int)i;
    return table == 2 ? sb_therm_c10(adc, pa_adc, pa_c10, 111)
                      : sb_therm_c10(adc, table ? bat_adc : ap_adc, std_c10, 23);
}

/* The raw range a table covers (outside it the temperature is clamped to -20 / 90 C). */
int sb_therm_raw_min(unsigned table)
{
    return table == 2 ? pa_adc[0] : table ? bat_adc[0] : ap_adc[0];
}

int sb_therm_raw_max(unsigned table)
{
    return table == 2 ? pa_adc[110] : table ? bat_adc[22] : ap_adc[22];
}

int sb_therms_read(struct sb_therm *t, unsigned max, const char **why)
{
    static const struct {
        const char *name;
        unsigned ch, table;
    } list[] = {
        { "Battery (thermistor)", 4, 1 }, { "AP (board, near the SoC)", 0, 0 }, { "USB connector", 1, 1 },
        { "Wi-Fi", 2, 0 }, { "PA (modem amplifier)", 10, 2 }, { "Charger", 3, 1 },
    };
    unsigned n = 0;
    const char *w = 0;
    for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]) && n < max; ++i) {
        const char *e = 0;
        struct sb_therm *s = &t[n++];
        s->name = list[i].name;
        s->ch = list[i].ch;
        s->raw = sb_adc_filtered(list[i].ch, &e, &s->rej, &s->bad);
        s->ok = s->raw >= 0;
        s->c10 = 0;
        s->clamped = 0;
        s->why = s->ok ? 0 : e;
        if (!s->ok) {
            if (!w)
                w = e;
            if (e && e[0] == 'A')               /* clock gated: nothing else can be read either */
                break;
            continue;
        }
        s->c10 = sb_therm_convert(list[i].table, s->raw);
        s->clamped = s->raw < sb_therm_raw_min(list[i].table) || s->raw > sb_therm_raw_max(list[i].table);
    }
    if (why)
        *why = w;
    return (n && !t[0].ok && w && w[0] == 'A') ? -1 : (int)n;
}
