/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS: the CS47L93 codec for media: the 3.5 mm jack (plug type, headset buttons), the headphones (HPOUT2) and
 * microphone capture for apps (codec_media.h has the wiring and the API). codec.c keeps the bus, the reset, the boot,
 * the clocks and the in-call microphone; this file reaches the chip only through codec.c's small media hooks
 * (codec_up, codec_clocks, codec_read, codec_write) and writes only the registers listed in mine[] below.
 *
 * Ported from Samsung's GPL-2.0 kernel for this phone (LineageOS android_kernel_samsung_exynos9810, Cirrus's Madera
 * code): drivers/extcon/extcon-madera.c (the clamp-mode jack detect, madera_jack_present with jd_use_jd2, the MICD
 * set-up and madera_micd_start / _stop, MICD1_ADC_MODE and madera_micd_adc_read for cirrus,micd-software-compare, the
 * madera_micd_levels table, the 1258 ohm microphone threshold, open circuit = headset, the first range with
 * ohms <= max wins, madera_micd_set_level), sound/soc/codecs/madera.c (madera_hp_ev / madera_out_ev: OUTPUT_ENABLES_1,
 * 6 ms per HP output up and 1 ms down on CS47L92/93; madera_in_ev's mute / VU order; madera_inmux for IN1L Mux B; the
 * mixer, digital and PGA scales), cs47l92.c (OUT2 on madera_hp_ev, the mixer sources AIF1RX1 0x20 / AIF1RX2 0x21 /
 * IN1L 0x10 / IN3L 0x14), sound/soc/samsung/star_madera.c ("HPOUT2x Impedance Volume" = DAC_DIGITAL_VOLUME_2x plus the
 * HPDET step), include/linux/mfd/madera/registers.h, and One UI's mixer_paths.xml / mixer_gains.xml (VENDOR):
 * route-headphone, route-headset-mic, "Output Ramp Up" 1ms/6dB, gain-media-headset, gain-recording-mic and
 * gain-recording-headset-mic.
 *
 * Deliberate differences from Android:
 * - Polled every 50 ms instead of the codec's IRQ (debounces count polls: plug in 3 polls, out 2, buttons 2).
 * - No HPDET impedance measurement: the headphone gain is capped at One UI's base 107 (its HPDET offset only ever
 *   raises it), and short-circuit / moisture detection are not done.
 * - The headset mic is not muted while a button is held (Android mutes it); IN1L goes straight to AIF1TX1/2 (One UI:
 *   through LHPF1 and ASRC1, whose coefficients are not in the vendor files).
 * - Before the output goes off it is muted through the 1 ms/6 dB ramp (One UI's "Output Ramp Up"; the ramp down is
 *   ours) and the output is enabled muted, then unmuted.
 *
 * Seen on a phone: nothing yet (written 2026-11-12, host-tested only). */
#include <stdarg.h>
#include "codec_media.h"
#include "drvprobe.h"

#define GPG1 0x104300A0UL           /* the codec's reset: gpg1-0 (codec.c) */
#define L33CTRL 0x6AU
#define L34CTRL 0x6BU

/* ---- the registers (include/linux/mfd/madera/registers.h) ---- */
#define R_SYSTEM_CLOCK_1 0x0101U
#define R_MIC_CHARGE_PUMP_1 0x0200U /* CPMIC_ENA 0x1, CPMIC_BYPASS 0x2 */
#define R_LDO2_CONTROL_1 0x0213U    /* LDO2_VSEL [10:5] (MICVDD) */
#define R_MIC_BIAS_CTRL_1 0x0218U
#define R_MIC_BIAS_CTRL_2 0x0219U   /* MICB2: EXT_CAP 0x8000, LVL [8:5], RATE 0x8, BYPASS 0x2, ENA 0x1 */
#define R_MIC_BIAS_CTRL_5 0x021CU   /* MICB1A_ENA 0x1 */
#define R_MIC_BIAS_CTRL_6 0x021EU   /* MICB2A_DISCH 0x2, MICB2A_ENA 0x1 */
#define R_HEADPHONE_DETECT_0 0x0299U    /* HPD_GND_SEL [2:0] */
#define R_MICD_CTRL_0 0x02A2U       /* MICD1_ADC_MODE 0x8000, MICD1_SENSE [7:4], MICD1_GND [2:0] */
#define R_MICD_CTRL_1 0x02A3U       /* BIAS_STARTTIME [15:12], RATE [11:8], BIAS_SRC [7:4], DBTIME 0x2, ENA 0x1 */
#define R_MICD_CTRL_2 0x02A4U       /* MICD_LVL_SEL [7:0] */
#define R_MICD_LEVEL_2 0x02A7U
#define R_MICD_LEVEL_3 0x02A8U
#define R_MICD_LEVEL_4 0x02A9U
#define R_MICD_CTRL_4 0x02ABU       /* MICDET_ADCVAL [6:0] */
#define R_MICD_CLAMP 0x02C6U        /* MICD_CLAMP_OVD 0x10, MICD_CLAMP_MODE [3:0] */
#define R_JACK_DETECT_ANALOGUE 0x02D3U  /* JD2_ENA 0x2, JD1_ENA 0x1 */
#define R_INPUT_ENABLES 0x0300U     /* IN3L 0x20, IN1L 0x02 */
#define R_INPUT_VOLUME_RAMP 0x0309U
#define R_HPF_CONTROL 0x030CU
#define R_IN1L_CONTROL 0x0310U      /* IN1_MODE 0x400, IN1L_PGA_VOL [7:1] */
#define R_ADC_VOL_1L 0x0311U        /* IN1L_SRC 0x4000 (B), IN1L_SRC_SE 0x2000, IN_VU 0x200, MUTE 0x100, VOL [7:0] */
#define R_IN3L_CONTROL 0x0320U
#define R_ADC_VOL_3L 0x0321U
#define R_DMIC3L_CONTROL 0x0322U
#define R_OUTPUT_ENABLES_1 0x0400U  /* OUT2L_ENA 0x8, OUT2R_ENA 0x4 */
#define R_OUTPUT_VOLUME_RAMP 0x0409U    /* OUT_VD_RAMP [6:4], OUT_VI_RAMP [2:0] */
#define R_DAC_VOL_2L 0x0419U        /* OUT_VU 0x200, OUT2L_MUTE 0x100, OUT2L_VOL [7:0] */
#define R_OUTPUT_PATH_CONFIG_2 0x041AU  /* HP2_GND_SEL [2:0] */
#define R_DAC_VOL_2R 0x041DU
#define R_AIF1_TX_ENABLES 0x0519U
#define R_AIF1_RX_ENABLES 0x051AU
#define R_OUT2LMIX_SRC 0x0690U
#define R_OUT2LMIX_VOL 0x0691U
#define R_OUT2RMIX_SRC 0x0698U
#define R_OUT2RMIX_VOL 0x0699U
#define R_AIF1TX1MIX_SRC 0x0700U
#define R_AIF1TX1MIX_VOL 0x0701U
#define R_AIF1TX2MIX_SRC 0x0708U
#define R_AIF1TX2MIX_VOL 0x0709U
#define R_IRQ1_RAW_STATUS_1 0x1880U
#define R_IRQ1_RAW_STATUS_7 0x1886U /* MICD_CLAMP_RISE_STS1 0x10, JD2 rise/fall 0x4/0x8, JD1 0x1/0x2 */
#define R_IRQ1_RAW_STATUS_13 0x188CU    /* HP2R/L_ENABLE_DONE 0x8 / 0x4 */
#define R_IRQ1_RAW_STATUS_14 0x188DU    /* HP2R/L_DISABLE_DONE */
#define R_INTERRUPT_DEBOUNCE_7 0x1A06U  /* MICD_CLAMP_DB 0x10, JD2_DB 0x4, JD1_DB 0x1 */

#define CLAMP_RISE 0x0010U
#define OUT2_ENA 0x000CU
#define HP2_DONE 0x000CU
#define DAC_VU 0x0200U
#define DAC_MUTE 0x0100U
#define IN_VU 0x0200U
#define IN_MUTE 0x0100U
#define SRC_AIF1RX1 0x20U
#define SRC_AIF1RX2 0x21U
#define SRC_IN1L 0x10U
#define SRC_IN3L 0x14U
#define MIX_0DB 0x80U               /* mixer "Input 1 Volume" 32 (range 0x20..0x50, 0x40 = 0 dB) << 1 */

/* the DT's accessory detect (cirrus,accdet acc@1) */
#define MICB2_CFG 0x81A8U           /* EXT_CAP, LVL 13 (1.5 V + 13 x 0.1 = 2.8 V), RATE (regulator-soft-start) */
#define MICB_CFG_MASK 0x81EAU
#define MICVDD_VSEL (0x24U << 5)    /* 3.0 V (micvdd), as codec.c */
#define MICD1_CFG 0x8001U           /* ADC mode (software compare), sense 0, ground 1 */
#define MICD1_CFG_MASK 0x80F7U
#define MICD_CTL1 0x1742U           /* bias start time 1, rate 7, bias source 4 (MICBIAS2A), dbtime 1 */
#define MICD_CTL1_MASK 0xFFF2U
#define CLAMP_MODE 0x8U             /* cirrus,micd-clamp-mode: JD1L_JD2L */
#define GND_SEL 1U                  /* micd-configs gnd (HPD_GND_SEL) and hp_gnd (HP2_GND_SEL) */
#define MIC_MIN_OHM 1258U           /* MADERA_MICROPHONE_MIN_OHM */
#define OPEN_OHM 0xFFFFFFFFU

/* the polls (CODEC_JACK_POLL_MS each) */
#define INS_POLLS 3U                /* plug in: 150 ms steady (the codec's own JD debounce and init-delay 30 ms inside) */
#define REM_POLLS 2U                /* plug out: 100 ms */
#define DET_DELAY_POLLS 6U          /* cirrus,micd-detect-debounce-ms 300 */
#define DET_TIMEOUT_POLLS 20U       /* cirrus,micd-timeout-ms 1000: still undecided = headphones */
#define SAME_POLLS 2U               /* the same plug type / button twice in a row */
#define RETRY_POLLS 100U            /* 5 s after a failure before the codec is brought up again */

static const unsigned micd_levels[65] = {   /* madera_micd_levels: MICDET_ADCVAL -> ohms */
    3, 6, 8, 11, 13, 16, 18, 21, 23, 26, 28, 31, 34, 36, 39, 41, 44, 46, 49, 52, 54, 57, 60, 62, 65, 67, 70, 73, 75, 78,
    81, 83, 89, 94, 100, 105, 111, 116, 122, 127, 139, 150, 161, 173, 186, 196, 209, 220, 245, 270, 295, 321, 348, 375,
    402, 430, 489, 550, 614, 681, 752, 903, 1071, 1257, 30000,
};
static const unsigned micd_range_max[5] = { 116, 173, 321, 752, 1257 };     /* cirrus,micd-ranges */
static const int micd_range_btn[5] = { CODEC_BTN_PLAY, CODEC_BTN_VOICE, CODEC_BTN_VOLUP, CODEC_BTN_VOLDOWN,
                                       CODEC_BTN_OTHER };

const uint8_t codec_hp_vol[CODEC_HP_LEVELS] = { 0, 51, 55, 59, 63, 67, 71, 75, 79, 83, 87, 91, 95, 99, 103, 107 };

static const struct audio_hal *M;
static const char *mwhy = "idle";
static unsigned mgen;               /* the codec power-up our set-up belongs to (codec_gen), 0 none */
static int hooked, yielding, jack_cfg, micd_on, hp_on, hp_level, mmic;     /* mmic: 0 off, 1 main, 2 headset */
enum { JS_OUT, JS_DETECT, JS_HP, JS_HEADSET };
static int js, jstate;
static unsigned in_same, out_same, det_polls, cls_last, cls_same, retry_wait;
static int btn_cur, btn_cand;
static unsigned btn_same, last_ohms = OPEN_OHM;
static int evq[8];
static unsigned evh, evn;

/* ---- the log ---- */

static void put(char *b, unsigned *n, unsigned cap, char c)
{
    if (*n + 1 < cap)
        b[(*n)++] = c;
}

/* %s %c %u %d %x with an optional zero-padded width */
static void vsay(const char *pre, const char *fmt, va_list ap)
{
    char b[220];
    unsigned n = 0;
    for (const char *p = pre; *p; ++p)
        put(b, &n, sizeof b, *p);
    for (const char *f = fmt; *f; ++f) {
        if (*f != '%') {
            put(b, &n, sizeof b, *f);
            continue;
        }
        ++f;
        unsigned width = 0;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (unsigned)(*f++ - '0');
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "(null)"; *s; ++s)
                put(b, &n, sizeof b, *s);
        } else if (*f == 'c') {
            put(b, &n, sizeof b, (char)va_arg(ap, int));
        } else if (*f == 'u' || *f == 'd' || *f == 'x') {
            unsigned long v;
            if (*f == 'd') {
                int s = va_arg(ap, int);
                if (s < 0) {
                    put(b, &n, sizeof b, '-');
                    v = (unsigned long)(-(long)s);
                } else {
                    v = (unsigned long)s;
                }
            } else {
                v = va_arg(ap, unsigned);
            }
            unsigned base = *f == 'x' ? 16U : 10U;
            char t[24];
            unsigned k = 0;
            do {
                t[k++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v && k < sizeof t);
            while (k < width && k < sizeof t)
                t[k++] = '0';
            while (k)
                put(b, &n, sizeof b, t[--k]);
        } else if (*f == '%') {
            put(b, &n, sizeof b, '%');
        } else {
            break;
        }
    }
    b[n] = 0;
    if (M && M->log)
        M->log(b);
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay("codec: media: ", fmt, ap);
    va_end(ap);
}

static void psay(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay("drvprobe: codec: ", fmt, ap);
    va_end(ap);
}

/* ---- the registers this file writes, and its access ---- */

static int mine(uint32_t reg)
{
    static const uint16_t list[] = {
        R_MIC_CHARGE_PUMP_1, R_LDO2_CONTROL_1, R_MIC_BIAS_CTRL_1, R_MIC_BIAS_CTRL_2, R_MIC_BIAS_CTRL_5,
        R_MIC_BIAS_CTRL_6, R_HEADPHONE_DETECT_0, R_MICD_CTRL_0, R_MICD_CTRL_1, R_MICD_CTRL_2, R_MICD_LEVEL_2,
        R_MICD_LEVEL_3, R_MICD_LEVEL_4, R_MICD_CLAMP, R_JACK_DETECT_ANALOGUE, R_INPUT_ENABLES, R_INPUT_VOLUME_RAMP,
        R_HPF_CONTROL, R_IN1L_CONTROL, R_ADC_VOL_1L, R_IN3L_CONTROL, R_ADC_VOL_3L, R_DMIC3L_CONTROL,
        R_OUTPUT_ENABLES_1, R_OUTPUT_VOLUME_RAMP, R_DAC_VOL_2L, R_OUTPUT_PATH_CONFIG_2, R_DAC_VOL_2R,
        R_AIF1_TX_ENABLES, R_AIF1_RX_ENABLES, R_OUT2LMIX_SRC, R_OUT2LMIX_VOL, R_OUT2RMIX_SRC, R_OUT2RMIX_VOL,
        R_AIF1TX1MIX_SRC, R_AIF1TX1MIX_VOL, R_AIF1TX2MIX_SRC, R_AIF1TX2MIX_VOL, R_INTERRUPT_DEBOUNCE_7,
    };
    for (unsigned i = 0; i < sizeof list / sizeof list[0]; ++i)
        if (list[i] == reg)
            return 1;
    return 0;
}

static int m_rd(uint32_t reg, uint16_t *v)
{
    int rc = codec_read(reg, v);
    if (rc)
        say("register %04x could not be read", reg);
    return rc;
}

/* read, change the mask's bits, write; then the `check' bits read back as written (0: not read back) */
static int m_upd(uint32_t reg, uint16_t mask, uint16_t val, uint16_t check)
{
    if (!mine(reg)) {
        say("refused: register %04x is not one this file writes", reg);
        return AUD_EPERM;
    }
    uint16_t now = 0, back = 0;
    int rc = m_rd(reg, &now);
    if (rc)
        return rc;
    uint16_t want = (uint16_t)((now & ~mask) | (val & mask));
    if ((rc = codec_write(reg, want)))
        return rc;
    if (!check)
        return 0;
    if ((rc = m_rd(reg, &back)))
        return rc;
    if ((back ^ want) & check) {
        say("register %04x <- %04x reads back %04x", reg, want, back);
        return AUD_EVERIFY;
    }
    return 0;
}

#define UPD(r, m, v) do { if ((rc = m_upd((r), (m), (v), (m)))) return rc; } while (0)
#define UPD_NV(r, m, v) do { if ((rc = m_upd((r), (m), (v), 0))) return rc; } while (0)

static void delay_ms(unsigned ms)
{
    M->delay_us(ms * 1000U);
}

/* ---- state ---- */

static void push(int e)
{
    if (evn < sizeof evq / sizeof evq[0])
        evq[(evh + evn++) % (sizeof evq / sizeof evq[0])] = e;
}

/* our view of the chip is gone (it was reset, or a failure put it there): nothing of ours is on any more */
static void forget(void)
{
    if (btn_cur)
        push(-btn_cur);
    btn_cur = btn_cand = 0;
    btn_same = 0;
    hp_on = mmic = micd_on = jack_cfg = 0;
    mgen = 0;
    js = JS_OUT;
    jstate = CODEC_JACK_NONE;
    in_same = out_same = det_polls = cls_last = cls_same = 0;
}

static int media_fail(int rc, const char *what)
{
    say("FAILED (%d): %s", rc, what);
    forget();
    retry_wait = RETRY_POLLS;
    if (!codec_call_active()) {
        yielding = 1;
        codec_off();
        yielding = 0;
        say("the codec is back in reset; the jack is tried again in %u s", RETRY_POLLS * CODEC_JACK_POLL_MS / 1000U);
    } else {
        say("a call holds the codec: it is left to the call");
    }
    mwhy = what;
    return rc;
}

/* the codec reset or re-probed behind our back (codec_hal_set): our state is stale */
static void sync_gen(void)
{
    unsigned g = codec_gen();
    if (mgen && g != mgen) {
        say("the codec was reset by its other user: jack detection starts again");
        forget();
    }
}

static int need_micvdd(void)
{
    return micd_on || mmic || codec_call_active();
}

static int micvdd_on(void)
{
    int rc;
    uint16_t v = 0;
    if ((rc = m_rd(R_MIC_CHARGE_PUMP_1, &v)))
        return rc;
    if ((v & 0x3U) == 0x1U)
        return 0;
    UPD(R_LDO2_CONTROL_1, 0x07E0, MICVDD_VSEL);
    UPD(R_MIC_CHARGE_PUMP_1, 0x0003, 0x0001);   /* CPMIC_ENA, not bypassed */
    delay_ms(3);                                /* its enable time */
    return 0;
}

static void micvdd_release(void)
{
    if (!need_micvdd())
        (void)m_upd(R_MIC_CHARGE_PUMP_1, 0x0001, 0x0000, 0);
}

/* ---- the jack: madera's probe-time set-up, MICD start / stop / the ADC reading ---- */

static unsigned level_index(unsigned ohms)
{
    unsigned j = 0;
    while (j + 1 < sizeof micd_levels / sizeof micd_levels[0] && micd_levels[j] < ohms)
        ++j;
    return j;
}

static int jack_setup(void)
{
    int rc;
    UPD(R_MIC_BIAS_CTRL_2, MICB_CFG_MASK, MICB2_CFG);   /* madera_configure_micbias: MICBIAS2 2.8 V */
    UPD(R_MIC_BIAS_CTRL_6, 0x0002, 0x0002);             /* MICBIAS2A active discharge */
    UPD(R_LDO2_CONTROL_1, 0x07E0, MICVDD_VSEL);
    UPD(R_MICD_CTRL_1, MICD_CTL1_MASK, MICD_CTL1);
    UPD(R_MICD_CTRL_0, MICD1_CFG_MASK, MICD1_CFG);
    UPD(R_HEADPHONE_DETECT_0, 0x0007, GND_SEL);
    UPD(R_OUTPUT_PATH_CONFIG_2, 0x0007, GND_SEL);       /* HP2_GND_SEL (micd-configs hp_gnd) */
    /* the button ranges (madera_micd_set_level: range i at LEVEL_4 - i/2, even upper byte, odd lower) */
    uint16_t lv[3] = { 0, 0, 0 };
    for (unsigned i = 0; i < 5; ++i) {
        unsigned j = level_index(micd_range_max[i]);
        lv[i / 2] |= (uint16_t)(i % 2 ? j : j << 8);
    }
    UPD(R_MICD_LEVEL_4, 0x3F3F, lv[0]);
    UPD(R_MICD_LEVEL_3, 0x3F3F, lv[1]);
    UPD(R_MICD_LEVEL_2, 0x3F00, lv[2]);
    UPD(R_MICD_CTRL_2, 0x00FF, 0x001F);
    UPD(R_MICD_CLAMP, 0x000F, CLAMP_MODE);
    UPD(R_INTERRUPT_DEBOUNCE_7, 0x0015, 0x0015);        /* MICD_CLAMP_DB, JD2_DB, JD1_DB */
    UPD(R_JACK_DETECT_ANALOGUE, 0x0003, 0x0003);        /* JD1_ENA, JD2_ENA (cirrus,jd-use-jd2) */
    say("jack: JD1/JD2 on, clamp mode 8 (JD1L_JD2L), MICBIAS2 2.8 V, MICD on MICBIAS2A (ADC mode, rate 7), "
        "buttons <= 116/173/321/752/1257 ohms");
    return 0;
}

static int micd_start(void)
{
    int rc;
    if ((rc = micvdd_on()))
        return rc;
    UPD(R_MICD_CLAMP, 0x0010, 0x0000);          /* MICD_CLAMP_OVD off */
    UPD(R_MICD_CTRL_0, 0x8000, 0x8000);         /* ADC mode */
    UPD(R_MIC_BIAS_CTRL_2, 0x0001, 0x0001);     /* cirrus,micd-force-micbias: MICBIAS2 and 2A on */
    UPD(R_MIC_BIAS_CTRL_6, 0x0001, 0x0001);
    UPD(R_MICD_CTRL_1, 0x0001, 0x0001);         /* MICD_ENA */
    micd_on = 1;
    return 0;
}

static void micd_stop(void)
{
    (void)m_upd(R_MICD_CTRL_1, 0x0001, 0x0000, 0);
    if (mmic != 2) {
        (void)m_upd(R_MIC_BIAS_CTRL_6, 0x0001, 0x0000, 0);
        (void)m_upd(R_MIC_BIAS_CTRL_2, 0x0001, 0x0000, 0);
    }
    micd_on = 0;
    micvdd_release();
    (void)m_upd(R_MICD_CLAMP, 0x0010, 0x0010, 0);   /* MICD_CLAMP_OVD on */
}

/* madera_micd_adc_read: MICD off, ADCVAL read, MICD on again for the next one; ohms or OPEN_OHM */
static int micd_read(unsigned *ohms)
{
    int rc;
    uint16_t v = 0;
    UPD_NV(R_MICD_CTRL_1, 0x0001, 0x0000);
    if ((rc = m_rd(R_MICD_CTRL_4, &v)))
        return rc;
    UPD_NV(R_MICD_CTRL_1, 0x0001, 0x0001);
    v &= 0x7F;
    *ohms = v < sizeof micd_levels / sizeof micd_levels[0] ? micd_levels[v] : OPEN_OHM;
    last_ohms = *ohms;
    return 0;
}

static int ohms_btn(unsigned ohms)
{
    if (ohms >= MIC_MIN_OHM)
        return 0;
    for (unsigned i = 0; i < 5; ++i)
        if (ohms <= micd_range_max[i])
            return micd_range_btn[i];
    return 0;
}

static const char *jack_name(int s)
{
    return s == CODEC_JACK_HEADSET ? "headset" : s == CODEC_JACK_HEADPHONES ? "headphones" : "no plug";
}

static void hp_off_seq(void);
static void mic_off_seq(void);
static void release_clocks(void);
static void on_yield(int reset);

static void unplugged(void)
{
    say("jack: plug out (was %s)", jack_name(jstate));
    if (hp_on) {
        hp_off_seq();
        hp_on = 0;
        say("headphones off: the plug was pulled out");
    }
    if (mmic == 2) {
        mic_off_seq();
        mmic = 0;
        say("headset mic capture off: the plug was pulled out");
    }
    release_clocks();
    if (btn_cur)
        push(-btn_cur);
    btn_cur = btn_cand = 0;
    btn_same = 0;
    if (micd_on)
        micd_stop();
    js = JS_OUT;
    jstate = CODEC_JACK_NONE;
    in_same = out_same = 0;
    mwhy = "no plug in the jack";
}

static int ensure_hal(void)
{
    if (!M)
        M = codec_hal();
#ifdef SBOS_KERNEL
    if (!M)
        codec_media_hal_set(audio_hal_libsb());
#endif
    if (!M)
        return 0;
    if (!hooked) {
        codec_set_yield(on_yield);
        hooked = 1;
    }
    return M->delay_us != 0;
}

/* codec_up, and our state reset if this is a new power-up */
static int up(void)
{
    unsigned g = 0;
    int rc = codec_up(&g);
    if (rc)
        return rc;
    if (g != mgen) {
        forget();
        mgen = g;
    }
    return 0;
}

int codec_jack_poll(void)
{
    if (!ensure_hal()) {
        mwhy = "no HAL";
        return CODEC_JACK_NONE;
    }
    sync_gen();
    int rc;
    if (!jack_cfg) {
        if (retry_wait) {
            --retry_wait;
            return CODEC_JACK_NONE;
        }
        if ((rc = up())) {
            media_fail(rc, "the codec did not come up for the jack");
            return CODEC_JACK_NONE;
        }
        if ((rc = jack_setup())) {
            media_fail(rc, "the jack detection set-up failed");
            return CODEC_JACK_NONE;
        }
        jack_cfg = 1;
        mwhy = "jack detection on";
    }
    uint16_t st = 0;
    if ((rc = m_rd(R_IRQ1_RAW_STATUS_7, &st))) {
        media_fail(rc, "the jack status could not be read");
        return CODEC_JACK_NONE;
    }
    int raw = !(st & CLAMP_RISE);
    if (js == JS_OUT) {
        out_same = 0;
        in_same = raw ? in_same + 1 : 0;
        if (in_same >= INS_POLLS) {
            say("jack: plug in (raw status 7 %04x): mic detect for up to %u ms", st,
                DET_TIMEOUT_POLLS * CODEC_JACK_POLL_MS);
            if ((rc = micd_start())) {
                media_fail(rc, "the mic detection could not be started");
                return CODEC_JACK_NONE;
            }
            js = JS_DETECT;
            det_polls = cls_last = cls_same = 0;
        }
        return jstate;
    }
    out_same = raw ? 0 : out_same + 1;
    if (out_same >= REM_POLLS) {
        unplugged();
        return jstate;
    }
    if (!raw)
        return jstate;
    unsigned ohms = 0;
    if (js == JS_DETECT) {
        if (++det_polls < DET_DELAY_POLLS)
            return jstate;
        if ((rc = micd_read(&ohms))) {
            media_fail(rc, "the mic detection could not be read");
            return CODEC_JACK_NONE;
        }
        unsigned cls = ohms >= MIC_MIN_OHM ? CODEC_JACK_HEADSET : CODEC_JACK_HEADPHONES;   /* open = headset */
        cls_same = cls == cls_last ? cls_same + 1 : 1;
        cls_last = cls;
        int timeout = det_polls >= DET_TIMEOUT_POLLS;
        if (cls_same < SAME_POLLS && !timeout)
            return jstate;
        if (cls_same < SAME_POLLS)
            cls = CODEC_JACK_HEADPHONES;
        jstate = (int)cls;
        if (cls == CODEC_JACK_HEADSET) {
            js = JS_HEADSET;
            btn_cur = btn_cand = 0;
            btn_same = 0;
        } else {
            js = JS_HP;
            micd_stop();
        }
        if (ohms == OPEN_OHM)
            say("jack: %s (mic contact open: counted as a headset)%s", jack_name(jstate), timeout ? ", timed out" : "");
        else
            say("jack: %s (MICDET %u ohms)%s", jack_name(jstate), ohms, timeout ? " (undecided in 1 s)" : "");
        mwhy = jstate == CODEC_JACK_HEADSET ? "a headset is plugged in" : "headphones are plugged in";
        return jstate;
    }
    if (js == JS_HEADSET) {
        if ((rc = micvdd_on())) {           /* the call path switches MICVDD off when a call ends */
            media_fail(rc, "MICVDD could not be kept on for the buttons");
            return CODEC_JACK_NONE;
        }
        if ((rc = micd_read(&ohms))) {
            media_fail(rc, "the headset buttons could not be read");
            return CODEC_JACK_NONE;
        }
        int b = ohms_btn(ohms);
        btn_same = b == btn_cand ? btn_same + 1 : 1;
        btn_cand = b;
        if (btn_same >= SAME_POLLS && b != btn_cur) {
            if (btn_cur)
                push(-btn_cur);
            if (b)
                push(b);
            say("jack: button %u %s (%u ohms)", (unsigned)(b ? b : btn_cur), b ? "pressed" : "released", ohms);
            btn_cur = b;
        }
    }
    return jstate;
}

int codec_jack_event(void)
{
    if (!evn)
        return 0;
    int e = evq[evh];
    evh = (evh + 1) % (sizeof evq / sizeof evq[0]);
    --evn;
    return e;
}

unsigned codec_jack_ohms(void)
{
    return last_ohms;
}

/* ---- the headphones ---- */

/* the only writer of the two volume registers: the cap is checked on every write */
static int hp_vol(unsigned vol, int mute)
{
    if (vol > CODEC_HP_VOL_MAX) {
        say("refused: headphone volume %u is above the cap %u (One UI's)", vol, CODEC_HP_VOL_MAX);
        return AUD_EPERM;
    }
    int rc;
    uint16_t v = (uint16_t)((mute ? DAC_MUTE : 0U) | vol);
    UPD(R_DAC_VOL_2L, 0x01FF, v);
    UPD_NV(R_DAC_VOL_2L, DAC_VU, DAC_VU);
    UPD(R_DAC_VOL_2R, 0x01FF, v);
    UPD_NV(R_DAC_VOL_2R, DAC_VU, DAC_VU);
    return 0;
}

static int done_wait(uint32_t reg, const char *what)
{
    uint16_t v = 0;
    for (int i = 0; i < 10; ++i) {
        int rc = m_rd(reg, &v);
        if (rc)
            return rc;
        if ((v & HP2_DONE) == HP2_DONE)
            return 0;
        delay_ms(2);
    }
    say("HPOUT2: %s not reported within 20 ms (status %04x): carried on", what, v);
    return 0;
}

static int hp_on_seq(int level)
{
    int rc;
    UPD(R_OUTPUT_VOLUME_RAMP, 0x0077, 0x0022);      /* "Output Ramp Up" 1ms/6dB; the ramp down the same */
    if ((rc = hp_vol(codec_hp_vol[level], 1)))
        return rc;
    UPD(R_OUT2LMIX_SRC, 0x00FF, SRC_AIF1RX1);       /* route-headphone */
    UPD(R_OUT2LMIX_VOL, 0x00FE, MIX_0DB);
    UPD(R_OUT2RMIX_SRC, 0x00FF, SRC_AIF1RX2);
    UPD(R_OUT2RMIX_VOL, 0x00FE, MIX_0DB);
    UPD(R_AIF1_RX_ENABLES, 0x0003, 0x0003);
    UPD(R_OUTPUT_ENABLES_1, OUT2_ENA, OUT2_ENA);    /* madera_hp_ev POST_PMU; the codec's own HP sequence */
    delay_ms(12);                                   /* madera_out_ev: 6 ms per HP output on CS47L92/93 */
    if ((rc = done_wait(R_IRQ1_RAW_STATUS_13, "ENABLE_DONE")))
        return rc;
    if (level && (rc = hp_vol(codec_hp_vol[level], 0)))  /* unmuted: ramps up at 1 ms/6 dB */
        return rc;
    return 0;
}

/* best effort, every step tried: muted (ramped down), output off, AIF1RX and the mixers off */
static void hp_off_seq(void)
{
    (void)m_upd(R_DAC_VOL_2L, DAC_MUTE | DAC_VU, DAC_MUTE | DAC_VU, 0);
    (void)m_upd(R_DAC_VOL_2R, DAC_MUTE | DAC_VU, DAC_MUTE | DAC_VU, 0);
    delay_ms(10);                                   /* -10.5 dB to mute at 1 ms/6 dB */
    (void)m_upd(R_OUTPUT_ENABLES_1, OUT2_ENA, 0, 0);
    delay_ms(2);                                    /* madera_out_ev: 1 ms per output */
    (void)done_wait(R_IRQ1_RAW_STATUS_14, "DISABLE_DONE");
    (void)m_upd(R_AIF1_RX_ENABLES, 0x0003, 0x0000, 0);
    (void)m_upd(R_OUT2LMIX_SRC, 0x00FF, 0x0000, 0);
    (void)m_upd(R_OUT2RMIX_SRC, 0x00FF, 0x0000, 0);
}

static void release_clocks(void)
{
    if (!hp_on && !mmic)
        (void)codec_clocks(0);
}

int codec_hp(int on, int level)
{
    if (!ensure_hal()) {
        mwhy = "no HAL";
        return AUD_ENOSYS;
    }
    sync_gen();
    int rc;
    if (!on) {
        if (hp_on) {
            hp_off_seq();
            hp_on = 0;
            release_clocks();
            say("headphones off");
        }
        mwhy = "headphones off";
        return 0;
    }
    level = level < 0 ? 0 : level >= CODEC_HP_LEVELS ? CODEC_HP_LEVELS - 1 : level;
    if (codec_call_active()) {
        mwhy = "a call has the codec: headphones not switched on";
        return AUD_EBUSY;
    }
    if (jstate == CODEC_JACK_NONE) {
        mwhy = "no headphones are plugged in";
        return AUD_EPERM;
    }
    if (hp_on) {
        if (level == hp_level)
            return 0;
        if ((rc = hp_vol(codec_hp_vol[level], level == 0)))
            return media_fail(rc, "the headphone volume could not be set");
        hp_level = level;
        say("headphones: level %u (volume %u)", (unsigned)level, codec_hp_vol[level]);
        return 0;
    }
    say("headphones on: AIF1RX1/2 -> OUT2L/R mixers (0 dB) -> HPOUT2, level %u (volume %u, cap %u)", (unsigned)level,
        codec_hp_vol[level], CODEC_HP_VOL_MAX);
    if ((rc = up()))
        return media_fail(rc, "the codec did not come up for the headphones");
    if ((rc = codec_clocks(1)))
        return media_fail(rc, "the clocks (CLKOUT0, SYSCLK, FLL1) did not come up for the headphones");
    hp_on = 1;
    hp_level = level;
    if ((rc = hp_on_seq(level)))
        return media_fail(rc, "the headphone output did not come up");
    mwhy = "headphones on";
    say("headphones on: done");
    return 0;
}

/* ---- capture for apps ---- */

static int mic_on_seq(int which)
{
    int rc;
    if ((rc = micvdd_on()))
        return rc;
    if (which == 0) {
        /* mixer_paths.xml's defaults for IN3, route-main-mic (mono), gain-recording-mic */
        UPD(R_DMIC3L_CONTROL, 0x0700, 5U << 8);     /* IN3 OSR 3.072 MHz */
        UPD(R_IN3L_CONTROL, 0x8000, 0x8000);        /* IN3L HPF */
        UPD(R_HPF_CONTROL, 0x0007, 2);              /* 10 Hz */
        UPD(R_INPUT_VOLUME_RAMP, 0x0007, 5);        /* 8 ms/6 dB */
        UPD(R_ADC_VOL_3L, 0x01FF, IN_MUTE | CODEC_MEDIA_MAIN_VOL);
        UPD(R_AIF1TX1MIX_SRC, 0x00FF, SRC_IN3L);
        UPD(R_AIF1TX2MIX_SRC, 0x00FF, SRC_IN3L);
        UPD(R_MIC_BIAS_CTRL_1, 0x0001, 0x0001);     /* MICBIAS1 */
        UPD(R_MIC_BIAS_CTRL_5, 0x0001, 0x0001);     /* MICBIAS1A (DMIC1) */
        delay_ms(5);
    } else {
        /* IN1L Mux B (IN1BL, differential: cirrus,inmode), route-headset-mic, gain-recording-headset-mic */
        UPD(R_ADC_VOL_1L, 0x61FF, 0x4000 | IN_MUTE | CODEC_MEDIA_HS_VOL);
        UPD(R_IN1L_CONTROL, 0x04FE, (uint16_t)((0x40U + CODEC_MEDIA_HS_PGA) << 1));    /* analogue, +8 dB */
        UPD(R_INPUT_VOLUME_RAMP, 0x0007, 5);
        UPD(R_AIF1TX1MIX_SRC, 0x00FF, SRC_IN1L);
        UPD(R_AIF1TX2MIX_SRC, 0x00FF, SRC_IN1L);
        UPD(R_MIC_BIAS_CTRL_2, 0x0001, 0x0001);     /* MICBIAS2 / 2A (already on for the buttons) */
        UPD(R_MIC_BIAS_CTRL_6, 0x0001, 0x0001);
    }
    UPD(R_AIF1TX1MIX_VOL, 0x00FE, MIX_0DB);
    UPD(R_AIF1TX2MIX_VOL, 0x00FE, MIX_0DB);
    uint32_t vreg = which == 0 ? R_ADC_VOL_3L : R_ADC_VOL_1L;
    UPD(R_AIF1_TX_ENABLES, 0x0003, 0x0003);
    UPD(R_INPUT_ENABLES, which == 0 ? 0x0020 : 0x0002, which == 0 ? 0x0020 : 0x0002);
    delay_ms(2);                                    /* madera_in_ev POST_PMU: unmuted, then the volume update */
    UPD(vreg, IN_MUTE, 0);
    delay_ms(2);
    UPD_NV(vreg, IN_VU, IN_VU);
    return 0;
}

static void mic_off_seq(void)
{
    int which = mmic == 2 ? 1 : 0;
    uint32_t vreg = which == 0 ? R_ADC_VOL_3L : R_ADC_VOL_1L;
    (void)m_upd(vreg, IN_MUTE | IN_VU, IN_MUTE | IN_VU, 0);
    (void)m_upd(R_INPUT_ENABLES, which == 0 ? 0x0020 : 0x0002, 0, 0);
    (void)m_upd(R_AIF1_TX_ENABLES, 0x0003, 0x0000, 0);
    (void)m_upd(R_AIF1TX1MIX_SRC, 0x00FF, 0x0000, 0);
    (void)m_upd(R_AIF1TX2MIX_SRC, 0x00FF, 0x0000, 0);
    if (which == 0) {
        (void)m_upd(R_MIC_BIAS_CTRL_5, 0x0001, 0x0000, 0);
        (void)m_upd(R_MIC_BIAS_CTRL_1, 0x0001, 0x0000, 0);
    } else if (!micd_on) {
        (void)m_upd(R_MIC_BIAS_CTRL_6, 0x0001, 0x0000, 0);
        (void)m_upd(R_MIC_BIAS_CTRL_2, 0x0001, 0x0000, 0);
    }
    int was = mmic;
    mmic = 0;
    micvdd_release();
    mmic = was;
}

int codec_mic_media(int on, int which)
{
    if (!ensure_hal()) {
        mwhy = "no HAL";
        return AUD_ENOSYS;
    }
    sync_gen();
    int rc;
    if (!on) {
        if (mmic) {
            mic_off_seq();
            mmic = 0;
            release_clocks();
            say("capture off");
        }
        mwhy = "capture off";
        return 0;
    }
    if (which != 0 && which != 1)
        return AUD_EINVAL;
    if (codec_call_active()) {
        mwhy = "a call has the microphone";
        return AUD_EBUSY;
    }
    if (which == 1 && jstate != CODEC_JACK_HEADSET) {
        mwhy = "no headset with a microphone is plugged in";
        return AUD_EPERM;
    }
    if (mmic == which + 1)
        return 0;
    if (mmic) {
        mic_off_seq();
        mmic = 0;
    }
    say("capture on: %s to AIF1TX1/2 (an I2S slave of UAIF0, 48 kHz 2x16)",
        which ? "the headset mic (IN1BL, MICBIAS2A, +8 dB analogue)" : "the main mic (DMIC1 on IN3L, MICBIAS1A, +0.5 dB)");
    if ((rc = up()))
        return media_fail(rc, "the codec did not come up for capture");
    if ((rc = codec_clocks(1)))
        return media_fail(rc, "the clocks (CLKOUT0, SYSCLK, FLL1) did not come up for capture");
    mmic = which + 1;
    if ((rc = mic_on_seq(which)))
        return media_fail(rc, "the capture path did not come up");
    mwhy = which ? "capturing from the headset mic" : "capturing from the main mic";
    say("capture on: done");
    return 0;
}

/* ---- calls win ---- */

static void on_yield(int reset)
{
    if (yielding || !M)
        return;
    yielding = 1;
    int had = hp_on || mmic;
    if (codec_gen() && mgen == codec_gen()) {
        if (hp_on)
            hp_off_seq();
        hp_on = 0;
        if (mmic)
            mic_off_seq();
        mmic = 0;
        if (reset && micd_on)
            micd_stop();
        (void)codec_clocks(0);
    }
    hp_on = mmic = 0;
    if (reset)
        forget();
    if (had) {
        mwhy = reset ? "the codec was reset by the call path: headphones / capture stopped"
                     : "a call took the codec: headphones / capture stopped";
        say("%s", mwhy);
    }
    yielding = 0;
}

void codec_media_hal_set(const struct audio_hal *hal)
{
    codec_hal_set(hal);
    M = hal;
    forget();
    evh = evn = 0;
    retry_wait = 0;
    last_ohms = OPEN_OHM;
    codec_set_yield(on_yield);
    hooked = 1;
    mwhy = "idle";
}

void codec_media_off(void)
{
    if (!ensure_hal())
        return;
    sync_gen();
    if (codec_gen() && mgen == codec_gen()) {
        if (hp_on)
            hp_off_seq();
        hp_on = 0;
        if (mmic)
            mic_off_seq();
        mmic = 0;
        if (micd_on)
            micd_stop();
        (void)codec_clocks(0);
    }
    forget();
    retry_wait = 0;
    if (!codec_call_active() && codec_gen()) {
        yielding = 1;
        codec_off();
        yielding = 0;
    }
    mwhy = "off";
}

const char *codec_media_why(void)
{
    return mwhy;
}

/* ---- the drivers-track probe ---- */

static int p_level = -1, p_step, p_last;
static unsigned p_polls, p_work, p_changes, p_buttons;
static uint16_t p_id, p_rev, p_boot;

static const char *btn_name(int b)
{
    static const char *const n[] = { "?", "play/pause", "voice", "volume up", "volume down", "other" };
    return n[b >= 1 && b <= 5 ? b : 0];
}

static int probe_level0(void)
{
    uint32_t con = M->rd32(GPG1), dat = M->rd32(GPG1 + 4), pud = M->rd32(GPG1 + 8);
    unsigned fn = con & 0xFU, lvl = dat & 1U;
    uint8_t l33 = 0, l34 = 0;
    int r33 = M->pmic_read ? M->pmic_read(L33CTRL, &l33) : AUD_ENOSYS;
    int r34 = M->pmic_read ? M->pmic_read(L34CTRL, &l34) : AUD_ENOSYS;
    psay("level 0: reset gpg1-0 con %x dat %x pud %x: %s, level %u", con, dat, pud,
         fn == 1 ? "an output" : fn == 0 ? "an input" : "another function", lvl);
    if (r33 || r34) {
        psay("FAIL the PMIC's LDO33 / LDO34 registers could not be read");
        return 1;
    }
    psay("level 0: LDO33 (AVDD, DBVDD1, CPVDD1) %02x %s; LDO34 (DCVDD, CPVDD2) %02x %s at %u mV", l33,
         (l33 & 0xC0) == 0xC0 ? "on" : "OFF", l34, (l34 & 0xC0) == 0xC0 ? "on" : "off", 700U + (l34 & 0x3FU) * 25U / 2U);
    const char *idw = "held in reset: ID not read, the codec untouched";
    uint16_t id = 0;
    if (fn == 1 && lvl == 1) {
        if (codec_peek(0, &id) == 0) {
            psay("level 0: out of reset, ID %04x (%s)", id, id == CODEC_ID ? "CS47L92/93" : "NOT a CS47L92/93");
            idw = id == CODEC_ID ? "out of reset, ID 6371" : "out of reset, ID NOT 6371";
        } else {
            idw = "out of reset, but its SPI bus is not set up in this boot: ID not read";
        }
    } else if (fn != 1) {
        idw = "the reset pin is not an output: ID not read, the codec untouched";
    }
    psay("PASS level 0: reset %s, LDO33 %s, LDO34 %s %u mV; %s", fn == 1 && !lvl ? "held (low)" : "not held",
         (l33 & 0xC0) == 0xC0 ? "on" : "OFF", (l34 & 0xC0) == 0xC0 ? "on" : "off", 700U + (l34 & 0x3FU) * 25U / 2U,
         idw);
    return 1;
}

static int probe_end(int ok, const char *fail_why)
{
    codec_media_off();
    int r = M->gpio_get ? M->gpio_get(GPG1, 0) : -1;
    if (r != 0 && !codec_call_active()) {
        psay("FAIL the codec did not go back in reset (gpg1-0 reads %d)", r);
    } else if (!ok) {
        psay("FAIL %s; the codec is back in reset", fail_why);
    } else {
        psay("PASS level %d: ID %04x, revision %u, BOOT_DONE; jack: %s (%u changes, %u button events); outputs off, "
             "back in reset", p_level, p_id, p_rev & 0xFFU, jack_name(p_last), p_changes, p_buttons);
    }
    p_level = -1;
    return 1;
}

int drv_codec_probe(int level, unsigned *wait_ms)
{
    *wait_ms = 0;
    if (!ensure_hal()) {
        psay("FAIL no hardware access (no HAL)");
        return 1;
    }
    if (level != p_level) {
        p_level = level;
        p_step = 0;
        p_polls = p_work = p_changes = p_buttons = 0;
        p_last = CODEC_JACK_NONE;
    }
    if (level <= DRV_LVL_READ) {
        p_level = -1;
        return probe_level0();
    }
    unsigned dur = level >= DRV_LVL_ATTENDED ? 10000U : 2000U;
    unsigned budget = level >= DRV_LVL_ATTENDED ? 14000U : 5000U;
    if (p_polls * CODEC_JACK_POLL_MS + p_work > budget)
        return probe_end(0, "the step ran past its time budget");
    if (p_step != 1)
        p_work += 100;              /* the up step and the end: at most ~100 ms each */
    if (p_step == 0) {
        psay("level %d: reset released, ID / revision / BOOT_DONE, then the jack for %u s (outputs off)", level,
             dur / 1000U);
        if (codec_call_active())
            return probe_end(0, "a call holds the codec");
        int rc = up();
        if (rc) {
            psay("FAIL the codec did not come up (%d): %s", rc, codec_why());
            p_level = -1;
            return 1;
        }
        if (codec_read(0x0000, &p_id) || codec_read(0x0001, &p_rev) || codec_read(R_IRQ1_RAW_STATUS_1, &p_boot))
            return probe_end(0, "the ID registers could not be read after the boot");
        psay("level %d: ID %04x, revision %u, IRQ1_RAW_STATUS_1 %04x", level, p_id, p_rev & 0xFFU, p_boot);
        if (level >= DRV_LVL_ATTENDED)
            psay("level 2: now plug in / pull out the headphones or a headset and press its buttons (%u s)",
                 dur / 1000U);
        p_step = 1;
        return 0;
    }
    if (p_step == 1) {
        int s = codec_jack_poll();
        if (!jack_cfg)
            return probe_end(0, mwhy);
        if (p_polls == 0 || s != p_last) {
            if (s == CODEC_JACK_NONE)
                psay("jack: no plug");
            else
                psay("jack: %s (MICDET %u ohms)", jack_name(s), last_ohms);
            if (p_polls)
                ++p_changes;
            p_last = s;
        } else if (js == JS_DETECT && det_polls == 1) {
            psay("jack: a plug is in: telling headphones from a headset");
        }
        for (int e; (e = codec_jack_event());) {
            psay("jack: button %s %s", btn_name(e < 0 ? -e : e), e > 0 ? "pressed" : "released");
            ++p_buttons;
        }
        if (level >= DRV_LVL_ATTENDED && s && p_polls % 20 == 19)
            psay("jack: %s, MICDET %u ohms", jack_name(s), last_ohms);
        if (++p_polls * CODEC_JACK_POLL_MS >= dur) {
            p_step = 2;
            return 0;
        }
        *wait_ms = CODEC_JACK_POLL_MS;
        return 0;
    }
    return probe_end(1, 0);
}

void drv_codec_abort(void)
{
    if (!ensure_hal())
        return;
    psay("abort: everything off, the codec back in reset");
    codec_media_off();
    p_level = -1;
}
