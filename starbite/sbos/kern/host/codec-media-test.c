/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/codec_media.c (with kern/codec.c under it) against a fake CS47L93: a register file behind the
 * HAL's SPI hook, its reset pin, supplies and clocks, and a fake 3.5 mm jack (no plug, 3-pole headphones, a headset
 * with a mic and buttons, a 4-pole plug with an open mic contact). Every SPI write is watched: the headphone gain cap,
 * the output enable / disable order (muted, mixers and clocks up first; never reset or SYSCLK off while the output is
 * on). Checks: jack detection and its debounces, the button ranges, the headphone ramp order, the gain cap, media
 * capture, calls winning, a bus failure at every transfer leaving the codec in reset, and each probe level.
 * Build and run: starbite/sbos/kern/host/codec-media-test.sh (the Mac's cc). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../codec_media.h"
#include "../drvprobe.h"

static int fails, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static int verbose;
static unsigned long long sim_us;

/* ---- the trace ---- */

enum { E_W32, E_SW, E_SR, E_GPIO, E_PR, E_PW };
struct ev {
    int k;
    unsigned a, b;
};
#define MAXEV 400000
static struct ev evs[MAXEV];
static int nev;
static void ev(int k, unsigned a, unsigned b)
{
    if (nev < MAXEV)
        evs[nev++] = (struct ev){ k, a, b };
}

static char logbuf[1 << 20];
static size_t loglen;
static void hlog(const char *s)
{
    size_t n = strlen(s);
    if (loglen + n + 2 < sizeof logbuf) {
        memcpy(logbuf + loglen, s, n);
        loglen += n;
        logbuf[loglen++] = '\n';
        logbuf[loglen] = 0;
    }
    if (verbose)
        printf("    | %s\n", s);
}
static int logged(const char *s)
{
    return strstr(logbuf, s) != 0;
}
static void log_reset(void)
{
    loglen = 0;
    logbuf[0] = 0;
}

/* ---- MMIO ---- */

#define NMM 256
static struct { uintptr_t a; uint32_t v; } mm[NMM];
static int nmm;
static uint32_t *mmref(uintptr_t a)
{
    for (int i = 0; i < nmm; ++i)
        if (mm[i].a == a)
            return &mm[i].v;
    if (nmm == NMM) {
        printf("mmio map full\n");
        exit(2);
    }
    mm[nmm].a = a, mm[nmm].v = 0;
    return &mm[nmm++].v;
}

#define USI01_MODE 0x1041100CUL
#define GPP0 0x10430000UL
#define GPG1 0x104300A0UL
#define CLKOUT0 0x14060A00UL
#define CMU0 0x10400000UL
#define CLKOUT_BOOT 0x00003F01U

static uint32_t f_rd32(uintptr_t a)
{
    if (a == CMU0 + 0x1808)
        return *mmref(a) & ~(1U << 16);
    return *mmref(a);
}
static void f_wr32(uintptr_t a, uint32_t v)
{
    ev(E_W32, (unsigned)a, v);
    *mmref(a) = v;
}

/* ---- the fake codec and jack ---- */

static uint16_t c16[0x2000];
static int reset_high, ldo34_on = 1, fake_id = 0x6371, spi_dead_after = -1, spi_xfers, dead_hit;
static unsigned long long release_us;
static int fll_upd_seen, micd_ran;
static int plug;                    /* 0 none, 1 3-pole, 2 headset, 3 4-pole with an open mic contact */
static int btn_ohms = -1;           /* a headset button held: its resistance */
static unsigned mic_ohms = 2200;
static int cap_viol, order_viol;
static unsigned long long out_on_us, unmute_us;

static const unsigned lv[65] = {
    3, 6, 8, 11, 13, 16, 18, 21, 23, 26, 28, 31, 34, 36, 39, 41, 44, 46, 49, 52, 54, 57, 60, 62, 65, 67, 70, 73, 75, 78,
    81, 83, 89, 94, 100, 105, 111, 116, 122, 127, 139, 150, 161, 173, 186, 196, 209, 220, 245, 270, 295, 321, 348, 375,
    402, 430, 489, 550, 614, 681, 752, 903, 1071, 1257, 30000,
};

static void regs_reset(void)
{
    memset(c16, 0, sizeof c16);
    c16[0x321] = 0x0180, c16[0x329] = 0x0180, c16[0x311] = 0x0180;
    c16[0x419] = 0x0180, c16[0x41D] = 0x0180;
    c16[0x500] = 0x000C;
    fll_upd_seen = micd_ran = 0;
}

static int alive(void)
{
    return reset_high && ldo34_on && (*mmref(USI01_MODE) & 7) == 2 && (*mmref(GPP0) >> 16) == 0x2222;
}

static int fll_locked(void)
{
    uint32_t ck = *mmref(CLKOUT0);
    return (c16[0x171] & 1) && !(c16[0x171] & 4) && fll_upd_seen && ((ck >> 8) & 0x3F) == 1 && !(ck & 1);
}

static unsigned now_ohms(void)
{
    if (plug == 1)
        return 0;
    if (plug == 3)
        return 100000;
    if (plug == 2)
        return btn_ohms >= 0 ? (unsigned)btn_ohms : mic_ohms;
    return 100000;
}

static uint16_t c16_read(unsigned r)
{
    switch (r) {
    case 0x0000:
        return (uint16_t)fake_id;
    case 0x0001:
        return 0x0004;
    case 0x1880:
        return sim_us - release_us >= 3000 ? 0x0080 : 0;
    case 0x1881:
        return fll_locked() ? 0x0100 : 0;
    case 0x0301:
        return c16[0x300];
    case 0x1886:                    /* the clamp (mode 8, JD1+JD2 on) rises with no plug; unconfigured reads 0 */
        if ((c16[0x2D3] & 3) == 3 && (c16[0x2C6] & 0xF) == 8)
            return plug ? 0 : 0x0010;
        return 0;
    case 0x02AB: {
        int ok = !(c16[0x2A3] & 1) && micd_ran;
        micd_ran = 0;
        if (!ok)
            return 0x37;            /* read while running / no new conversion: a stale value (a "button") */
        int powered = (c16[0x200] & 3) == 1 && (c16[0x219] & 1) && (c16[0x21E] & 1) && (c16[0x2A2] & 0x8000) &&
                      ((c16[0x2A3] >> 4) & 0xF) == 4 && (c16[0x2A2] & 0xF7) == 0x01;
        if (!powered)
            return 0;               /* no bias: the mic pin reads as a short */
        unsigned o = now_ohms();
        if (o > 30000)
            return 0x7F;
        unsigned i = 0;
        while (i < 64 && lv[i] < o)
            ++i;
        return (uint16_t)i;
    }
    case 0x188C:
        return c16[0x400] & 0xC;
    case 0x188D:
        return (uint16_t)(~c16[0x400] & 0xC);
    }
    return r < 0x2000 ? c16[r] : 0;
}

static void c16_write(unsigned r, uint16_t v)
{
    if (r >= 0x2000)
        return;
    if (r == 0x0419 || r == 0x041D) {
        if ((v & 0xFF) > CODEC_HP_VOL_MAX) {
            ++cap_viol;
            printf("  CAP: %04x <- %04x\n", r, v);
        }
        if ((c16[r] & 0x100) && !(v & 0x100))
            unmute_us = sim_us;
        v &= (uint16_t)~0x0200;
    }
    if ((r == 0x0691 || r == 0x0699) && (v & 0xFE) > 0x80) {
        ++cap_viol;
        printf("  CAP: mixer %04x <- %04x\n", r, v);
    }
    if (r == 0x0400) {
        unsigned was = c16[0x400] & 0xC, now = v & 0xC;
        int muted = (c16[0x419] & 0x100) && (c16[0x41D] & 0x100);
        if (now & ~was) {
            out_on_us = sim_us;
            if (!muted || c16[0x690] != 0x20 || c16[0x698] != 0x21 || !(c16[0x101] & 0x40) || !fll_locked() ||
                (c16[0x51A] & 3) != 3) {
                ++order_viol;
                printf("  ORDER: HPOUT2 enabled with muted %d, mixers %x/%x, SYSCLK %x, FLL %d, AIF1RX %x\n", muted,
                       c16[0x690], c16[0x698], c16[0x101], fll_locked(), c16[0x51A]);
            }
        }
        if ((was & ~now) && !muted) {
            ++order_viol;
            printf("  ORDER: HPOUT2 disabled unmuted\n");
        }
    }
    if (r == 0x0101 && !(v & 0x40) && (c16[0x400] & 0xC)) {
        ++order_viol;
        printf("  ORDER: SYSCLK off with HPOUT2 on\n");
    }
    if (r == 0x0171 && !(v & 1) && (c16[0x400] & 0xC)) {
        ++order_viol;
        printf("  ORDER: FLL1 off with HPOUT2 on\n");
    }
    if (r == 0x0321 || r == 0x0329 || r == 0x0311)
        v &= (uint16_t)~0x0200;
    if (r == 0x0172 && (v & 0x8000))
        fll_upd_seen = 1;
    if (r == 0x02A3 && (v & 1))
        micd_ran = 1;
    c16[r] = v;
}

static int f_spi(uintptr_t bus, const uint8_t *tx, uint8_t *rx, unsigned n)
{
    (void)bus;
    ++spi_xfers;
    if (spi_dead_after >= 0 && spi_xfers > spi_dead_after) {
        dead_hit = 1;
        return -5;
    }
    if (!alive()) {
        if (rx)
            memset(rx, 0xFF, n);
        return 0;
    }
    unsigned reg = ((unsigned)(tx[0] & 0x7F) << 24) | (unsigned)tx[1] << 16 | (unsigned)tx[2] << 8 | tx[3];
    if (tx[0] & 0x80) {
        uint16_t v = reg < 0x3000 ? c16_read(reg) : 0;
        if (rx) {
            memset(rx, 0, n);
            if (n >= 8)
                rx[6] = (uint8_t)(v >> 8), rx[7] = (uint8_t)v;
        }
        ev(E_SR, reg, v);
        return 0;
    }
    if (n == 8) {
        uint16_t v = (uint16_t)(tx[6] << 8 | tx[7]);
        ev(E_SW, reg, v);
        c16_write(reg, v);
    }
    return 0;
}

/* ---- GPIO, PMIC, delay ---- */

static uint8_t pmic[256];
static int pmic_fail;
static int f_gpio_set(uintptr_t bank, unsigned pin, int level)
{
    ev(E_GPIO, (unsigned)bank, (unsigned)level);
    if (bank == GPG1 && pin == 0) {
        if (!level && reset_high && (c16[0x400] & 0xC) && !dead_hit) {  /* a dead bus leaves only the reset */
            ++order_viol;
            printf("  ORDER: reset asserted with HPOUT2 on\n");
        }
        if (level && !reset_high) {
            release_us = sim_us;
            regs_reset();
        }
        reset_high = level;
        *mmref(GPG1) = (*mmref(GPG1) & ~0xFU) | 1U;
        *mmref(GPG1 + 4) = (unsigned)level;
    }
    return 0;
}
static int f_gpio_get(uintptr_t bank, unsigned pin)
{
    return bank == GPG1 && pin == 0 ? reset_high : 0;
}
static int f_pmic_read(unsigned reg, uint8_t *v)
{
    ev(E_PR, reg, 0);
    if (pmic_fail)
        return -5;
    *v = pmic[reg & 255];
    return 0;
}
static int f_pmic_write(unsigned reg, uint8_t v)
{
    ev(E_PW, reg, v);
    pmic[reg & 255] = v;
    if (reg == 0x6B)
        ldo34_on = (v & 0xC0) == 0xC0;
    return 0;
}
static void f_delay(unsigned us)
{
    sim_us += us;
}

static struct audio_hal hal = { f_rd32, f_wr32, 0, f_spi, f_gpio_set, f_gpio_get, f_pmic_read, f_pmic_write, f_delay,
                                hlog };

static void world(int plug_kind)
{
    memset(mm, 0, sizeof mm);
    nmm = 0;
    nev = 0;
    log_reset();
    sim_us = 0;
    cap_viol = order_viol = 0;
    spi_dead_after = -1, spi_xfers = 0, dead_hit = 0;
    fake_id = 0x6371;
    plug = plug_kind, btn_ohms = -1, mic_ohms = 2200;
    out_on_us = unmute_us = 0;
    *mmref(CMU0 + 0x2010) = *mmref(CMU0 + 0x205C) = *mmref(CMU0 + 0x20BC) = *mmref(CMU0 + 0x20C0) = 0x00200000;
    *mmref(CMU0 + 0x0120) = 0x10;
    *mmref(CMU0 + 0x1808) = 0;
    *mmref(GPG1) = 0x1, *mmref(GPG1 + 4) = 0, *mmref(GPG1 + 8) = 0;
    *mmref(CLKOUT0) = CLKOUT_BOOT;
    reset_high = 0;
    memset(pmic, 0, sizeof pmic);
    pmic[0x6A] = 0xE8, pmic[0x6B] = 0xE8;
    ldo34_on = 1, pmic_fail = 0;
    regs_reset();
    codec_media_hal_set(&hal);
    while (codec_jack_event())
        ;
}

static int poll1(void)
{
    sim_us += CODEC_JACK_POLL_MS * 1000ULL;
    return codec_jack_poll();
}
static int polls(int n)
{
    int s = 0;
    for (int i = 0; i < n; ++i)
        s = poll1();
    return s;
}
static int first_ev(int from, int k, unsigned a, long b)
{
    for (int i = from; i < nev; ++i)
        if (evs[i].k == k && evs[i].a == a && (b < 0 || evs[i].b == (unsigned)b))
            return i;
    return -1;
}
static int count_ev(int k)
{
    int n = 0;
    for (int i = 0; i < nev; ++i)
        n += evs[i].k == k;
    return n;
}
static int wrote_bits(unsigned reg, unsigned bits)
{
    for (int i = 0; i < nev; ++i)
        if (evs[i].k == E_SW && evs[i].a == reg && (evs[i].b & bits))
            return 1;
    return 0;
}

/* ---- the tests ---- */

static void t_jack_none(void)
{
    printf("jack: no plug (the set-up, the clamp read only after it, MICD never started)\n");
    world(0);
    int s = 0, any = 0;
    for (int i = 0; i < 40; ++i)
        any |= (s = poll1());
    CHECK(!any && s == 0, "always none");
    CHECK(reset_high, "the codec kept out of reset for the jack");
    CHECK(c16[0x2D3] == 3 && (c16[0x2C6] & 0xF) == 8 && (c16[0x1A06] & 0x15) == 0x15, "JD1/JD2, clamp 8, debounce");
    CHECK((c16[0x219] & 0x81EA) == 0x81A8 && (c16[0x21E] & 2), "MICBIAS2 2.8 V ext cap soft start, 2A discharge");
    CHECK((c16[0x2A3] & 0xFFF2) == 0x1742 && (c16[0x2A2] & 0x80F7) == 0x8001, "MICD config %04x %04x", c16[0x2A3],
          c16[0x2A2]);
    CHECK((c16[0x2A9] & 0x3F3F) == 0x252B && (c16[0x2A8] & 0x3F3F) == 0x333C && (c16[0x2A7] & 0x3F00) == 0x3F00 &&
              (c16[0x2A4] & 0xFF) == 0x1F, "button levels %04x %04x %04x", c16[0x2A9], c16[0x2A8], c16[0x2A7]);
    CHECK((c16[0x41A] & 7) == 1 && (c16[0x299] & 7) == 1, "HP2_GND_SEL / HPD_GND_SEL 1");
    int jd = first_ev(0, E_SW, 0x2D3, 3), rd = first_ev(0, E_SR, 0x1886, -1);
    CHECK(jd >= 0 && rd > jd, "the jack status is read only after JD is on");
    CHECK(!wrote_bits(0x2A3, 1) && !wrote_bits(0x400, 0xC), "no MICD, no output");
    CHECK(count_ev(E_PW) == 0, "no PMIC write");
}

static void t_jack_types(void)
{
    printf("jack: headphones (3-pole), headset, open mic contact, undecided -> headphones after 1 s\n");
    world(1);
    int s = 0, at = -1;
    for (int i = 0; i < 30 && at < 0; ++i)
        if ((s = poll1()) != 0)
            at = i + 1;
    CHECK(s == CODEC_JACK_HEADPHONES, "headphones: %d", s);
    CHECK(at >= 9 && at <= 12, "decided at poll %d (150 ms in + 300 ms detect debounce + 2 readings)", at);
    CHECK(!(c16[0x2A3] & 1) && !(c16[0x219] & 1) && !(c16[0x21E] & 1) && !(c16[0x200] & 1) && (c16[0x2C6] & 0x10),
          "MICD stopped: bias, MICVDD off, clamp override on");
    CHECK(polls(20) == 1, "stays headphones");

    world(2);
    s = polls(14);
    CHECK(s == CODEC_JACK_HEADSET, "headset: %d", s);
    CHECK((c16[0x2A3] & 1) && (c16[0x219] & 1) && (c16[0x21E] & 1) && (c16[0x200] & 3) == 1 && !(c16[0x2C6] & 0x10),
          "MICD running for the buttons");
    CHECK(codec_jack_ohms() == 30000, "MICDET %u ohms", codec_jack_ohms());

    world(3);
    CHECK(polls(14) == CODEC_JACK_HEADSET, "open mic contact: headset (micd-open-circuit-declare)");

    world(2);
    int alt = 0;
    for (int i = 0; i < 40; ++i) {
        mic_ohms = (alt ^= 1) ? 2200 : 50;
        s = poll1();
        if (s)
            break;
    }
    CHECK(s == CODEC_JACK_HEADPHONES, "undecided readings: headphones after the 1 s timeout (%d)", s);
}

static void t_debounce(void)
{
    printf("jack: debounce (a bouncing plug, one-poll glitches, buttons)\n");
    world(0);
    polls(2);
    int any = 0;
    for (int i = 0; i < 30; ++i) {
        plug = (i % 2) ? 2 : 0;
        any |= poll1();
    }
    CHECK(!any && !wrote_bits(0x2A3, 1), "a plug bouncing every poll: nothing reported, MICD not started");
    plug = 2;
    CHECK(polls(14) == 2, "then steady: headset");
    plug = 0;
    CHECK(poll1() == 2, "one poll out: still a headset");
    plug = 2;
    CHECK(polls(3) == 2 && codec_jack_event() == 0, "glitch: nothing");

    static const struct { int ohms, btn; } b[] = {
        { 0, CODEC_BTN_PLAY }, { 116, CODEC_BTN_PLAY }, { 150, CODEC_BTN_VOICE }, { 250, CODEC_BTN_VOLUP },
        { 321, CODEC_BTN_VOLUP }, { 500, CODEC_BTN_VOLDOWN }, { 1000, CODEC_BTN_OTHER },
    };
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; ++i) {
        btn_ohms = b[i].ohms;
        polls(3);
        int e1 = codec_jack_event();
        btn_ohms = -1;
        polls(3);
        int e2 = codec_jack_event();
        CHECK(e1 == b[i].btn && e2 == -b[i].btn, "%d ohms: events %d %d (want %d)", b[i].ohms, e1, e2, b[i].btn);
    }
    btn_ohms = 0;
    poll1();
    btn_ohms = -1;
    polls(3);
    CHECK(codec_jack_event() == 0, "a one-poll button blip: nothing");
    btn_ohms = 250;
    polls(3);
    CHECK(codec_jack_event() == CODEC_BTN_VOLUP, "held");
    plug = 0;
    int s = polls(2);
    CHECK(s == 0 && codec_jack_event() == -CODEC_BTN_VOLUP, "pulled out with a button held: released, none");
    CHECK(!(c16[0x2A3] & 1) && !(c16[0x219] & 1), "MICD and MICBIAS2 off");
    btn_ohms = -1;
}

static void t_hp(void)
{
    printf("headphones: refused without a plug; the enable order, the ramp, the gain cap, off, auto-off on unplug\n");
    world(0);
    polls(5);
    CHECK(codec_hp(1, 10) == AUD_EPERM && !wrote_bits(0x400, 0xC), "no plug: refused (%s)", codec_media_why());
    for (int i = 1; i < CODEC_HP_LEVELS; ++i)
        CHECK(codec_hp_vol[i] > codec_hp_vol[i - 1] && codec_hp_vol[i] <= CODEC_HP_VOL_MAX, "level table %d", i);
    CHECK(codec_hp_vol[CODEC_HP_LEVELS - 1] == 107, "level 15 = One UI's 107");

    world(2);
    polls(14);
    int rc = codec_hp(1, 15);
    CHECK(rc == 0, "on: rc %d (%s)", rc, codec_media_why());
    CHECK((c16[0x400] & 0xC) == 0xC && (c16[0x419] & 0x1FF) == 107 && (c16[0x41D] & 0x1FF) == 107, "on at 107");
    CHECK((c16[0x409] & 0x77) == 0x22, "ramps 1 ms/6 dB");
    CHECK(c16[0x690] == 0x20 && c16[0x698] == 0x21 && (c16[0x691] & 0xFE) == 0x80 && (c16[0x699] & 0xFE) == 0x80,
          "AIF1RX1/2 -> OUT2L/R at 0 dB");
    CHECK(unmute_us >= out_on_us + 12000, "unmuted %llu us after the enable", unmute_us - out_on_us);
    CHECK((*mmref(CLKOUT0) & 0x3F01) == 0x0100 && (c16[0x101] & 0x40), "CLKOUT0 and SYSCLK on");
    CHECK(codec_hp(1, 99) == 0 && (c16[0x419] & 0xFF) == 107, "level 99 clamped to 15");
    CHECK(codec_hp(1, 7) == 0 && (c16[0x419] & 0x1FF) == codec_hp_vol[7], "level 7");
    CHECK(codec_hp(1, -4) == 0 && (c16[0x419] & 0x100) && (c16[0x41D] & 0x100), "level -4: muted, output on");
    CHECK((c16[0x400] & 0xC) == 0xC, "still on");
    CHECK(codec_hp(1, 12) == 0 && !(c16[0x419] & 0x100), "unmuted again");
    CHECK(codec_hp(0, 0) == 0, "off");
    CHECK(!(c16[0x400] & 0xC) && !c16[0x690] && !(c16[0x51A] & 3), "output, AIF1RX and mixers off");
    CHECK(*mmref(CLKOUT0) == CLKOUT_BOOT && !(c16[0x171] & 1) && !(c16[0x101] & 0x40), "clocks off, CLKOUT0 back");
    CHECK(reset_high && polls(2) == 2, "the jack still works");

    CHECK(codec_hp(1, 9) == 0, "on again");
    plug = 0;
    CHECK(polls(2) == 0 && !(c16[0x400] & 0xC) && *mmref(CLKOUT0) == CLKOUT_BOOT, "pulled out: off by itself");
    CHECK(cap_viol == 0 && order_viol == 0, "cap %d order %d", cap_viol, order_viol);
}

static void t_mic(void)
{
    printf("capture: main mic, headset mic (refused without a headset), with the headphones\n");
    world(0);
    polls(5);
    CHECK(codec_mic_media(1, 1) == AUD_EPERM, "headset mic without a headset: refused");
    CHECK(codec_mic_media(1, 2) == AUD_EINVAL, "bad which");
    int rc = codec_mic_media(1, 0);
    CHECK(rc == 0, "main: rc %d (%s)", rc, codec_media_why());
    CHECK((c16[0x300] & 0x20) && c16[0x700] == 0x14 && c16[0x708] == 0x14 && (c16[0x321] & 0x1FF) == 129 &&
              (c16[0x218] & 1) && (c16[0x21C] & 1) && (c16[0x519] & 3) == 3 && (c16[0x200] & 3) == 1,
          "IN3L -> AIF1TX1/2 at 129, MICBIAS1/1A, MICVDD");
    CHECK(codec_mic_media(0, 0) == 0 && !(c16[0x300] & 0x20) && !c16[0x700] && !(c16[0x218] & 1) &&
              !(c16[0x200] & 1) && *mmref(CLKOUT0) == CLKOUT_BOOT, "main off: all off");

    world(2);
    polls(14);
    CHECK(codec_hp(1, 10) == 0, "headphones");
    rc = codec_mic_media(1, 1);
    CHECK(rc == 0, "headset mic: rc %d (%s)", rc, codec_media_why());
    CHECK((c16[0x311] & 0x61FF) == 0x4080 && (c16[0x310] & 0x04FE) == 0x90 && c16[0x700] == 0x10 &&
              c16[0x708] == 0x10 && (c16[0x300] & 2), "IN1BL differential +8 dB, 0 dB digital -> AIF1TX1/2 (%04x %04x)",
          c16[0x311], c16[0x310]);
    CHECK(codec_mic_media(1, 0) == 0 && c16[0x700] == 0x14 && !(c16[0x300] & 2) && (c16[0x300] & 0x20),
          "switched to the main mic");
    CHECK(codec_mic_media(1, 1) == 0 && c16[0x700] == 0x10, "back to the headset mic");
    CHECK(codec_mic_media(0, 0) == 0 && (c16[0x219] & 1) && (c16[0x2A3] & 1) && (c16[0x400] & 0xC),
          "off: MICBIAS2 and MICD stay for the buttons, the headphones stay");
    CHECK(codec_hp(0, 0) == 0 && *mmref(CLKOUT0) == CLKOUT_BOOT, "all off");
    CHECK(cap_viol == 0 && order_viol == 0, "cap %d order %d", cap_viol, order_viol);
}

static void t_call(void)
{
    printf("calls win: headphones and capture let go first; refused during the call; the jack keeps working\n");
    world(2);
    polls(14);
    CHECK(codec_hp(1, 15) == 0 && codec_mic_media(1, 1) == 0, "media on");
    int mark = nev;
    int rc = codec_mic(1);
    CHECK(rc == 0, "the call's mic: rc %d (%s)", rc, codec_why());
    int off = first_ev(mark, E_SW, 0x400, 0), call = first_ev(mark, E_SW, 0x700, 0x14);
    CHECK(off >= 0 && call > off, "the headphones went off (%d) before the call path wrote (%d)", off, call);
    CHECK(codec_call_active() && c16[0x700] == 0x14 && (c16[0x300] & 0xA0) == 0xA0 && !(c16[0x300] & 2),
          "the call's route");
    CHECK(codec_hp(1, 10) == AUD_EBUSY && codec_mic_media(1, 0) == AUD_EBUSY && codec_mic_media(1, 1) == AUD_EBUSY,
          "refused during the call (%s)", codec_media_why());
    btn_ohms = 0;
    polls(3);
    CHECK(codec_jack_event() == CODEC_BTN_PLAY, "a button during the call");
    btn_ohms = -1;
    polls(3);
    CHECK(codec_jack_event() == -CODEC_BTN_PLAY, "released");
    CHECK(codec_mic(0) == 0 && !(c16[0x200] & 1), "the call ends (codec.c switched MICVDD off)");
    CHECK(poll1() == 2 && (c16[0x200] & 3) == 1, "the next poll puts MICVDD back for the buttons");
    CHECK(codec_hp(1, 10) == 0 && (c16[0x400] & 0xC), "headphones again after the call");

    printf("calls win: the call path re-probes (a reset) with the headphones on\n");
    mark = nev;
    CHECK(codec_probe() == 0, "re-probe");
    off = first_ev(mark, E_SW, 0x400, 0);
    int rst = first_ev(mark, E_GPIO, GPG1, 0);
    CHECK(off >= 0 && rst > off, "output off (%d) before the reset (%d)", off, rst);
    CHECK(polls(14) == 2 && (c16[0x2A3] & 1), "the jack set up again after the new power-up: headset");
    CHECK(codec_hp(1, 4) == 0, "headphones again");
    printf("calls win: codec_hal_set (modem's audio_prepare) with the headphones on\n");
    mark = nev;
    codec_hal_set(&hal);
    CHECK(!(c16[0x400] & 0xC), "output off before the HAL change");
    CHECK(polls(14) == 2 && reset_high, "re-detected");
    codec_off();
    CHECK(!reset_high && *mmref(CLKOUT0) == CLKOUT_BOOT, "codec_off: reset");
    CHECK(cap_viol == 0 && order_viol == 0, "cap %d order %d", cap_viol, order_viol);
}

/* a whole session: jack, headphones, capture, a call, buttons */
static void scenario(void)
{
    polls(15);
    codec_hp(1, 12);
    codec_mic_media(1, 1);
    btn_ohms = 300;
    polls(4);
    btn_ohms = -1;
    polls(3);
    codec_mic(1);
    polls(3);
    codec_mic(0);
    codec_hp(1, 5);
    codec_mic_media(1, 0);
    polls(5);
    codec_mic_media(0, 0);
    codec_hp(0, 0);
    plug = 0;
    polls(3);
    codec_media_off();
}

static void t_bus_failures(void)
{
    printf("a bus failure at every transfer of a session: the codec ends in reset, CLKOUT0 back, no cap / order\n");
    world(2);
    scenario();
    int total = spi_xfers, bad = 0;
    CHECK(!reset_high && cap_viol == 0 && order_viol == 0, "clean session ends in reset");
    for (int k = 0; k < total; ++k) {
        world(2);
        spi_dead_after = k;
        polls(15);
        codec_hp(1, 12);
        codec_mic_media(1, 1);
        polls(4);
        codec_mic(1);
        codec_mic(0);
        codec_hp(1, 5);
        polls(3);
        if (!dead_hit)
            continue;
        if (reset_high || *mmref(CLKOUT0) != CLKOUT_BOOT || cap_viol || order_viol) {
            if (bad++ < 5)
                printf("  dead after %d: reset %d clkout %08x cap %d order %d (%s)\n", k, reset_high,
                       *mmref(CLKOUT0), cap_viol, order_viol, codec_media_why());
        }
    }
    CHECK(bad == 0, "%d of %d failure points left the codec on", bad, total);
    printf("  (%d failure points)\n", total);

    world(2);
    spi_dead_after = 0;
    CHECK(polls(3) == 0 && !reset_high, "dead from the start: none, in reset");
    int mark = nev;
    CHECK(polls(50) == 0 && first_ev(mark, E_GPIO, GPG1, 1) < 0, "no retry within 5 s");
    spi_dead_after = -1;
    CHECK(polls(80) == 2, "the bus back: retried after 5 s, headset");
}

static int run_probe(int level, unsigned long long max_ms, void (*script)(unsigned long long ms))
{
    unsigned w = 0;
    unsigned long long t0 = sim_us;
    int n = 0;
    for (;;) {
        if (script)
            script((sim_us - t0) / 1000);
        unsigned long long before = sim_us;
        int rc = drv_codec_probe(level, &w);
        CHECK(sim_us - before <= 100000, "one call took %llu us", sim_us - before);
        if (rc == 1)
            break;
        sim_us += w * 1000ULL;
        if ((sim_us - t0) / 1000 > max_ms || ++n > 100000) {
            printf("  probe did not finish\n");
            return 0;
        }
    }
    return 1;
}

static void t_probe(void)
{
    printf("probe level 0: reads only\n");
    world(2);
    nev = 0;
    CHECK(run_probe(0, 100, 0), "finished");
    CHECK(logged("drvprobe: codec: PASS level 0: reset held (low), LDO33 on, LDO34 on 1200 mV; held in reset"),
          "PASS line:\n%s", logbuf);
    CHECK(count_ev(E_W32) == 0 && count_ev(E_SW) == 0 && count_ev(E_SR) == 0 && count_ev(E_GPIO) == 0 &&
              count_ev(E_PW) == 0, "nothing written or sent");
    world(2);
    CHECK(codec_probe() == 0, "probed by someone else");
    log_reset();
    nev = 0;
    run_probe(0, 100, 0);
    CHECK(logged("out of reset, ID 6371") && count_ev(E_SW) == 0 && count_ev(E_W32) == 0 && count_ev(E_GPIO) == 0,
          "out of reset: the ID read, nothing written");
    world(2);
    pmic_fail = 1;
    run_probe(0, 100, 0);
    CHECK(logged("drvprobe: codec: FAIL the PMIC"), "PMIC read failure: FAIL");

    printf("probe level 1: up, ID, the jack for 2 s, back in reset (headset / no plug / dead bus / wrong ID)\n");
    world(2);
    CHECK(run_probe(1, 6000, 0), "finished");
    CHECK(logged("drvprobe: codec: PASS level 1: ID 6371, revision 4, BOOT_DONE; jack: headset"), "PASS:\n%s",
          logbuf);
    CHECK(!reset_high && !wrote_bits(0x400, 0xC) && first_ev(0, E_W32, (unsigned)CLKOUT0, -1) < 0 &&
              count_ev(E_PW) == 0, "in reset; no output, no clock, no PMIC write");
    world(0);
    run_probe(1, 6000, 0);
    CHECK(logged("jack: no plug") && logged("PASS level 1") && !reset_high, "no plug:\n%s", logbuf);
    world(1);
    run_probe(1, 6000, 0);
    CHECK(logged("jack: headphones (MICDET 3 ohms)") && logged("PASS level 1: ID 6371, revision 4, BOOT_DONE; jack: "
                                                                   "headphones"), "headphones");
    world(2);
    spi_dead_after = 0;
    run_probe(1, 6000, 0);
    CHECK(logged("drvprobe: codec: FAIL the codec did not come up") && !reset_high, "dead bus: FAIL, in reset");
    world(2);
    fake_id = 0x1234;
    run_probe(1, 6000, 0);
    CHECK(logged("drvprobe: codec: FAIL") && !reset_high, "wrong ID: FAIL");
    world(2);
    spi_dead_after = 400;
    run_probe(1, 6000, 0);
    CHECK(logged("drvprobe: codec: FAIL") && !reset_high, "the bus dies during the jack read: FAIL, in reset:\n%s",
          logbuf);

    printf("probe level 1: abort part way\n");
    world(2);
    unsigned w = 0;
    for (int i = 0; i < 12; ++i) {
        drv_codec_probe(1, &w);
        sim_us += w * 1000ULL;
    }
    CHECK(reset_high, "running");
    drv_codec_abort();
    CHECK(!reset_high && !(c16[0x400] & 0xC), "abort: in reset");
    CHECK(run_probe(1, 6000, 0) && logged("PASS level 1"), "a fresh run after the abort");
}

static void l2_script(unsigned long long ms)
{
    plug = ms >= 1000 && ms < 7000 ? 2 : 0;
    btn_ohms = ms >= 3000 && ms < 3400 ? 250 : -1;
}

static void t_probe_l2(void)
{
    printf("probe level 2: 10 s with a person plugging, pressing, unplugging\n");
    world(0);
    CHECK(run_probe(2, 15000, l2_script), "finished");
    CHECK(logged("jack: headset") && logged("button volume up pressed") && logged("button volume up released") &&
              logged("PASS level 2") && logged("jack: none") == 0, "log:\n%s", logbuf);
    CHECK(strstr(logbuf, "button volume up released") < strstr(logbuf, "PASS level 2: ID 6371, revision 4, BOOT_DONE; "
                                                                      "jack: no plug (2 changes, 2 button events)"),
          "summary");
    CHECK(!reset_high, "in reset");
}

static void t_nohal(void)
{
    printf("no HAL\n");
    codec_media_hal_set(0);
    CHECK(codec_jack_poll() == 0 && codec_hp(1, 3) == AUD_ENOSYS && codec_mic_media(1, 0) == AUD_ENOSYS,
          "ENOSYS (%s)", codec_media_why());
    unsigned w;
    CHECK(drv_codec_probe(1, &w) == 1, "the probe ends");
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    t_jack_none();
    t_jack_types();
    t_debounce();
    t_hp();
    t_mic();
    t_call();
    t_bus_failures();
    t_probe();
    t_probe_l2();
    t_nohal();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
