/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS survival tools (1.2-alpha27): see survival.h.
 *
 * USB power out, the registers (MAX77705 charger block, I2C 0x69; mainline include/linux/power/max77705_charger.h
 * and drivers/power/supply/max77705_charger.c, GPL-2.0, reference only):
 *   0xB2 CHG_INT_OK     bit 6 CHGIN_OK (an input on the USB-C port)
 *   0xB3 DETAILS_00     bits 6:5 CHGIN_DTLS
 *   0xB4 DETAILS_01     bits 3:0 CHG_DTLS, 6:4 BAT_DTLS
 *   0xB5 DETAILS_02     bits 3:0 BYP_DTLS (the boost / bypass path's own status: limits and faults)
 *   0xB7 CHG_CNFG_00    bits 3:0 MODE: bit 0 CHG, 1 OTG, 2 BUCK, 3 BOOST; OTG out = OTG | BOOST (0x0A); bit 4 WDTEN kept
 *   0xB9 CHG_CNFG_02    bits 7:6 OTG_ILIM 500 / 900 / 1200 / 1500 mA (bits 5:0 the charge current, kept)
 * The USB-C controller (I2C 0x25, read only here): 0x06 USBC_STATUS1 bits 7:3 VBADC (VBUS in ~1 V steps: 0 below
 * 3.5 V, 1 3.5-4.5, 2 4.5-5.5, ...), 0x08 BC_STATUS bit 7 VBUSDET. Nothing else is written: no protection is changed.
 * Writes go through libsb's gate (sb_max_write) with a tier-1 authorisation whose held step is the UI's hold-to-arm.
 */
#include "sb.h"
#include "kern.h"
#include "kapp.h"
#include "survival.h"
#include "gnssfix.h"
#include "shub.h"

#define KT_SURV_USB 1U
#define KT_SURV_TICK 2U                     /* the slow tick (last 2 %'s keeper), 5 s */
#define KT_SURV_SHUB 3U                     /* 1.2-alpha28: the sensor hub's bring-up steps, then its 50 ms poll */
#define CHG 0x69U
#define R_INT_OK 0xb2U
#define R_DTLS0 0xb3U
#define R_DTLS1 0xb4U
#define R_DTLS2 0xb5U
#define R_CNFG00 0xb7U
#define R_CNFG02 0xb9U
#define R_CNFG11 0xc2U                      /* 1.2-alpha30: bits 6:0 VBYPSET, the boost's target (0 = 5.0 V, Samsung's
                                             * init; the step is not documented: the S9 #2 override measures it) */
#define R_CNFG06 0xbdU                      /* bits 3:2 CHGPROT: 0b11 unlocks writes to CNFG_01..03 (OTG_ILIM is in 02);
                                             * bits 1:0 WDTCLR (written 00 here: no watchdog effect) */
#define MODE_OTG 0x0aU

/* ---- small float maths (no libm in the kernel): enough for the sun, ~1e-9 ---------------------------------- */
#define PI 3.14159265358979323846
static double fmodp(double x, double m) { double r = x - m * (double)(int64_t)(x / m); return r < 0 ? r + m : r; }
static double s_sin(double x)
{
    x = fmodp(x + PI, 2 * PI) - PI;          /* -pi .. pi */
    double x2 = x * x, t = x, s = x;
    for (int k = 1; k < 12; ++k) {
        t *= -x2 / (double)((2 * k) * (2 * k + 1));
        s += t;
    }
    return s;
}
static double s_cos(double x) { return s_sin(x + PI / 2); }
static double s_sqrt(double x) { return x > 0 ? __builtin_sqrt(x) : 0; }
static double s_atan(double x)
{
    int neg = x < 0, inv = 0;
    if (neg) x = -x;
    if (x > 1) { x = 1 / x; inv = 1; }
    x = x / (1 + s_sqrt(1 + x * x));         /* halve the angle twice: |x| <= tan(pi/16) */
    x = x / (1 + s_sqrt(1 + x * x));
    double x2 = x * x, t = x, s = x;
    for (int k = 1; k < 14; ++k) {
        t *= -x2;
        s += t / (double)(2 * k + 1);
    }
    s *= 4;
    if (inv) s = PI / 2 - s;
    return neg ? -s : s;
}
static double s_acos(double x)
{
    if (x >= 1) return 0;
    if (x <= -1) return PI;
    double a = s_atan(s_sqrt(1 - x * x) / (x < 0 ? -x : x));
    return x < 0 ? PI - a : a;
}
#define RAD (PI / 180.0)

/* ---- the sun (the almanac's algorithm, zenith 90.833: the upper limb with refraction; ~1-2 min) ------------- */
static int doy(int y, int mo, int d)
{
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return cum[mo - 1] + d + (leap && mo > 2);
}

/* UT hours of the event, or 99 (never up) / -99 (never down) */
static double sun_ut(double lat, double lon, int n, int rising)
{
    double lh = lon / 15.0, t = n + ((rising ? 6.0 : 18.0) - lh) / 24.0;
    double M = 0.9856 * t - 3.289;
    double L = fmodp(M + 1.916 * s_sin(M * RAD) + 0.020 * s_sin(2 * M * RAD) + 282.634, 360.0);
    double RA = fmodp(s_atan(0.91764 * s_sin(L * RAD) / s_cos(L * RAD)) / RAD, 360.0);
    double Lq = (double)((int)(L / 90.0)) * 90.0, Rq = (double)((int)(RA / 90.0)) * 90.0;
    RA = (RA + (Lq - Rq)) / 15.0;
    double sd = 0.39782 * s_sin(L * RAD), cd = s_cos(s_atan(sd / s_sqrt(1 - sd * sd)));
    double ch = (s_cos(90.833 * RAD) - sd * s_sin(lat * RAD)) / (cd * s_cos(lat * RAD));
    if (ch > 1) return 99;
    if (ch < -1) return -99;
    double H = (rising ? 360.0 - s_acos(ch) / RAD : s_acos(ch) / RAD) / 15.0;
    double T = H + RA - 0.06571 * t - 6.622;
    return fmodp(T - lh, 24.0);
}

int surv_sun(int32_t lat_x100, int32_t lon_x100, int y, int mo, int d, int off_min, int *rise, int *set)
{
    if (lat_x100 < -9000 || lat_x100 > 9000 || lon_x100 < -18000 || lon_x100 > 18000 || y < 2000 || y > 2200 ||
        mo < 1 || mo > 12 || d < 1 || d > 31)
        return -1;
    double lat = lat_x100 / 100.0, lon = lon_x100 / 100.0;
    int n = doy(y, mo, d);
    double r = sun_ut(lat, lon, n, 1), s = sun_ut(lat, lon, n, 0);
    if (r == 99 || s == 99)
        return 2;
    if (r == -99 || s == -99)
        return 1;
    *rise = (int)fmodp(r * 60.0 + off_min + 0.5, 1440.0);
    *set = (int)fmodp(s * 60.0 + off_min + 0.5, 1440.0);
    return 0;
}

int32_t surv_lat_x100 = SURV_NOLOC, surv_lon_x100 = SURV_NOLOC;

int surv_sun_today(int *rise, int *set)
{
    struct sbos_time t;
    if (surv_lat_x100 == SURV_NOLOC || surv_lon_x100 == SURV_NOLOC)
        return -2;
    if (!clock_now(&t) || !t.utc)
        return -3;
    return surv_sun(surv_lat_x100, surv_lon_x100, t.y, t.mo, t.d, t.off_min, rise, set);
}

/* ---- Morse SOS: ... --- ... then a word gap; 34 units of 200 ms (6.8 s) ----------------------------------- */
uint32_t surv_sos_opts = SURV_SOS_TORCH | SURV_SOS_VIB;
static const uint8_t morse[] = {              /* on / off units in turn, starting lit */
    1, 1, 1, 1, 1, 3,                         /* S, the letter gap */
    3, 1, 3, 1, 3, 3,                         /* O */
    1, 1, 1, 1, 1, 7,                         /* S, the word gap */
};

uint32_t surv_morse_cycle_ms(void)
{
    uint32_t u = 0;
    for (unsigned i = 0; i < sizeof(morse); ++i)
        u += morse[i];
    return u * SURV_MORSE_UNIT_MS;
}

int surv_morse_at(uint32_t ms, uint32_t *next_ms)
{
    uint32_t cyc = surv_morse_cycle_ms(), base = ms - ms % cyc, at = 0;
    ms %= cyc;
    for (unsigned i = 0; i < sizeof(morse); ++i) {
        uint32_t end = at + morse[i] * SURV_MORSE_UNIT_MS;
        if (ms < end) {
            if (next_ms)
                *next_ms = base + end;
            return !(i & 1U);
        }
        at = end;
    }
    if (next_ms)
        *next_ms = base + cyc;
    return 0;
}

static int last2, saver_user, saver_applied;   /* extreme power saving and last 2 % (below) */

/* ---- USB power out ------------------------------------------------------------------------------------------ */
const char *const surv_usb_guide[] = {
    "5 V from the USB-C port, at most 1.5 A (the charger chip's own limit), 10 s a burst.",
    "To start a fire: a USB-C cable or adapter with bare ends, then fine steel wool (0000) or a thin wire on dry tinder.",
    "Touch both ends to the steel wool at once; it glows. Keep the phone and the cable away from the flame.",
    "Never short the phone's port directly and never put metal into it.",
    "Unplug any charger or computer first. The phone stops by itself on any fault, when the battery is hot or low.",
    0,
};

static struct {
    int state;
    uint64_t t_on, t_cool;                  /* the burst's start; the cooldown's end */
    uint8_t cnfg00, cnfg02;                 /* as found: written back at the stop */
    uint8_t cnfg06;                         /* 1.2-alpha29: CHGPROT as found (fire-1: OTG_ILIM stayed 900 mA locked) */
    int prot_written;                       /* CHGPROT unlocked by us: put back at the stop */
    int written;                            /* CNFG_00 holds OTG: it must go back on every path */
    int base_ma, have_base;
    int ma, mv, est_ma, vlo, vhi, bat_c10, ilim_ma;
    uint8_t byp, chg;
    unsigned low_ticks, back_fail;
    int vbus_seen;                          /* VBADC showed 4.5-5.5 V in this burst: the short check trusts it */
    /* 1.2-alpha29: Extreme fire mode (continuous while held; docs/USB-FIRE-RESEARCH.md) */
    int fire;                               /* this session is fire mode */
    uint64_t t_hold;                        /* the screen's last keep-alive (the finger) */
    uint64_t t_pause;                       /* resting (output off) until then: the chip dropped OTG, or the duty cap */
    uint64_t t_duty;                        /* the duty cap's current on-phase start */
    unsigned trips, resets, sag_ticks;      /* the chip's current-limit events; OTG dropped by the chip; low-voltage */
    int paused, bright_was;
    int usb_c10, chg_c10;                   /* the USB connector's and the charger's thermistors (ADC 1, 3) */
    int fire_min_left;
    uint32_t bursts, events;
    int refused;
    const char *why;
    /* 1.2-alpha30: the fire starter (fire == 2; survival.h surv_fs_start) */
    uint32_t fs_pulse_ms;
    int fs_repeat, fs_ilim_code;            /* the mode; OTG_ILIM code 1 / 2 / 3 = 900 / 1200 / 1500 mA */
    int fs_out, fs_n, fs_max;               /* the output on now (a pulse); pulses done; the most in this hold */
    uint64_t t_phase, t_fs_temp, t_fs_log;  /* the pulse's (or the rest's) start; the last temperature read, log */
    uint64_t fs_on_ms;                      /* the time on in this hold: the cooldown is 5 s + 3x it */
    int fs_sum, fs_cnt, fs_peak, fs_vmin_cur, fs_sag_cur;   /* this pulse, as it runs */
    int fs_avg, fs_peak_last, fs_w10, fs_vmin, fs_sag;      /* the last pulse, finished */
    int fs_why;
    /* 1.2-alpha30, S9 #2's override (the user's test phone, attended; docs/USB-FIRE-STARTER.md 5): 1 fast re-arm
     * (the chip's limit or switch-off answered by switching OTG straight back on, not after its own 300 ms), 2 the
     * same plus a higher boost target (VBYPSET); pulses <= 1 s. The battery pack, the fuel gauge, B2SOVRC and the
     * PMIC's thermal shutdown stay as they are. */
    int fs_ov, fs_vbyp;
    /* run 1 (11-16): the gauge's VBYP (the boost's output, before the chip's OTG switch) and its input-current word,
     * so a pulse's real volts are logged; the cell's power; the status lines a pulse logs (the ring is 16 KiB) */
    int fs_vb_sum, fs_vb_cnt, fs_vb_last, fs_vb_mean, fs_cellw_sum, fs_cellw10, fs_iin_raw;
    unsigned fs_stat_lines;
    /* the steel-wool preset (fs_tol): the chip's own limit-and-retry (30 ms on, ~300 ms off) is let run, as designed;
     * the port's band per reading for the load / chip split */
    int fs_tol, fs_vmid_sum, fs_vmid_cnt, fs_vmid, fs_load_w10, fs_chip_w10;
    uint8_t cnfg11;
    int vbyp_written;
    unsigned fs_rearms, fs_rearms_pulse, fs_btrips;
} U;
static struct sb_auth au;
static void (*prev_before_reset)(void);

static int chg_rd(uint8_t r, uint8_t *v) { return sb_chg_read(r, v); }
/* CNFG_02 with OTG_ILIM at this session's step: the top one (1.5 A) except the fire starter's own choice */
static uint8_t ilim_cnfg02(void)
{
    unsigned code = U.fire == 2 && U.fs_ilim_code >= 1 && U.fs_ilim_code <= 3 ? (unsigned)U.fs_ilim_code : 3U;
    return (uint8_t)((U.cnfg02 & 0x3fU) | code << 6);
}

/* a tier-1 write of one charger register under its own authorisation (the hold-to-arm was its held step) */
static int chg_wr(uint8_t reg, uint8_t val)
{
    sb_auth_init(&au, SB_SP_MAX, 0, (uint64_t)CHG << 8 | reg, 1, SB_TIER1, 0, "survival: USB power out");
    sb_auth_step(&au, SB_STEP_WARN);        /* the guidance on screen ... */
    sb_auth_step(&au, SB_STEP_HELD);        /* ... and the hold */
    int ok = sb_gate_open(&au) && sb_max_write(CHG, reg, val, SB_TIER1);
    sb_gate_close();
    uint8_t back = 0;
    return ok && chg_rd(reg, &back) && back == val;
}

static int bat_temp_c10(void)
{
    int r = sb_adc_filtered(4, 0, 0, 0);
    return r >= 0 ? sb_therm_convert(1, r) : -1000;
}

static int limit_c10(void)
{
    int l = heat_surv_bat_c10;              /* the user's Survival battery limit (55 / 58 / 60 / 65 C) */
    return l < 450 ? 450 : l > 650 ? 650 : l;
}

static void vbus_read(void)
{
    uint8_t s1 = 0;
    U.vlo = U.vhi = -1;
    if (sb_usbc_read(0x06, &s1)) {
        /* 1.2-alpha29: VBADC is bits 7:4 (4 bits, as the MAX77843's), 1 V steps: survival-41d5137 on the phone read
         * USBC_STATUS1 0x2F on the Mac's 5 V cable: code 2 = 4.5-5.5 V (bits 7:3 had given 7.5-8.5 V) */
        unsigned c = (s1 >> 4) & 0xfU;
        U.vlo = c ? 2500 + 1000 * (int)c : 0;
        U.vhi = 3500 + 1000 * (int)c;
    }
}

static int input_present(void)
{
    uint8_t ok = 0, bc = 0;
    if (chg_rd(R_INT_OK, &ok) && (ok & 0x40U))
        return 1;
    return sb_usbc_read(0x08, &bc) && (bc & 0x80U);
}

/* 1.2-alpha29: the USB connector's (ADC channel 1) and the charger's (channel 3) thermistors, the battery's table
 * (libsb adc.c sb_therms_read; clamped at 90 C): -1000 unread */
static int therm_c10(unsigned ch)
{
    int r = sb_adc_filtered(ch, 0, 0, 0);
    return r >= 0 ? sb_therm_convert(1, r) : -1000;
}
#define USB_CONN_STOP_C10 700               /* stock stops charging at 80 C on this one; we stop the output at 70 */
#define CHG_STOP_C10 750
#define WARM_MARGIN_C10 50                  /* within 5 C of a stop: the duty cap (fire mode) */
#define FIRE_MAX_MS 120000U
#define FIRE_COOL_MS 15000U
#define FIRE_HOLD_MS 1500U                  /* the finger's keep-alive: released after this */
#define FIRE_SAG_MV 3200                    /* the battery under load: 3 readings (300 ms) below it stop */
#define FIRE_REARM_MS 2000U                 /* the chip dropped OTG by itself: rest, then once more (3 at most) */
#define FIRE_DUTY_ON_MS 2000U               /* warm: 2 s on, 2 s off */
/* 1.2-alpha30: the fire starter (survival.h surv_fs_start; docs/USB-FIRE-STARTER.md) */
#define FS_TICK_MS 20U                      /* every check 50 times a second while a hold runs */
#define FS_CONN_STOP_C10 600                /* stricter than the other modes: the USB connector 60 C, */
#define FS_CHG_STOP_C10 650                 /* the charger 65 C, the battery 45 C (SURV_FS_BAT_STOP_C10) */
#define FS_VBUS_GRACE_MS 100U               /* the port's reading settles after the boost starts */
#define FS_EST_FROM_MS 200U                 /* the fuel gauge's current lags: the mean from 200 ms into a pulse */
#define FS_COOL_BASE_MS 5000U
#define FS_COOL_MAX_MS 60000U
#define FS_COOL_FAULT_MS 30000U
static void fs_pulse_begin(void);
static void fs_keep(const char *line);
static void fs_pulse_end(void);
static void fs_tick(uint64_t now);

/* 1.2-alpha29: OTG_ILIM (CNFG_02) only takes a write with CHGPROT unlocked (CNFG_06 bits 3:2 = 0b11; Samsung's
 * driver unlocks it at init and keeps it so; docs/USB-FIRE-RESEARCH.md 2). A write guard, not a protection: the
 * chip's current limit, short handling and thermal shutdown stay as they are. 1 unlocked (or already) */
static int chgprot_unlock(void)
{
    uint8_t c6 = 0;
    if (!chg_rd(R_CNFG06, &c6))
        return 0;
    if (((c6 >> 2) & 3U) == 3U)
        return 1;
    U.cnfg06 = c6;
    sb_auth_init(&au, SB_SP_MAX, 0, (uint64_t)CHG << 8 | R_CNFG06, 1, SB_TIER1, 0, "survival: USB power out (CHGPROT)");
    sb_auth_step(&au, SB_STEP_WARN);
    sb_auth_step(&au, SB_STEP_HELD);
    int ok = sb_gate_open(&au) && sb_max_write(CHG, R_CNFG06, (uint8_t)((c6 & ~0x0fU) | 0x0cU), SB_TIER1);
    sb_gate_close();
    uint8_t back = 0;
    ok = ok && chg_rd(R_CNFG06, &back) && ((back >> 2) & 3U) == 3U;
    U.prot_written = ok;
    sb_log(ok ? "survival: usb out: CHGPROT unlocked for OTG_ILIM (put back at the stop)" :
                "survival: usb out: CHGPROT did not unlock: OTG_ILIM stays as the chip has it");
    return ok;
}

static void chgprot_back(void)
{
    if (!U.prot_written)
        return;
    uint8_t c6 = 0;
    if (!chg_rd(R_CNFG06, &c6))
        return;
    sb_auth_init(&au, SB_SP_MAX, 0, (uint64_t)CHG << 8 | R_CNFG06, 1, SB_TIER1, 0, "survival: USB power out (CHGPROT)");
    sb_auth_step(&au, SB_STEP_WARN);
    sb_auth_step(&au, SB_STEP_HELD);
    int ok = sb_gate_open(&au) && sb_max_write(CHG, R_CNFG06, (uint8_t)((c6 & ~0x0fU) | (U.cnfg06 & 0x0cU)), SB_TIER1);
    sb_gate_close();
    if (ok)
        U.prot_written = 0;
}

static void usb_back(void)
{
    if (!U.written)
        return;
    int a = chg_wr(R_CNFG00, U.cnfg00);
    int b = chg_wr(R_CNFG02, U.cnfg02);
    if (a && U.vbyp_written) {              /* the boost's target back first after the output is off */
        if (chg_wr(R_CNFG11, U.cnfg11)) {
            U.vbyp_written = 0;
            sb_log("survival: fs: VBYPSET back as it was");
        } else {
            b = 0;
        }
    }
    if (a && b) {
        chgprot_back();                     /* after CNFG_02: the guard as it was */
        U.written = 0;
        U.back_fail = 0;
        sb_log("survival: usb out: the charger is back as it was (mode, current limit, CHGPROT)");
    } else if (!U.back_fail++) {
        sb_log("survival: usb out: the charger did NOT take its old mode back: retried every 100 ms");
    }
}

static void before_reset(void)
{
    if (U.written)
        usb_back();
    chgprot_back();
    shub_leave();                           /* 1.2-alpha28: the hub in standby, its pins and clocks as found */
    if (prev_before_reset)
        prev_before_reset();
}

void surv_usb_stop(const char *why)
{
    if (U.state != SURV_USB_ON && !U.written)
        return;
    usb_back();
    int fault = why && why[0] == '!';
    U.why = fault ? why + 1 : why;
    ++U.events;
    U.state = SURV_USB_COOL;
    if (U.fire == 2) {                      /* the fire starter: 5 s + 3x the time on, 30 s after a fault */
        if (U.fs_out) {
            U.fs_on_ms += sb_ms() - U.t_phase;
            fs_pulse_end();
        }
        uint64_t c = fault ? FS_COOL_FAULT_MS : FS_COOL_BASE_MS + 3U * U.fs_on_ms;
        U.t_cool = sb_ms() + (c > FS_COOL_MAX_MS ? FS_COOL_MAX_MS : c);
        if (U.fs_why == SURV_FS_WHY_NONE)
            U.fs_why = fault ? SURV_FS_WHY_FAULT : SURV_FS_WHY_STOP;
    } else {
        U.t_cool = sb_ms() + (fault ? SURV_USB_COOL_FAULT_MS : U.fire ? FIRE_COOL_MS : SURV_USB_COOL_MS);
    }
    if (U.fire == 1) {
        if (U.bright_was > 0)
            (void)panel_brightness(U.bright_was);
        struct ub v;
        ub_init(&v);
        ub_s(&v, "survival: fire: the chip's current-limit events ");
        ub_dec(&v, U.trips);
        ub_s(&v, ", OTG dropped by the chip ");
        ub_dec(&v, U.resets);
        sb_log(v.b);
    }
    kt_set(KT_SURV, KT_SURV_USB, sb_ms(), U.written ? 100 : 1000, 1);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "survival: usb out: off after ");
    ub_dec(&u, (sb_ms() - U.t_on) / 100U);
    ub_s(&u, "00 ms: ");
    ub_s(&u, U.why ? U.why : "stopped");
    sb_log(u.b);
    if (fault)
        notif_post("survival", "USB power out stopped", U.why);
}

static int refuse(const char *why)
{
    U.why = why;
    U.refused = 1;
    U.fs_why = SURV_FS_WHY_REFUSED;
    ++U.events;
    if (U.written)
        kt_set(KT_SURV, KT_SURV_USB, sb_ms(), 100, 1);    /* the charger's old mode is retried every 100 ms */
    sb_logf("survival: usb out: refused: ", why);
    notif_post("survival", "USB power out did not start", why);
    return -1;
}

static int usb_start(int fire)
{
    uint64_t now = sb_ms();
    if (U.state == SURV_USB_ON)
        return U.fire == fire ? 0 : refuse("the other USB power mode is running: stop it first");
    if (U.written)
        return refuse("the charger is not back from the last burst yet");
    if (U.state == SURV_USB_COOL && now < U.t_cool) {
        int r = refuse("cooling down after the last burst");
        U.fs_why = SURV_FS_WHY_COOL;
        return r;
    }
    U.state = SURV_USB_OFF;
    if (saver_applied && !fire)
        return refuse("Extreme power saving (or Last 2 %) is on");
    if (fire && sbos_selftest_enabled)
        return refuse("never in a self-test image");
    if (heater_running_ui())
        return refuse("Heat is running (both draw hard on the battery)");
    if (fire == 2 && !kui_finger_down()) {  /* the fire starter fires only under a finger, never from the keys */
        int r = refuse("keep a finger on the fire bar, or hold Power, until it fires");
        U.fs_why = SURV_FS_WHY_FINGER;
        return r;
    }
    if (!sb_max_init())
        return refuse("the charger chip does not answer");
    if (input_present())
        return refuse("something is powering the USB-C port: unplug the charger or computer first");
    struct sbos_battery b;
    battery_get(&b);
    U.bat_c10 = bat_temp_c10();
    if (!b.ok || b.pct < (fire == 1 ? 1 : 5))
        return refuse(fire == 1 ? "the battery is empty (below 1 %)" : "the battery is below 5 %");
    if (b.mv && b.mv < (fire == 1 ? 3300 : 3500))
        return refuse(fire == 1 ? "the battery is below 3.3 V" : "the battery is below 3.5 V");
    U.usb_c10 = therm_c10(1);
    U.chg_c10 = therm_c10(3);
    if (U.usb_c10 > -1000 && U.usb_c10 >= (fire == 2 ? FS_CONN_STOP_C10 : USB_CONN_STOP_C10) - WARM_MARGIN_C10)
        return refuse("the USB connector is hot: let it cool");
    if (U.chg_c10 > -1000 && U.chg_c10 >= (fire == 2 ? FS_CHG_STOP_C10 : CHG_STOP_C10) - WARM_MARGIN_C10)
        return refuse("the charger is hot: let it cool");
    if (U.bat_c10 <= -1000)
        return refuse("the battery temperature cannot be read");
    if (fire == 2 && U.bat_c10 >= SURV_FS_BAT_START_C10)
        return refuse("the battery is at 43 C or more: let it cool (the fire starter stops at 45 C)");
    if (U.bat_c10 >= limit_c10() - 20)
        return refuse("the battery is near its Survival limit: let it cool");
    if (!chg_rd(R_CNFG00, &U.cnfg00) || !chg_rd(R_CNFG02, &U.cnfg02))
        return refuse("the charger's settings cannot be read");
    if ((U.cnfg00 & 0x0fU) == MODE_OTG)
        return refuse("the charger is already in OTG mode (another user of the port)");
    /* the battery's draw before: the estimate's baseline */
    int sum = 0, n = 0, mv = 0, ma = 0;
    for (int i = 0; i < 3; ++i)
        if (sb_bat_fast(&mv, &ma)) {
            sum += ma;
            ++n;
        }
    U.have_base = n > 0;
    U.base_ma = n ? sum / n : 0;
    /* the chip's own top current limit, then the boost on */
    U.written = 1;                          /* from here every path writes both back */
    U.fire = fire;                          /* ilim_cnfg02 reads it */
    (void)chgprot_unlock();
    if (!chg_wr(R_CNFG02, ilim_cnfg02()))
        sb_log("survival: usb out: OTG_ILIM did not read back (write-protected?): the chip's limit as it is");
    uint8_t c2 = 0;
    U.ilim_ma = chg_rd(R_CNFG02, &c2) ? (int[]){ 500, 900, 1200, 1500 }[c2 >> 6] : -1;
    if (fire == 2) {                        /* the charger's registers as they are (read only; CHG_INT 0xB0 skipped:
                                             * clear-on-read) */
        struct ub r;
        ub_init(&r);
        ub_s(&r, "survival: fs: charger 0xB1..0xC3:");
        for (uint8_t a = 0xb1U; a <= 0xc3U; ++a) {
            uint8_t v = 0;
            ub_c(&r, ' ');
            if (chg_rd(a, &v))
                ub_hex(&r, v, 2);
            else
                ub_s(&r, "??");
        }
        sb_log(r.b);
    }
    if (fire == 2 && U.fs_ov == 2) {        /* S9 #2's override: a higher boost target */
        uint8_t c11 = 0;
        if (!chg_rd(R_CNFG11, &c11)) {
            usb_back();
            return refuse("the boost's voltage register cannot be read");
        }
        U.cnfg11 = c11;
        U.vbyp_written = 1;
        uint8_t want = (uint8_t)((c11 & 0x80U) | (U.fs_vbyp & 0x7f));
        int ok = chg_wr(R_CNFG11, want);
        struct ub v;
        ub_init(&v);
        ub_s(&v, "survival: fs: override: VBYPSET ");
        ub_hex(&v, c11 & 0x7fU, 2);
        ub_s(&v, ok ? " -> " : " -> (did NOT read back) ");
        ub_hex(&v, want & 0x7fU, 2);
        sb_log(v.b);
    }
    if (!chg_wr(R_CNFG00, (uint8_t)((U.cnfg00 & ~0x0fU) | MODE_OTG))) {
        usb_back();
        return refuse("the charger did not take OTG mode");
    }
    U.state = SURV_USB_ON;
    U.refused = 0;
    U.t_on = now;
    U.low_ticks = 0;
    U.vbus_seen = 0;
    U.fire = fire;
    U.t_hold = U.t_duty = now;
    U.t_pause = 0;
    U.paused = 0;
    U.trips = U.resets = U.sag_ticks = 0;
    U.fire_min_left = -1;
    if (fire == 2) {                        /* the fire starter: the first pulse starts now */
        U.fs_out = 1;
        U.t_phase = U.t_fs_temp = U.t_fs_log = now;
        U.fs_n = 0;
        U.fs_on_ms = 0;
        U.fs_why = SURV_FS_WHY_NONE;
        U.fs_avg = U.fs_peak_last = U.fs_w10 = 0;
        U.fs_vmin = -1;
        U.fs_sag = 0;
        fs_pulse_begin();
    }
    if (fire == 1) {                        /* everything else off so the battery's energy goes to the port */
        if (wifi_on())
            (void)wifi_power(0);
        if (kmodem_on())
            kmodem_stop("Extreme fire mode");
        if (guest_owns_m3())
            (void)guest_stop();
        surv_sensors_on(0);
        U.bright_was = panel_bright_level;
        (void)panel_brightness(1);
        sb_log("survival: fire: Extreme fire mode: continuous while held (at most 2 min), the chip's own 1.5 A limit "
               "and protections, radios off, the screen dim");
    }
    U.est_ma = -1;
    U.why = 0;
    ++U.bursts;
    kt_set(KT_SURV, KT_SURV_USB, now, fire == 2 ? FS_TICK_MS : 100, 1);
    struct ub u;
    ub_init(&u);
    ub_s(&u, fire == 2 ? "survival: fs: ON (fire starter " : "survival: usb out: ON (burst ");
    ub_dec(&u, U.bursts);
    ub_s(&u, ", OTG limit ");
    ub_dec(&u, (uint64_t)(U.ilim_ma > 0 ? U.ilim_ma : 0));
    ub_s(&u, " mA, CNFG_00 was ");
    ub_hex(&u, U.cnfg00, 2);
    ub_s(&u, ", battery ");
    ub_dec(&u, (uint64_t)(U.bat_c10 / 10));
    ub_s(&u, " C, base ");
    ub_dec(&u, (uint64_t)(U.base_ma < 0 ? -U.base_ma : U.base_ma));
    ub_s(&u, " mA out of it)");
    if (fire == 2) {
        ub_s(&u, "; pulses of ");
        ub_dec(&u, U.fs_pulse_ms);
        ub_s(&u, U.fs_repeat ? " ms, repeated (at most " : " ms, once (");
        ub_dec(&u, (uint64_t)U.fs_max);
        ub_s(&u, "), USB connector ");
        ub_dec(&u, (uint64_t)(U.usb_c10 > 0 ? U.usb_c10 / 10 : 0));
        ub_s(&u, " C, charger ");
        ub_dec(&u, (uint64_t)(U.chg_c10 > 0 ? U.chg_c10 / 10 : 0));
        ub_s(&u, " C");
    }
    sb_log(u.b);
    return 0;
}

int surv_usb_start(void)
{
    return usb_start(0);
}

/* 1.2-alpha29: Extreme fire mode: after the UI's stronger confirmation; runs while surv_usb_hold() comes */
int surv_usb_fire_start(void)
{
    return usb_start(1);
}

void surv_usb_hold(void)
{
    if (U.state == SURV_USB_ON && U.fire == 1) {
        U.t_hold = sb_ms();
        power_input();                      /* the finger on the screen: it stays on */
    }
}

int surv_usb_active(void)
{
    return U.state == SURV_USB_ON || U.written;
}

/* fire mode: the output off for a rest (the chip dropped OTG, or the duty cap) and on again */
static void fire_pause(uint64_t ms, const char *why)
{
    if (!U.paused)
        (void)chg_wr(R_CNFG00, U.cnfg00);   /* the charger's old mode: the output off */
    U.paused = 1;
    U.t_pause = sb_ms() + ms;
    sb_logf("survival: fire: resting: ", why);
}

static int fire_resume(void)
{
    if (!chg_wr(R_CNFG02, ilim_cnfg02()))
        sb_log("survival: fire: OTG_ILIM did not read back on the re-arm (the chip's limit as it is)");
    if (!chg_wr(R_CNFG00, (uint8_t)((U.cnfg00 & ~0x0fU) | MODE_OTG)))
        return 0;
    U.paused = 0;
    U.t_duty = sb_ms();
    return 1;
}

/* ---- 1.2-alpha30: the fire starter ------------------------------------------------------------------------ */
static void fs_stop(int code, const char *why)
{
    U.fs_why = code;
    surv_usb_stop(why);
}

static void fs_pulse_begin(void)
{
    U.fs_rearms_pulse = 0;
    U.fs_vb_sum = U.fs_vb_cnt = U.fs_cellw_sum = 0;
    U.fs_vmid_sum = U.fs_vmid_cnt = 0;
    U.fs_stat_lines = 0;
    U.fs_sum = U.fs_cnt = U.fs_peak = 0;
    U.fs_vmin_cur = 99;
    U.fs_sag_cur = 99999;
}

/* a pulse ended (its time, a stop or a fault): its figures become "the last pulse" and go to the log */
static void fs_pulse_end(void)
{
    U.fs_out = 0;
    U.fs_avg = U.fs_cnt ? U.fs_sum / U.fs_cnt : -1;
    U.fs_peak_last = U.fs_cnt ? U.fs_peak : -1;
    U.fs_vb_mean = U.fs_vb_cnt ? U.fs_vb_sum / U.fs_vb_cnt : -1;
    U.fs_cellw10 = U.fs_cnt ? U.fs_cellw_sum / U.fs_cnt : 0;
    /* W out of the boost (mA x VBYP); the lead gets that minus the chip's switch drop (VBYP - VBUS) x I */
    U.fs_w10 = U.fs_avg > 0 ? (int)((int64_t)U.fs_avg * (U.fs_vb_mean > 3000 ? U.fs_vb_mean : 5100) / 100000) : 0;
    /* the split: into the load mA x the port's voltage (the middle of its 1 V band; below 3.5 V counted as 1.75 V),
     * the rest of the boost's output lost in the chip's OTG switch */
    U.fs_vmid = U.fs_vmid_cnt ? U.fs_vmid_sum / U.fs_vmid_cnt : -1;
    U.fs_load_w10 = U.fs_avg > 0 && U.fs_vmid > 0 ? (int)((int64_t)U.fs_avg * U.fs_vmid / 100000) : 0;
    U.fs_chip_w10 = U.fs_w10 > U.fs_load_w10 ? U.fs_w10 - U.fs_load_w10 : 0;
    U.fs_vmin = U.fs_vmin_cur == 99 ? -1 : U.fs_vmin_cur;
    U.fs_sag = U.fs_sag_cur == 99999 ? 0 : U.fs_sag_cur;
    ++U.fs_n;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "survival: fs: pulse ");
    ub_dec(&u, (uint64_t)U.fs_n);
    ub_s(&u, " (limit ");
    ub_dec(&u, (uint64_t)(U.ilim_ma > 0 ? U.ilim_ma : 0));
    ub_s(&u, " mA, ");
    ub_dec(&u, (sb_ms() - U.t_phase));
    ub_s(&u, " ms): mean ");
    ub_dec(&u, (uint64_t)(U.fs_avg > 0 ? U.fs_avg : 0));
    ub_s(&u, " mA, peak ");
    ub_dec(&u, (uint64_t)(U.fs_peak_last > 0 ? U.fs_peak_last : 0));
    ub_s(&u, " mA out of the port (");
    ub_dec(&u, (uint64_t)U.fs_cnt);
    ub_s(&u, " readings), about ");
    ub_dec(&u, (uint64_t)(U.fs_w10 / 10));
    ub_c(&u, '.');
    ub_dec(&u, (uint64_t)(U.fs_w10 % 10));
    ub_s(&u, " W out of the boost (VBYP mean ");
    ub_dec(&u, (uint64_t)(U.fs_vb_mean > 0 ? U.fs_vb_mean : 0));
    ub_s(&u, " mV), ");
    ub_dec(&u, (uint64_t)(U.fs_cellw10 / 10));
    ub_c(&u, '.');
    ub_dec(&u, (uint64_t)(U.fs_cellw10 % 10));
    ub_s(&u, " W from the cell; into the load about ");
    ub_dec(&u, (uint64_t)(U.fs_load_w10 / 10));
    ub_c(&u, '.');
    ub_dec(&u, (uint64_t)(U.fs_load_w10 % 10));
    ub_s(&u, " W, lost in the chip's switch about ");
    ub_dec(&u, (uint64_t)(U.fs_chip_w10 / 10));
    ub_c(&u, '.');
    ub_dec(&u, (uint64_t)(U.fs_chip_w10 % 10));
    ub_s(&u, " W (port band middle ");
    ub_dec(&u, (uint64_t)(U.fs_vmid > 0 ? U.fs_vmid : 0));
    ub_s(&u, " mV); VBUS code low ");
    ub_dec(&u, (uint64_t)(U.fs_vmin < 0 ? 99 : U.fs_vmin));
    ub_s(&u, "; battery low ");
    ub_dec(&u, (uint64_t)U.fs_sag);
    ub_s(&u, " mV, ");
    ub_dec(&u, (uint64_t)(U.bat_c10 > 0 ? U.bat_c10 / 10 : 0));
    ub_c(&u, '.');
    ub_dec(&u, (uint64_t)(U.bat_c10 > 0 ? U.bat_c10 % 10 : 0));
    ub_s(&u, " C; BYP_DTLS ");
    ub_hex(&u, U.byp, 1);
    if (U.fs_tol) {
        ub_s(&u, "; steel wool (the chip's own retry), limit events ");
        ub_dec(&u, U.trips);
    }
    if (U.fs_ov) {
        ub_s(&u, "; override ");
        ub_dec(&u, (uint64_t)U.fs_ov);
        ub_s(&u, ", re-arms ");
        ub_dec(&u, U.fs_rearms_pulse);
        ub_s(&u, ", limit events ");
        ub_dec(&u, U.trips);
        ub_s(&u, ", boost-limit events ");
        ub_dec(&u, U.fs_btrips);
    }
    sb_log(u.b);
    fs_keep(u.b);
}

static int fs_on(void)
{
    if (!chg_wr(R_CNFG02, ilim_cnfg02()))
        sb_log("survival: fs: OTG_ILIM did not read back (the chip's limit as it is)");
    if (!chg_wr(R_CNFG00, (uint8_t)((U.cnfg00 & ~0x0fU) | MODE_OTG)))
        return 0;
    U.fs_out = 1;
    U.t_phase = sb_ms();
    fs_pulse_begin();
    return 1;
}

static uint32_t fs_gap_ms(void) { return U.fs_pulse_ms > 1000U ? U.fs_pulse_ms : 1000U; }   /* 3 / 5 s: as long */

void surv_fs_release(void)
{
    if (U.state == SURV_USB_ON && U.fire == 2)
        fs_stop(SURV_FS_WHY_RELEASED, "released");
}

int surv_fs_start(uint32_t pulse_ms, int repeat, int ilim_ma)
{
    return surv_fs_start_ov(pulse_ms, repeat, ilim_ma, 0, 0);
}

int surv_fs_start_ov(uint32_t pulse_ms, int repeat, int ilim_ma, int ov, int vbyp)
{
    return surv_fs_start_x(pulse_ms, repeat, ilim_ma, ov, vbyp, 0);
}

int surv_fs_start_x(uint32_t pulse_ms, int repeat, int ilim_ma, int ov, int vbyp, int tol)
{
    if (tol && ov)
        return refuse("the steel-wool preset runs without the override");
    if (ov < 0 || ov > 2 || vbyp < 0 || vbyp > 0x7f)
        return refuse("the override's settings are out of range");
    if (ov && pulse_ms > SURV_FS_OV_MAX_MS)
        pulse_ms = SURV_FS_OV_MAX_MS;       /* overridden pulses: 1 s at most */
    if (pulse_ms != 500U && pulse_ms != 1000U && pulse_ms != 2000U && pulse_ms != 3000U && pulse_ms != 5000U &&
        !(ov && pulse_ms == SURV_FS_OV_MAX_MS))
        return refuse("the fire starter's pulse is 0.5, 1, 2, 3 or 5 s");
    if (ilim_ma != 900 && ilim_ma != 1200 && ilim_ma != 1500)
        return refuse("the fire starter's limit is 0.9, 1.2 or 1.5 A");
    if (U.state == SURV_USB_ON)
        return U.fire == 2 ? 0 : refuse("the other USB power mode is running: stop it first");
    U.fs_pulse_ms = pulse_ms;
    U.fs_repeat = repeat != 0;
    U.fs_max = repeat ? SURV_FS_MAX_PULSES : 1;
    U.fs_ilim_code = ilim_ma == 900 ? 1 : ilim_ma == 1200 ? 2 : 3;
    U.fs_ov = ov;
    U.fs_vbyp = vbyp;
    U.fs_tol = tol != 0;
    U.fs_rearms = U.fs_btrips = 0;
    return usb_start(2);
}

/* run 1 (11-16): the pmsg ring kept only the last two pulses: every pulse's summary also goes into the blob
 * "fslog" (the state slot, saved 3 s later; the newest ~6 KiB, whole lines) */
static char fslog[6144];
static uint32_t fslog_n;
static void fs_keep(const char *line)
{
    uint32_t n = (uint32_t)strlen(line) + 1U;
    if (n >= sizeof(fslog))
        return;
    while (fslog_n + n > sizeof(fslog)) {   /* drop the oldest line */
        uint32_t k = 0;
        while (k < fslog_n && fslog[k] != '\n')
            ++k;
        k = k < fslog_n ? k + 1U : fslog_n;
        memmove(fslog, fslog + k, fslog_n - k);
        fslog_n -= k;
    }
    memcpy(fslog + fslog_n, line, n - 1U);
    fslog[fslog_n + n - 1U] = '\n';
    fslog_n += n;
    if (prefs_blob_put("fslog", fslog, fslog_n) >= 0)
        settings_changed_soon(3000);
}

/* S9 #2's override: OTG off and straight back on (the chip's own rest is ~300 ms). 1 on again, 0 failed */
static int fs_rearm(const char *what)
{
    ++U.fs_rearms;
    if (++U.fs_rearms_pulse <= 3U || U.fs_rearms_pulse % 10U == 0U) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "survival: fs: override: re-arm ");
        ub_dec(&u, U.fs_rearms_pulse);
        ub_s(&u, " in this pulse after ");
        ub_s(&u, what);
        ub_s(&u, " at +");
        ub_dec(&u, sb_ms() - U.t_phase);
        ub_s(&u, " ms");
        sb_log(u.b);
    }
    if (!chg_wr(R_CNFG00, U.cnfg00))
        return 0;
    return chg_wr(R_CNFG00, (uint8_t)((U.cnfg00 & ~0x0fU) | MODE_OTG));
}

/* every 20 ms while a hold runs: a pulse's checks, its end, the rest between pulses */
static void fs_tick(uint64_t now)
{
    if (!kui_finger_down()) {               /* ui.c calls surv_fs_release at the lift; this is the backstop */
        fs_stop(SURV_FS_WHY_RELEASED, "released");
        return;
    }
    if (!K.screen_on) {
        fs_stop(SURV_FS_WHY_RELEASED, "the screen went off");
        return;
    }
    uint64_t in = now - U.t_phase;
    if (!U.fs_out) {                        /* resting between pulses: the output is off */
        if (in < fs_gap_ms())
            return;
        if (!fs_on()) {
            fs_stop(SURV_FS_WHY_FAULT, "!the charger did not take OTG mode again");
            return;
        }
        return;
    }
    uint8_t c0 = 0, d0 = 0, d1 = 0, d2 = 0;
    if (!(chg_rd(R_CNFG00, &c0) && chg_rd(R_DTLS0, &d0) && chg_rd(R_DTLS1, &d1) && chg_rd(R_DTLS2, &d2))) {
        fs_stop(SURV_FS_WHY_FAULT, "!the charger chip stopped answering");
        return;
    }
    if (((d2 & 0x0fU) != U.byp || (d1 & 0x7fU) != U.chg) && ++U.fs_stat_lines <= 4U) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "survival: fs: the chip's status: BYP_DTLS ");
        ub_hex(&u, d2 & 0x0fU, 1);
        ub_s(&u, " CHG_DTLS ");
        ub_hex(&u, d1 & 0x0fU, 1);
        ub_s(&u, " BAT_DTLS ");
        ub_hex(&u, (d1 >> 4) & 7U, 1);
        ub_s(&u, " CHGIN_DTLS ");
        ub_hex(&u, (d0 >> 5) & 3U, 1);
        ub_s(&u, " at ");
        ub_dec(&u, in);
        ub_s(&u, " ms");
        sb_log(u.b);
    }
    U.byp = d2 & 0x0fU;
    U.chg = d1 & 0x7fU;
    if (U.byp & 1U) {                       /* OTGILIM: the chip's output switch at its current limit */
        ++U.trips;
        if (!U.fs_ov && !U.fs_tol) {
            fs_stop(SURV_FS_WHY_ILIM, "!the chip hit its current limit: the lead draws too much, move the contacts "
                                      "apart (or pick a higher limit)");
            return;
        }
    }
    if (U.byp & 0x0eU) {                    /* the boost's own limit, or a flag we do not know: off */
        if (U.byp & 2U)
            ++U.fs_btrips;
        if (!U.fs_ov && !(U.fs_tol && (U.byp & 0x0eU) == 2U)) {
            fs_stop(SURV_FS_WHY_FAULT, U.byp & 2U ? "!the chip's boost hit its current limit" :
                                                    "!the chip flagged its OTG path (BYP_DTLS)");
            return;
        }
    }
    if ((U.chg & 0x0fU) == 0x0aU) {
        fs_stop(SURV_FS_WHY_HOT, "!the charger chip shut down from heat (its own protection)");
        return;
    }
    if (((U.chg >> 4) & 7U) == 6U) {
        fs_stop(SURV_FS_WHY_FAULT, "!the chip flagged battery overcurrent");
        return;
    }
    if ((c0 & 0x0fU) != MODE_OTG && !U.fs_ov) {
        fs_stop(SURV_FS_WHY_FAULT, "!the charger switched its output off by itself (its protection)");
        return;
    }
    if (U.fs_ov && ((U.byp & 0x0fU) || (c0 & 0x0fU) != MODE_OTG)) {
        if (U.fs_rearms_pulse >= SURV_FS_OV_MAX_REARMS) {
            fs_stop(SURV_FS_WHY_FAULT, "!override: 50 re-arms in one pulse: off (a dead short?)");
            return;
        }
        if (!fs_rearm((c0 & 0x0fU) != MODE_OTG ? "the chip switched OTG off" :
                      (U.byp & 1U) ? "its current limit" : "its boost limit")) {
            fs_stop(SURV_FS_WHY_FAULT, "!override: the charger did not take OTG mode again");
            return;
        }
    }
    vbus_read();
    int code = U.vhi < 0 ? -1 : (U.vhi - 3500) / 1000;
    if (in >= FS_VBUS_GRACE_MS && code >= 0) {
        if (code < U.fs_vmin_cur)
            U.fs_vmin_cur = code;
        int vmid = code == 0 ? 1750 : 3000 + 1000 * code;
        U.fs_vmid_sum += vmid;
        ++U.fs_vmid_cnt;
        if (code == 0 && !U.fs_ov && !U.fs_tol) {   /* below 3.5 V: collapsed (the override / steel wool expect dips) */
            fs_stop(SURV_FS_WHY_VBUS, "!the port's voltage collapsed below 3.5 V: off");
            return;
        }
    }
    if (sb_bat_fast(&U.mv, &U.ma)) {
        if (U.mv && U.mv < U.fs_sag_cur)
            U.fs_sag_cur = U.mv;
        if (U.mv && U.mv < 3300) {
            fs_stop(SURV_FS_WHY_LOWBAT, "!the battery dropped below 3.3 V");
            return;
        }
        uint16_t w = 0;
        if (sb_fg_word(0xb3U, &w)) {        /* the gauge's VBYP, ~0.427 mV a step (charge.c FG_VBYP) */
            U.fs_vb_last = (int)((uint32_t)w * 427U / 1000U);
            if (in >= FS_VBUS_GRACE_MS && U.fs_vb_last > 1000) {
                U.fs_vb_sum += U.fs_vb_last;
                ++U.fs_vb_cnt;
            }
        }
        if (sb_fg_word(0xb4U, &w))          /* the gauge's input-current word, raw (its meaning in OTG: logged) */
            U.fs_iin_raw = w;
        if (U.have_base && in >= FS_EST_FROM_MS) {
            int extra = U.base_ma - U.ma;
            int vb = U.fs_vb_last > 3000 ? U.fs_vb_last : 5100;
            int est = extra > 0 ? (int)((int64_t)extra * U.mv / vb * 85 / 100) : 0;
            U.est_ma = est;
            U.fs_sum += est;
            U.fs_cellw_sum += extra > 0 ? (int)((int64_t)extra * U.mv / 100000) : 0;   /* 0.1 W from the cell */
            ++U.fs_cnt;
            if (est > U.fs_peak)
                U.fs_peak = est;
        }
    }
    if (now - U.t_fs_temp >= 200U) {        /* five times a second: the temperatures and the charge */
        U.t_fs_temp = now;
        U.bat_c10 = bat_temp_c10();
        U.usb_c10 = therm_c10(1);
        U.chg_c10 = therm_c10(3);
        if (U.bat_c10 <= -1000 || U.bat_c10 >= SURV_FS_BAT_STOP_C10) {
            fs_stop(SURV_FS_WHY_HOT, "!the battery reached 45 C (or cannot be read)");
            return;
        }
        if (U.usb_c10 > -1000 && U.usb_c10 >= FS_CONN_STOP_C10) {
            fs_stop(SURV_FS_WHY_HOT, "!the USB connector reached 60 C");
            return;
        }
        if (U.chg_c10 > -1000 && U.chg_c10 >= FS_CHG_STOP_C10) {
            fs_stop(SURV_FS_WHY_HOT, "!the charger reached 65 C");
            return;
        }
        struct sbos_battery b;
        battery_get(&b);
        if (b.ok && b.pct < 3) {
            fs_stop(SURV_FS_WHY_LOWBAT, "!the battery is at 3 %");
            return;
        }
    }
    if (now - U.t_fs_log >= 250U) {         /* the ladder's trace: four lines a second while the output is on */
        U.t_fs_log = now;
        struct ub u;
        ub_init(&u);
        ub_s(&u, "survival: fs: p");
        ub_dec(&u, (uint64_t)U.fs_n + 1U);
        ub_s(&u, " +");
        ub_dec(&u, in);
        ub_s(&u, " ms: VBUS code ");
        ub_dec(&u, (uint64_t)(code < 0 ? 99 : code));
        ub_s(&u, ", battery ");
        ub_dec(&u, (uint64_t)U.mv);
        ub_s(&u, " mV ");
        ub_dec(&u, (uint64_t)(U.ma < 0 ? -U.ma : U.ma));
        ub_s(&u, U.ma < 0 ? " mA out, about " : " mA in, about ");
        ub_dec(&u, (uint64_t)(U.est_ma < 0 ? 0 : U.est_ma));
        ub_s(&u, " mA at the port; VBYP ");
        ub_dec(&u, (uint64_t)(U.fs_vb_last > 0 ? U.fs_vb_last : 0));
        ub_s(&u, " mV, IIN word ");
        ub_hex(&u, (uint64_t)U.fs_iin_raw, 4);
        ub_s(&u, "; ");
        ub_dec(&u, (uint64_t)(U.bat_c10 > 0 ? U.bat_c10 : 0));
        ub_s(&u, " / ");
        ub_dec(&u, (uint64_t)(U.usb_c10 > 0 ? U.usb_c10 : 0));
        ub_s(&u, " / ");
        ub_dec(&u, (uint64_t)(U.chg_c10 > 0 ? U.chg_c10 : 0));
        ub_s(&u, " dC");
        sb_log(u.b);
    }
    if (in >= U.fs_pulse_ms) {              /* the pulse is over: the output off */
        if (!chg_wr(R_CNFG00, U.cnfg00)) {
            fs_stop(SURV_FS_WHY_FAULT, "!the charger did not take its old mode at the pulse's end");
            return;
        }
        U.fs_on_ms += in;
        fs_pulse_end();
        if (U.fs_n >= U.fs_max) {
            fs_stop(SURV_FS_WHY_DONE, U.fs_max > 1 ? "every pulse of this hold done" : "the pulse is done");
            return;
        }
        U.t_phase = now;                    /* the rest between pulses */
    }
}

static void usb_tick(void)
{
    uint64_t now = sb_ms();
    if (U.state != SURV_USB_ON) {
        if (U.written)
            usb_back();
        if (U.state == SURV_USB_COOL && now >= U.t_cool) {
            U.state = SURV_USB_OFF;
            kt_cancel(KT_SURV, KT_SURV_USB);
        }
        return;
    }
    if (U.fire == 2) {
        fs_tick(now);
        return;
    }
    if (!U.fire && now - U.t_on >= SURV_USB_BURST_MS) {
        surv_usb_stop("the 10 s burst is over");
        return;
    }
    if (U.fire && now - U.t_hold > FIRE_HOLD_MS) {
        surv_usb_stop("released");
        return;
    }
    if (U.fire && !K.screen_on) {
        surv_usb_stop("the screen went off");
        return;
    }
    if (U.fire && now - U.t_on >= FIRE_MAX_MS) {
        surv_usb_stop("2 minutes of continuous output: 15 s for the port and the charger to cool");
        return;
    }
    if (U.fire && U.paused) {               /* resting: on again when the time is up and nothing is hot */
        if (now < U.t_pause)
            return;
        if (!fire_resume()) {
            surv_usb_stop("!the charger did not take OTG mode again");
            return;
        }
        sb_log("survival: fire: on again");
    }
    uint8_t c0 = 0, d0 = 0, d1 = 0, d2 = 0;
    int rd_ok = chg_rd(R_CNFG00, &c0) && chg_rd(R_DTLS0, &d0) && chg_rd(R_DTLS1, &d1) && chg_rd(R_DTLS2, &d2);
    if (!rd_ok) {
        surv_usb_stop("!the charger chip stopped answering");
        return;
    }
    if ((d2 & 0x0fU) != U.byp || (d1 & 0x0fU) != U.chg) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "survival: usb out: the chip's status: BYP_DTLS ");
        ub_hex(&u, d2 & 0x0fU, 1);
        ub_s(&u, " CHG_DTLS ");
        ub_hex(&u, d1 & 0x0fU, 1);
        ub_s(&u, " CHGIN_DTLS ");
        ub_hex(&u, (d0 >> 5) & 3U, 1);
        if (U.trips < 20U || (d2 & 1U) == 0)
            sb_log(u.b);                    /* the current-limit events: the first 20 one by one, then counted */
    }
    if ((d2 & 1U) && !(U.byp & 1U))
        ++U.trips;                          /* BYP_DTLS bit 0: the output switch at its current limit (the chip
                                             * opens it after ~30 ms and retries every ~300 ms by itself) */
    U.byp = d2 & 0x0fU;
    U.chg = d1 & 0x0fU;
    if (U.chg == 0x0aU) {                   /* CHG_DTLS 0x0A: the charger's own thermal shutdown */
        surv_usb_stop("!the charger chip shut down from heat (its own protection)");
        return;
    }
    if ((c0 & 0x0fU) != MODE_OTG) {
        if (!U.fire) {
            surv_usb_stop("!the charger switched its output off by itself (its protection)");
            return;
        }
        if (++U.resets >= 3U) {
            surv_usb_stop("!the charger switched its output off by itself 3 times");
            return;
        }
        fire_pause(FIRE_REARM_MS, "the charger switched its output off by itself (its protection): 2 s, then again");
        return;
    }
    vbus_read();
    if (U.vhi >= 5500)
        U.vbus_seen = 1;
    if (!U.fire) {                          /* fire mode leaves a short to the chip's own limit and retry */
        if (U.vbus_seen && U.vhi >= 0 && U.vhi <= 3500) {   /* VBUS below 3.5 V: the load holds it down (a short) */
            if (++U.low_ticks >= 30) {
                surv_usb_stop("!the output was held below 3.5 V for 3 s (a short): off");
                return;
            }
        } else {
            U.low_ticks = 0;
        }
    }
    if (sb_bat_fast(&U.mv, &U.ma)) {
        if (!U.fire && U.mv < 3300) {
            surv_usb_stop("!the battery dropped below 3.3 V");
            return;
        }
        if (U.fire) {
            U.sag_ticks = U.mv < FIRE_SAG_MV ? U.sag_ticks + 1U : 0U;
            if (U.sag_ticks >= 3U) {
                surv_usb_stop("!the battery sagged below 3.2 V (the brown-out floor)");
                return;
            }
        }
        if (U.have_base && now - U.t_on >= 300U) {
            int extra = U.base_ma - U.ma;   /* more discharge: the boost's input */
            int est = extra > 0 ? extra * U.mv / 5000 * 85 / 100 : 0;
            U.est_ma = est;
        }
    }
    if ((now - U.t_on) % 1000U < 100U) {    /* once a second: the temperatures and the charge */
        U.bat_c10 = bat_temp_c10();
        U.usb_c10 = therm_c10(1);
        U.chg_c10 = therm_c10(3);
        struct sbos_battery b;
        battery_get(&b);
        if (U.bat_c10 <= -1000 || U.bat_c10 >= limit_c10()) {
            surv_usb_stop("!the battery reached the Survival limit");
            return;
        }
        if (U.usb_c10 > -1000 && U.usb_c10 >= USB_CONN_STOP_C10) {
            surv_usb_stop("!the USB connector reached 70 C");
            return;
        }
        if (U.chg_c10 > -1000 && U.chg_c10 >= CHG_STOP_C10) {
            surv_usb_stop("!the charger reached 75 C");
            return;
        }
        if (b.ok && b.pct < (U.fire ? 1 : 3)) {
            surv_usb_stop(U.fire ? "!the battery is empty (1 %)" : "!the battery is at 3 %");
            return;
        }
        if (U.fire) {
            int draw = U.ma < 0 ? -U.ma : 0;
            int usable = b.ok ? b.cap_mah - b.full_mah / 100 : 0;
            U.fire_min_left = draw > 200 && usable > 0 ? usable * 60 / draw : -1;
            int warm = (U.usb_c10 > -1000 && U.usb_c10 >= USB_CONN_STOP_C10 - WARM_MARGIN_C10) ||
                       (U.chg_c10 > -1000 && U.chg_c10 >= CHG_STOP_C10 - WARM_MARGIN_C10) ||
                       U.bat_c10 >= limit_c10() - WARM_MARGIN_C10;
            if (warm && now - U.t_duty >= FIRE_DUTY_ON_MS) {
                fire_pause(FIRE_DUTY_ON_MS, "warm (within 5 C of a stop): 2 s on, 2 s off");
                return;
            }
        }
        struct ub u;
        ub_init(&u);
        ub_s(&u, U.fire ? "survival: fire: t " : "survival: usb out: t ");
        ub_dec(&u, (now - U.t_on) / 1000U);
        ub_s(&u, " s, battery ");
        ub_dec(&u, (uint64_t)U.mv);
        ub_s(&u, " mV ");
        ub_dec(&u, (uint64_t)(U.ma < 0 ? -U.ma : U.ma));
        ub_s(&u, U.ma < 0 ? " mA out, " : " mA in, ");
        ub_dec(&u, (uint64_t)(U.bat_c10 / 10));
        ub_s(&u, " C; USB connector ");
        ub_dec(&u, (uint64_t)(U.usb_c10 > 0 ? U.usb_c10 / 10 : 0));
        ub_s(&u, " C, charger ");
        ub_dec(&u, (uint64_t)(U.chg_c10 > 0 ? U.chg_c10 / 10 : 0));
        ub_s(&u, " C; VBUS code ");
        ub_dec(&u, (uint64_t)(U.vhi < 0 ? 99 : (U.vhi - 3500) / 1000));
        ub_s(&u, ", about ");
        ub_dec(&u, (uint64_t)(U.est_ma < 0 ? 0 : U.est_ma));
        ub_s(&u, " mA out of the port; limit events ");
        ub_dec(&u, U.trips);
        sb_log(u.b);
    }
}

void surv_usb_status(struct surv_usb *s)
{
    uint64_t now = sb_ms();
    memset(s, 0, sizeof(*s));
    s->state = U.state;
    s->ms_left = U.state == SURV_USB_ON ? (int32_t)((U.fire ? FIRE_MAX_MS : SURV_USB_BURST_MS) - (uint32_t)(now - U.t_on)) :
                 U.state == SURV_USB_COOL && U.t_cool > now ? (int32_t)(U.t_cool - now) : 0;
    if (s->ms_left < 0)
        s->ms_left = 0;
    s->est_ma = U.state == SURV_USB_ON ? U.est_ma : -1;
    s->bat_ma = U.ma;
    s->bat_mv = U.mv;
    s->vbus_lo_mv = U.state == SURV_USB_ON ? U.vlo : -1;
    s->vbus_hi_mv = U.state == SURV_USB_ON ? U.vhi : -1;
    s->bat_c10 = U.bat_c10;
    s->limit_c10 = limit_c10();
    s->ilim_ma = U.ilim_ma;
    s->bursts = U.bursts;
    s->byp_dtls = U.byp;
    s->chg_dtls = U.chg;
    s->why = U.why;
    s->refused = U.refused;
    s->events = U.events;
    s->fire = U.fire;
    s->trips = U.trips;
    s->resets = U.resets;
    s->paused = U.state == SURV_USB_ON && U.paused;
    s->usb_c10 = U.usb_c10;
    s->chg_c10 = U.chg_c10;
    s->fire_min_left = U.state == SURV_USB_ON && U.fire == 1 ? U.fire_min_left : -1;
    s->fs_pulse_ms = (int32_t)U.fs_pulse_ms;
    s->fs_repeat = U.fs_repeat;
    s->fs_n = U.fs_n;
    s->fs_max = U.fs_max;
    s->fs_out = U.state == SURV_USB_ON && U.fire == 2 && U.fs_out;
    s->fs_avg_ma = U.fs_avg;
    s->fs_peak_ma = U.fs_peak_last;
    s->fs_w10 = U.fs_w10;
    s->fs_vmin = U.fs_vmin;
    s->fs_sag_mv = U.fs_sag;
    s->fs_why = U.fs_why;
    s->fs_vbyp_mv = U.fs_vb_mean;
    s->fs_cellw10 = U.fs_cellw10;
    s->fs_load_w10 = U.fs_load_w10;
    s->fs_chip_w10 = U.fs_chip_w10;
    if (U.state == SURV_USB_ON && U.fire == 2) {   /* the time left of this pulse, or of the rest before the next */
        uint64_t in = now - U.t_phase, len = U.fs_out ? U.fs_pulse_ms : fs_gap_ms();
        s->ms_left = in < len ? (int32_t)(len - in) : 0;
    }
}

/* ---- the emergency card: the Info app's state ("CRD2"; "CRD1" had the first six fields) --------------------- */
const char *const surv_card_label[SURV_CARD_NF] = { "Name", "Blood type", "Allergies", "Medicines", "Conditions",
                                                    "Call", "Call 2", "Call 3", "Notes" };
struct card_rec { char magic[4]; char f[SURV_CARD_NF][SURV_CARD_FL + 1]; };

int surv_card_get(struct surv_card *c)
{
    memset(c, 0, sizeof(*c));
    int a = app_by_id("info");
    if (a < 0)
        return 0;
    const struct kapp *p = &apps[a];
    unsigned nf = 0;
    if (p->state_n >= 4 && p->state[0] == 'C' && p->state[1] == 'R' && p->state[2] == 'D')
        nf = p->state[3] == '2' ? SURV_CARD_NF : p->state[3] == '1' ? 6U : 0U;
    if (!nf || p->state_n < 4U + nf * (SURV_CARD_FL + 1U))
        return 0;
    int any = 0;
    for (unsigned i = 0; i < nf; ++i) {
        memcpy(c->f[i], p->state + 4 + i * (SURV_CARD_FL + 1U), SURV_CARD_FL);
        c->f[i][SURV_CARD_FL] = 0;
        any |= c->f[i][0] != 0;
    }
    return any;
}

int surv_card_set(const struct surv_card *c)
{
    int a = app_by_id("info");
    if (a < 0)
        return 0;
    static struct card_rec r;
    memset(&r, 0, sizeof(r));
    r.magic[0] = 'C', r.magic[1] = 'R', r.magic[2] = 'D', r.magic[3] = '2';
    for (unsigned i = 0; i < SURV_CARD_NF; ++i) {
        memcpy(r.f[i], c->f[i], SURV_CARD_FL);
        r.f[i][SURV_CARD_FL] = 0;
    }
    memcpy(apps[a].state, &r, sizeof(r));
    apps[a].state_n = sizeof(r);
    settings_changed();
    sb_log("survival: the emergency card was changed (saved with the settings)");
    return 1;
}

/* ---- extreme power saving and last 2 % -------------------------------------------------------------------------
 * 1.2-alpha29 (Track D): Extreme power saving is the base: everything off but the clock, the torch and SOS (Heat, USB
 * power out, Wi-Fi, the modem, Linux, the sensor hub, re-checked every 5 s), the A55 at its lowest step (455 MHz)
 * with the screen on too, the screen at its low level and off after 15 s, the panel itself off with the screen (no
 * dim clock), and fewer wake-ups with the screen off (the keys every 100 ms instead of 50, the gauge every 5 min
 * instead of 1). Last 2 % turns it on and adds the run to 1 %. Either can be switched on by hand. */
int surv_last2_auto = 1;
static uint32_t saver_screen_off_was;
static int saver_bright_was, saver_panel_off_was, saver_aod_was;

int surv_last2(void)
{
    return last2;
}

int surv_saver(void)
{
    return saver_applied;
}

static void last2_keep(void)
{
    /* everything off but the clock, the torch and SOS; re-checked every 5 s (nothing else may start meanwhile) */
    const char *w = last2 ? "Last 2 % mode" : "Extreme power saving";
    if (heater_running_ui())
        heater_stop_all(w);
    if (U.state == SURV_USB_ON && !U.fire)
        surv_usb_stop(w);
    if (wifi_on())
        (void)wifi_power(0);
    if (kmodem_on())
        kmodem_stop(w);
    if (guest_owns_m3())
        (void)guest_stop();
    surv_sensors_on(0);
}

static void saver_apply(const char *why)
{
    int want = saver_user || last2;
    if (want == saver_applied)
        return;
    saver_applied = want;
    if (want) {
        last2_keep();
        saver_screen_off_was = K.screen_off_s;
        saver_bright_was = panel_bright_level;
        saver_panel_off_was = panel_off_on;
        saver_aod_was = em_aod;
        K.screen_off_s = 15;
        (void)panel_brightness(1);
        panel_off_on = 1;                   /* the panel itself off with the screen */
        em_aod = 0;                         /* no dim clock */
        power_last2_clock(1);
        kt_set(KT_SURV, KT_SURV_TICK, sb_ms(), 5000, 1);
        sb_logf("survival: Extreme power saving ON: only the clock, the torch and SOS; A55 at 455 MHz; ", why ? why : "");
    } else {
        K.screen_off_s = saver_screen_off_was ? saver_screen_off_was : K.screen_off_s;
        if (saver_bright_was)
            (void)panel_brightness(saver_bright_was);
        panel_off_on = saver_panel_off_was;
        em_aod = saver_aod_was;
        power_last2_clock(0);
        kt_cancel(KT_SURV, KT_SURV_TICK);
        sb_logf("survival: Extreme power saving off: ", why ? why : "");
    }
}

/* settings.c: while the saver applies, the values to save are the user's own (from before it), not its temporary
 * 15 s / low / panel off; 1 when it filled them */
int surv_saver_saved(uint32_t *screen_off_s, int *bright, int *panel_off)
{
    if (!saver_applied)
        return 0;
    *screen_off_s = saver_screen_off_was;
    *bright = saver_bright_was;
    *panel_off = saver_panel_off_was;
    return 1;
}

int surv_saver_set(int on, const char *why)
{
    saver_user = on != 0;
    saver_apply(why);
    return 0;
}

/* services.c: with the screen off, how often the keys and the gauge are read (fewer wake-ups while saving) */
uint32_t surv_input_poll_ms(void) { return saver_applied ? 100U : 50U; }
uint32_t surv_battery_poll_ms(void) { return saver_applied ? 300000U : 60000U; }

static const char *last2_why;

const char *surv_last2_why(void)
{
    return last2_why;
}

static int last2_force;                     /* the self test: the charging refusal skipped (it runs on the cable) */

int surv_last2_set(int on, const char *why)
{
    if (!on == !last2)
        return 0;
    if (on && !last2_force) {
        struct sbos_battery b;
        battery_get(&b);
        if (b.ok && b.charger) {
            last2_why = "The phone is charging: Last 2 % is for when it can't be.";
            return -1;
        }
    }
    last2_why = 0;
    last2 = on != 0;
    saver_apply(why);
    if (last2) {
        sb_logf("survival: Last 2 % mode ON (runs to 1 %): ", why ? why : "");
        notif_post("battery", "Last 2 % mode", "Only the clock, the torch and SOS. Runs to 1 %.");
    } else {
        sb_logf("survival: Last 2 % mode off: ", why ? why : "");
    }
    return 0;
}

/* ---- the battery estimate (1.2-alpha29): hours / days left, honest about what is measured -------------------
 * The kernel's battery model learns the mean current with the screen on and off (batmodel.c). Extreme power saving
 * with the screen off is learned here (the gauge's average current after 2 minutes in that state, not charging; an
 * exponential mean kept with the survival options). Until 5 readings it uses the screen-off figure and says so. */
static int32_t saver_ma;                    /* learned mean, mA (0 none) */
static uint32_t saver_n;

void surv_battery_sample(int avg_ma, int charger, int screen_on, uint64_t in_state_ms)
{
    if (!saver_applied || screen_on || charger || avg_ma >= 0 || in_state_ms < 120000U)
        return;
    int32_t d = -avg_ma;
    if (d > 2000)
        return;
    saver_ma = saver_ma ? saver_ma + (d - saver_ma) / 4 : d;
    if (saver_n < 255)
        ++saver_n;
}

int surv_estimate(struct surv_est *e)
{
    struct sbos_battery b;
    struct sbos_estimate be;
    memset(e, 0, sizeof(*e));
    battery_get(&b);
    if (!b.ok)
        return 0;
    battery_estimate(&be);
    e->pct = b.pct;
    e->charging = b.charger != 0;
    int usable = b.cap_mah - b.full_mah / 100;      /* to 1 % (Last 2 % runs that far) */
    e->usable_mah = usable > 0 ? usable : 0;
    e->ma_on = be.now_ma;
    e->ma_off = be.screen_off_ma;
    e->learned_normal = be.learned;
    e->ma_saver = saver_n >= 5 ? saver_ma : be.screen_off_ma;
    e->learned_saver = saver_n >= 5;
    e->ma_heat = 2800;                      /* Survival heat, measured 2026-11-08 (heater.c FLAT_MA) */
    if (e->ma_on > 0)
        e->min_on = e->usable_mah * 60 / e->ma_on;
    if (e->ma_off > 0)
        e->min_off = e->usable_mah * 60 / e->ma_off;
    if (e->ma_saver > 0)
        e->min_saver = e->usable_mah * 60 / e->ma_saver;
    e->min_heat = e->usable_mah * 60 / e->ma_heat;
    return 1;
}

/* ---- light and sound tools (1.2-alpha29) -------------------------------------------------------------------
 * The screen at the panel's top level for a bright signal (the UI draws it white); the whistle needs the speaker
 * (Track I sets surv_tone_hook when its driver lands: never a call path, only a tone) */
void surv_light_boost(int on)
{
    panel_boost(4, on != 0);
}

int (*surv_tone_hook)(int hz, int on);

int surv_whistle(int on)
{
    if (!surv_tone_hook)
        return -1;
    return surv_tone_hook(3150, on);        /* a pea whistle's pitch, about 3.15 kHz */
}

const char *surv_whistle_why(void)
{
    return surv_tone_hook ? 0 : "The whistle needs the speaker's driver (not in this version yet).";
}

/* services.c's battery check: in Last 2 % the phone runs to 1 % (or 3.4 V on average) instead of 3 %; 1 = off now */
int surv_last2_battery(int pct, int avg_mv, int charger)
{
    if (charger) {
        if (last2 && pct >= 10)
            surv_last2_set(0, "charging, 10 % reached");
        return 0;
    }
    if (sbos_selftest_enabled && !last2)
        return -1;                          /* unattended images keep their return to the menu at the reserve */
    if (!last2 && surv_last2_auto && pct <= 3)
        surv_last2_set(1, "the battery reached 3 %");
    if (!last2)
        return -1;                          /* the usual reserve applies */
    return pct < 1 || (avg_mv > 0 && avg_mv < 3400);
}

/* ---- sensors ------------------------------------------------------------------------------------------------- */
/* 1.2-alpha28: the hub on demand (it passed on the phone at 22:3x: shub-4aa8cd3). A start takes about 4 s (the
 * firmware from the card, its 452 writes, SSP); then the 50 ms poll. It stops 60 s after the last keep-alive, at
 * Last 2 % and before a restart */
#define SHUB_IDLE_MS 60000U
static uint64_t shub_last_use;
static int shub_on;
static unsigned shub_mask;                  /* the SHUB_SENS_* the running hub was started with */

/* 1.2-alpha29 (the drivers track's ask): the hub with only the SHUB_SENS_* sensors asked for (0: none, e.g. the call
 * screen, which adds proximity itself). A caller wanting a sensor the running hub lacks restarts it with both sets
 * (about 4 s); everything else as surv_sensors_on */
int surv_sensors_on_mask(unsigned mask)
{
    if (saver_applied)
        return -1;
    shub_last_use = sb_ms();
    mask &= SHUB_SENS_ALL;
    if (shub_on && shub_state() != SHUB_FAILED) {
        if ((shub_mask | mask) == shub_mask)
            return 0;
        shub_leave();                       /* a sensor more: start again with both sets */
        shub_mask |= mask;
    } else {
        shub_mask = mask;
    }
    shub_on = 1;
    shub_begin_ex(0, shub_mask);
    sb_log(shub_mask == SHUB_SENS_ALL ? "survival: the sensor hub starting (compass, level, steps)" :
                                        "survival: the sensor hub starting (only some sensors)");
    kt_set(KT_SURV, KT_SURV_SHUB, sb_ms(), 20, 0);
    return 0;
}

int surv_sensors_on(int on)
{
    if (!on) {
        if (shub_on) {
            shub_on = 0;
            shub_mask = 0;
            kt_cancel(KT_SURV, KT_SURV_SHUB);
            shub_leave();
            sb_log("survival: the sensor hub off");
        }
        return 0;
    }
    return surv_sensors_on_mask(SHUB_SENS_ALL);
}

int surv_sensors_state(void)
{
    return shub_on ? shub_state() : SHUB_OFF;
}

static void shub_tick(void)
{
    if (!shub_on)
        return;
    if (saver_applied || sb_ms() - shub_last_use > SHUB_IDLE_MS) {
        surv_sensors_on(0);
        return;
    }
    if (shub_state() == SHUB_STARTING) {
        if (!shub_step()) {
            kt_set(KT_SURV, KT_SURV_SHUB, sb_ms(), shub_wait_ms(), 0);
            return;
        }
        if (shub_state() != SHUB_RUNNING) {
            sb_logf("survival: the sensor hub did not start: ", shub_why());
            return;                         /* failed: left as found; the next keep-alive tries again */
        }
        sb_log("survival: the sensor hub runs");
    }
    if (shub_state() == SHUB_RUNNING) {
        shub_poll();
        kt_set(KT_SURV, KT_SURV_SHUB, sb_ms(), 50, 0);
    }
}

const char *surv_sensors_why(void)
{
    /* 1.2-alpha28: the driver exists (shub.c) but this image does not start the hub: its firmware must come from the
     * card (it is never in the image); the test images start it */
    if (shub_on && shub_state() == SHUB_RUNNING)
        return "The sensor hub is running.";
    if (shub_on && shub_state() == SHUB_STARTING)
        return "Starting the sensor hub (about 4 seconds).";
    if (shub_state() == SHUB_FAILED)
        return shub_why();
    if (saver_applied)
        return "Off while Extreme power saving is on.";
    return "The compass, the level and the step counter start when this page opens.";
}

/* ---- the kernel ---------------------------------------------------------------------------------------------- */
void surv_init(void)
{
    memset(&U, 0, sizeof(U));
    U.ilim_ma = -1;
    U.vlo = U.vhi = -1;
    U.bat_c10 = -1000;
    prev_before_reset = sb_before_reset;
    sb_before_reset = before_reset;
    /* 1.2-alpha30: a restart in the middle of an S9 #2 override (a crash: before_reset did not run) must not leave
     * the boost's target raised: VBYPSET back to Samsung's 0 (5.0 V) if it is not. Nothing is written otherwise. */
    uint8_t c11 = 0;
    if (sb_max_init() && chg_rd(R_CNFG11, &c11) && (c11 & 0x7fU) != 0) {
        int ok = chg_wr(R_CNFG11, (uint8_t)(c11 & 0x80U));
        sb_log(ok ? "survival: fs: VBYPSET was raised at the start (a crash in an override?): back to 5.0 V" :
                    "survival: fs: VBYPSET was raised at the start and did NOT go back");
    }
}

void surv_timer(unsigned id)
{
    if (id == KT_SURV_USB)
        usb_tick();
    else if (id == KT_SURV_TICK && saver_user && !saver_applied)
        saver_apply("on again after a restart");
    else if (id == KT_SURV_TICK && saver_applied)
        last2_keep();
    else if (id == KT_SURV_SHUB)
        shub_tick();
}

/* the saved options (settings.c's "sbos.surv" record): v1, sos options, last-2 auto, the location */
void surv_pack(uint8_t o[16])
{
    memset(o, 0, 16);
    o[0] = 1;
    o[1] = (uint8_t)surv_sos_opts;
    o[2] = (uint8_t)((surv_last2_auto != 0) | (saver_user != 0) << 1);   /* 1.2-alpha29: Extreme saving kept */
    o[3] = (uint8_t)saver_n;                /* 1.2-alpha29: Extreme power saving's learned current */
    o[12] = (uint8_t)saver_ma;
    o[13] = (uint8_t)(saver_ma >> 8);
    for (int i = 0; i < 4; ++i) {
        o[4 + i] = (uint8_t)((uint32_t)surv_lat_x100 >> (8 * i));
        o[8 + i] = (uint8_t)((uint32_t)surv_lon_x100 >> (8 * i));
    }
}

void surv_unpack(const uint8_t in[16])
{
    if (in[0] != 1)
        return;
    surv_sos_opts = in[1] & 0x33U;
    surv_last2_auto = in[2] & 1;
    if ((in[2] & 2) && !sbos_selftest_enabled) {    /* Extreme power saving was on: on again (not in test images) */
        saver_user = 1;
        kt_set(KT_SURV, KT_SURV_TICK, sb_ms(), 5000, 1);
    }
    saver_n = in[3];
    saver_ma = (int32_t)(in[12] | in[13] << 8);
    uint32_t a = 0, b = 0;
    for (int i = 0; i < 4; ++i) {
        a |= (uint32_t)in[4 + i] << (8 * i);
        b |= (uint32_t)in[8 + i] << (8 * i);
    }
    surv_lat_x100 = (int32_t)a;
    surv_lon_x100 = (int32_t)b;
}

/* ---- the silent self-test (uitest.c's survival variant): one step a call, 1 when done ------------------------ */
static int st_phase;
static uint64_t st_t;
static unsigned st_fails;
static const char *st_first_fail;           /* named again in the last line: pmsg may keep only the run's end */

static void st_check(const char *what, int ok)
{
    sb_logf(ok ? "selftest: survival: passed: " : "selftest: survival: FAILED: ", what);
    if (!ok && !st_fails++)
        st_first_fail = what;
}

static void st_sun(const char *place, int32_t lat, int32_t lon, int y, int mo, int d, int off, int er, int es, int ec)
{
    int r = -1, s = -1, c = surv_sun(lat, lon, y, mo, d, off, &r, &s);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "selftest: survival: sun ");
    ub_s(&u, place);
    ub_s(&u, ": ");
    if (c == 0) {
        ub_dec2(&u, (unsigned)(r / 60));
        ub_c(&u, ':');
        ub_dec2(&u, (unsigned)(r % 60));
        ub_s(&u, " - ");
        ub_dec2(&u, (unsigned)(s / 60));
        ub_c(&u, ':');
        ub_dec2(&u, (unsigned)(s % 60));
    } else {
        ub_s(&u, c == 1 ? "up all day" : c == 2 ? "down all day" : "bad input");
    }
    int ok = c == ec && (c != 0 || ((r - er < 0 ? er - r : r - er) <= 3 && (s - es < 0 ? es - s : s - es) <= 3));
    ub_s(&u, ok ? " (as expected)" : " (NOT as expected)");
    sb_log(u.b);
    if (!ok && !st_fails++)
        st_first_fail = "the sun table (a NOT as expected line)";
}

int surv_selftest_step(void)
{
    switch (st_phase) {
    case 0: {
        sb_log("selftest: survival: start (silent: no sound; the torch and the motor run one SOS; USB power out read "
               "only unless nothing is plugged in)");
        /* the sun against NOAA's solar calculator (its spreadsheet equations, run on the Mac), 3 min allowed */
        st_sun("Helsinki 2026-11-11", 6017, 2494, 2026, 11, 11, 120, 8 * 60 + 9, 15 * 60 + 59, 0);
        st_sun("Helsinki 2026-06-21", 6017, 2494, 2026, 6, 21, 180, 3 * 60 + 54, 22 * 60 + 50, 0);
        st_sun("Utqiagvik 2026-12-21", 7129, -15679, 2026, 12, 21, -540, 0, 0, 2);
        st_sun("Tromso 2026-06-21", 6965, 1896, 2026, 6, 21, 120, 0, 0, 1);
        st_sun("Quito 2026-03-20", -22, -7851, 2026, 3, 20, -300, 6 * 60 + 18, 18 * 60 + 25, 0);
        st_sun("Sydney 2026-01-15", -3387, 15121, 2026, 1, 15, 660, 5 * 60 + 59, 20 * 60 + 9, 0);
        int r, s, c = surv_sun_today(&r, &s);
        sb_logx("selftest: survival: sun today here (-2 no location set, -3 no clock): ", (uint64_t)(int64_t)c);
        st_check("the Morse cycle is 6.8 s", surv_morse_cycle_ms() == 6800U);
        uint32_t nx = 0;
        st_check("Morse: lit at 0, dark at 250 ms, lit at 1600 ms (O's first dash), dark in the word gap",
                 surv_morse_at(0, &nx) == 1 && nx == 200U && surv_morse_at(250, 0) == 0 && surv_morse_at(1600, 0) == 1 &&
                     surv_morse_at(6000, 0) == 0);
        struct surv_card cd;
        int have = surv_card_get(&cd);
        sb_logx("selftest: survival: the emergency card (1 = one is set; its text is not logged): ", (uint64_t)have);
        sb_log(surv_sensors_why());
        {
            static const char s1[] = "LASTFIX v1 utc=1794500000 lat=60.169856 lon=-24.9383795 alt=12.3 acc=8.5 sats=9 src=gps\n";
            struct gnss_fix gf;
            st_check("the GPS fix line parses (lat 60169856, lon -24938380 rounded, alt 123 dm, 9 sats)",
                     gnss_fix_parse(s1, (int)sizeof(s1) - 1, &gf) && gf.lat_e6 == 60169856 && gf.lon_e6 == -24938380 &&
                         gf.alt_dm == 123 && gf.sats == 9 && gf.utc == 1794500000LL);
            st_check("a line without lat is refused", !gnss_fix_parse("utc=1794500000 lon=1.0", 22, &gf));
            int g = gnss_lastfix(&gf, 1);
            sb_logx("selftest: survival: the card's GPS fix (1 a fix, 0 none, 2^64-1 no card): ", (uint64_t)(int64_t)g);
        }
        st_t = sb_ms();
        st_phase = 1;
        return 0;
    }
    case 1: {
        /* USB power out: the chip read only, then the refusal (or one 2-s burst with no load when nothing is plugged) */
        uint8_t c0 = 0, c2 = 0, ok = 0, d2 = 0, bc = 0, s1 = 0;
        int r = sb_max_init() && chg_rd(R_CNFG00, &c0) && chg_rd(R_CNFG02, &c2) && chg_rd(R_INT_OK, &ok) &&
                chg_rd(R_DTLS2, &d2);
        int ur = sb_usbc_read(0x08, &bc) && sb_usbc_read(0x06, &s1);
        struct ub u;
        ub_init(&u);
        ub_s(&u, "selftest: survival: charger CNFG_00 ");
        ub_hex(&u, c0, 2);
        ub_s(&u, " CNFG_02 ");
        ub_hex(&u, c2, 2);
        ub_s(&u, " INT_OK ");
        ub_hex(&u, ok, 2);
        ub_s(&u, " DTLS_02 ");
        ub_hex(&u, d2, 2);
        ub_s(&u, "; USB-C BC_STATUS ");
        ub_hex(&u, bc, 2);
        ub_s(&u, " USBC_STATUS1 ");
        ub_hex(&u, s1, 2);
        sb_log(u.b);
        st_check("the charger and the USB-C controller read", r && ur);
        if (input_present()) {
            vbus_read();                    /* 1.2-alpha29: the cable's 5 V decodes to 4.5-5.5 V (VBADC bits 7:4) */
            sb_logx("selftest: survival: the cable's VBUS range top, mV: ", (uint64_t)(U.vhi < 0 ? 0 : U.vhi));
            st_check("the cable's VBUS reads 4.5-5.5 V (USBC_STATUS1 VBADC)", U.vlo == 4500 && U.vhi == 5500);
            int rr = surv_usb_start();
            st_check("USB power out refused while the port has an input (the cable)", rr < 0 && !U.written);
            st_phase = 3;
            return 0;
        }
        sb_log("selftest: survival: nothing on the port: one no-load burst, stopped after 2 s");
        st_check("USB power out started (no load)", surv_usb_start() == 0);
        st_t = sb_ms();
        st_phase = 2;
        return 0;
    }
    case 2: {
        if (U.state == SURV_USB_ON && sb_ms() - st_t < 2000U)
            return 0;
        struct surv_usb s;
        surv_usb_status(&s);
        sb_logx("selftest: survival: VBUS range top mV during the burst: ", (uint64_t)(s.vbus_hi_mv < 0 ? 0 : s.vbus_hi_mv));
        st_check("VBUS about 5 V during the burst (VBADC 4.5-5.5 V)", s.vbus_hi_mv == 5500);
        surv_usb_stop("the self test's 2 s");
        uint8_t c0 = 0;
        st_check("the charger back in its old mode", !U.written && chg_rd(R_CNFG00, &c0) && c0 == U.cnfg00);
        st_check("a second burst refused during the cooldown", surv_usb_start() < 0);
        st_phase = 3;
        return 0;
    }
    case 3:
        /* Last 2 %: on (the A55 to 455 MHz), checked, off. The self test runs on the USB cable, where Last 2 % is
         * refused (1.2-alpha28): that refusal is checked, then the mode itself is entered past it */
        {
            struct sbos_battery b;
            battery_get(&b);
            if (b.ok && b.charger) {
                int r = surv_last2_set(1, "the self test (charging: expect a refusal)");
                st_check("Last 2 % refused while charging, with a reason", r < 0 && surv_last2_why() && !surv_last2());
            }
        }
        int panel_off_was = panel_off_on;
        last2_force = 1;
        surv_last2_set(1, "the self test");
        last2_force = 0;
        st_check("Extreme power saving applies with Last 2 % (keys every 100 ms, the gauge every 5 min, the panel "
                 "off with the screen)", surv_saver() && surv_input_poll_ms() == 100U &&
                     surv_battery_poll_ms() == 300000U && panel_off_on);
        {
            struct surv_est e;
            int ok = surv_estimate(&e);
            struct ub u;
            ub_init(&u);
            ub_s(&u, "selftest: survival: estimate (minutes to 1 %): screen on ");
            ub_dec(&u, (uint64_t)(ok ? e.min_on : 0));
            ub_s(&u, ", off ");
            ub_dec(&u, (uint64_t)(ok ? e.min_off : 0));
            ub_s(&u, ", extreme ");
            ub_dec(&u, (uint64_t)(ok ? e.min_saver : 0));
            ub_s(&u, e.learned_saver ? " (measured)" : " (typical)");
            ub_s(&u, ", heat ");
            ub_dec(&u, (uint64_t)(ok ? e.min_heat : 0));
            ub_s(&u, e.charging ? "; charging now" : "");
            sb_log(u.b);
            st_check("the battery estimate is filled", ok && e.ma_on > 0 && e.ma_off > 0 && e.ma_saver > 0);
        }
        sb_mdelay(20);
        sb_logx("selftest: survival: A55 kHz in Last 2 %: ", hw_a55_khz());
        st_check("Last 2 % on: the A55 at its lowest step", hw_a55_khz() > 0 && hw_a55_khz() <= 470000U);
        surv_last2_set(0, "the self test");
        st_check("Extreme power saving off again, the panel setting as it was",
                 !surv_saver() && surv_input_poll_ms() == 50U && panel_off_on == panel_off_was);
        sb_mdelay(20);
        sb_logx("selftest: survival: A55 kHz after: ", hw_a55_khz());
        /* SOS: one Morse cycle on the torch and the motor (the beacon on screen), then off */
        surv_sos_opts = SURV_SOS_TORCH | SURV_SOS_VIB | 3U << SURV_BEACON_SHIFT;
        sos_set(1);
        st_t = sb_ms();
        st_phase = 4;
        return 0;
    case 4:
        if (sb_ms() - st_t < 3000U + surv_morse_cycle_ms() + 400U)
            return 0;
        struct sbos_sos ss;
        sos_status(&ss);
        st_check("SOS reached its flashing phase", ss.state == 1);
        sos_set(0);
        surv_sos_opts = SURV_SOS_TORCH | SURV_SOS_VIB;
        st_check("the torch off after SOS", !torch_on());
        /* 1.2-alpha28: the sensor hub on demand, as the Compass page starts it (the firmware from the card) */
        sb_log("selftest: survival: the sensor hub on demand (compass): its firmware from the card, about 4 s");
        surv_sensors_on(1);
        st_t = sb_ms();
        st_phase = 5;
        return 0;
    case 5: {
        int s5 = surv_sensors_state();
        if (s5 == SHUB_STARTING && sb_ms() - st_t < 12000U)
            return 0;
        if (s5 == SHUB_RUNNING && sb_ms() - st_t < 6000U) {
            surv_sensors_on(1);             /* a few seconds of samples */
            return 0;
        }
        int h = 0, a = 0, pi = 0, ro = 0;
        int hv = surv_heading(&h, &a), tv = surv_tilt(&pi, &ro);
        struct ub u;
        ub_init(&u);
        ub_s(&u, "selftest: survival: sensors: state ");
        ub_dec(&u, (uint64_t)s5);
        ub_s(&u, ", heading x10 ");
        ub_dec(&u, (uint64_t)(hv ? h : 0));
        ub_s(&u, " accuracy ");
        ub_dec(&u, (uint64_t)a);
        ub_s(&u, ", pitch x10 ");
        ub_dec(&u, (uint64_t)(uint32_t)pi);
        ub_s(&u, " roll x10 ");
        ub_dec(&u, (uint64_t)(uint32_t)ro);
        ub_s(&u, "; ");
        ub_s(&u, surv_sensors_why());
        sb_log(u.b);
        if (s5 == SHUB_FAILED && shub_why() && !memcmp(shub_why(), "the sensor hub's firmware is not on the card", 44))
            sb_log("selftest: survival: (not counted: the firmware file is not on this card yet)");
        else
            st_check("the sensor hub ran on demand and gave a level reading", s5 == SHUB_RUNNING && tv);
        surv_sensors_on(0);
        st_check("the sensor hub off again", surv_sensors_state() == SHUB_OFF);
        sb_logx(st_fails ? "selftest: survival: done, FAILED checks: " : "selftest: survival: done, every check passed ",
                st_fails);
        if (st_fails)
            sb_logf("selftest: survival: the first failed check: ", st_first_fail ? st_first_fail : "(not named)");
        st_phase = 6;
        return 1;
    }
    default:
        return 1;
    }
}

/* ---- the power bench (1.2-alpha29, uitest.c's SBOS_SELFTEST_POWER): what each state really draws ----------------
 * Attended: the gauge measures discharge only with the USB cable OUT. Three windows, each 30 s to settle and 120 s
 * of samples (inside the kernel's 10-minute no-input return) (the gauge's current every 5 s, its average at the end): the screen on (the lock screen at its level),
 * the screen off as usual, and the screen off with Extreme power saving (the A55's other three cores are not started by
 * Starbite OS). Then a summary with hours and days at the battery's charge,
 * and a restart into recovery (plug the cable back in for pmsg). The sampling wakes the CPU every 5 s: a little
 * above the true idle figure, said in the summary. */
static struct {
    int phase, win;
    uint64_t t0;
    int64_t sum;
    int n, min_ma, max_ma, valid;
    int32_t mean[3], avg_reg[3];
} PB;
static const char *const pb_name[3] = { "screen on (lock screen)", "screen off", "screen off, Extreme power saving" };

static void pb_window_start(int w)
{
    PB.win = w;
    PB.t0 = sb_ms();
    PB.sum = 0;
    PB.n = 0;
    PB.min_ma = 1 << 30;
    PB.max_ma = -(1 << 30);
    sb_logf("selftest: power: window: ", pb_name[w]);
}

/* 1 when the window is complete */
static int pb_window_step(void)
{
    uint64_t el = sb_ms() - PB.t0;
    if (el < 30000U)
        return 0;                           /* settling */
    int mv = 0, ma = 0;
    if (sb_bat_fast(&mv, &ma)) {
        PB.sum += ma;
        ++PB.n;
        if (ma < PB.min_ma)
            PB.min_ma = ma;
        if (ma > PB.max_ma)
            PB.max_ma = ma;
        if (ma > 0)
            PB.valid = 0;                   /* charging: not a measurement */
    }
    if (el < 150000U)
        return 0;
    uint16_t v = 0;
    int avg = sb_fg_word_checked(0x0b, &v, 0, 0) ? (int32_t)(int16_t)v * 5 / 16 : 0;
    int mean = PB.n ? (int)(PB.sum / PB.n) : 0;
    PB.mean[PB.win] = -mean;
    PB.avg_reg[PB.win] = -avg;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "selftest: power: ");
    ub_s(&u, pb_name[PB.win]);
    ub_s(&u, ": mean ");
    ub_dec(&u, (uint64_t)(mean < 0 ? -mean : mean));
    ub_s(&u, mean <= 0 ? " mA out" : " mA IN (charging)");
    ub_s(&u, " (samples ");
    ub_dec(&u, (uint64_t)PB.n);
    ub_s(&u, ", ");
    ub_dec(&u, (uint64_t)(PB.max_ma < 0 ? -PB.max_ma : PB.max_ma));
    ub_s(&u, "..");
    ub_dec(&u, (uint64_t)(PB.min_ma < 0 ? -PB.min_ma : PB.min_ma));
    ub_s(&u, " mA), the gauge's average ");
    ub_dec(&u, (uint64_t)(avg < 0 ? -avg : avg));
    ub_s(&u, " mA");
    sb_log(u.b);
    return 1;
}

int surv_power_selftest_step(void)
{
    switch (PB.phase) {
    case 0: {
        struct sbos_battery b;
        battery_get(&b);
        PB.valid = !(b.ok && b.charger);
        sb_log(PB.valid ? "selftest: power: start: the cable is out, the gauge measures the phone's draw (about 8 min)" :
                          "selftest: power: start: ON THE CABLE: the gauge sees the charge, these figures are NOT the draw");
        sb_logx("selftest: power: battery %: ", (uint64_t)(b.ok ? b.pct : 0));
        surv_saver_set(0, "the power bench");
        pb_window_start(0);
        PB.phase = 1;
        return 0;
    }
    case 1:
        if (!pb_window_step())
            return 0;
        power_screen_set(0);
        pb_window_start(1);
        PB.phase = 2;
        return 0;
    case 2:
        if (!pb_window_step())
            return 0;
        surv_saver_set(1, "the power bench");
        power_screen_set(0);                /* (the saver's own state with the screen off) */
        pb_window_start(2);
        PB.phase = 3;
        return 0;
    case 3: {
        if (!pb_window_step())
            return 0;
        surv_saver_set(0, "the power bench");
        power_screen_set(1);
        struct sbos_battery b;
        battery_get(&b);
        int usable = b.ok ? b.cap_mah - b.full_mah / 100 : 0;
        for (int w = 0; w < 3; ++w) {
            struct ub u;
            ub_init(&u);
            ub_s(&u, "selftest: power: summary: ");
            ub_s(&u, pb_name[w]);
            ub_s(&u, " ");
            ub_dec(&u, (uint64_t)(PB.mean[w] > 0 ? PB.mean[w] : 0));
            ub_s(&u, " mA = ");
            int ma = PB.mean[w] > 0 ? PB.mean[w] : 0;
            if (ma && usable > 0) {
                int h = usable / ma;
                ub_dec(&u, (uint64_t)h);
                ub_s(&u, " h (");
                ub_dec(&u, (uint64_t)(h / 24));
                ub_s(&u, ".");
                ub_dec(&u, (uint64_t)(h % 24 * 10 / 24));
                ub_s(&u, " days) on ");
                ub_dec(&u, (uint64_t)usable);
                ub_s(&u, " mAh left");
            } else {
                ub_s(&u, "(no figure)");
            }
            sb_log(u.b);
        }
        sb_log(PB.valid ? "selftest: power: done (valid: the cable was out; each figure includes a 5 s sampling wake-up)" :
                          "selftest: power: done, NOT VALID: the phone was charging during the windows");
        PB.phase = 4;
        return 1;
    }
    default:
        return 1;
    }
}
