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

#define KT_SURV_USB 1U
#define KT_SURV_TICK 2U                     /* the slow tick (last 2 %'s keeper), 5 s */
#define CHG 0x69U
#define R_INT_OK 0xb2U
#define R_DTLS0 0xb3U
#define R_DTLS1 0xb4U
#define R_DTLS2 0xb5U
#define R_CNFG00 0xb7U
#define R_CNFG02 0xb9U
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
    int written;                            /* CNFG_00 holds OTG: it must go back on every path */
    int base_ma, have_base;
    int ma, mv, est_ma, vlo, vhi, bat_c10, ilim_ma;
    uint8_t byp, chg;
    unsigned low_ticks, back_fail;
    int vbus_seen;                          /* VBADC showed 4.5-5.5 V in this burst: the short check trusts it */
    uint32_t bursts;
    int refused;
    const char *why;
} U;
static struct sb_auth au;
static void (*prev_before_reset)(void);

static int chg_rd(uint8_t r, uint8_t *v) { return sb_chg_read(r, v); }

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
        unsigned c = s1 >> 3;
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

static void usb_back(void)
{
    if (!U.written)
        return;
    int a = chg_wr(R_CNFG00, U.cnfg00);
    int b = chg_wr(R_CNFG02, U.cnfg02);
    if (a && b) {
        U.written = 0;
        U.back_fail = 0;
        sb_log("survival: usb out: the charger is back as it was (mode and current limit)");
    } else if (!U.back_fail++) {
        sb_log("survival: usb out: the charger did NOT take its old mode back: retried every 100 ms");
    }
}

static void before_reset(void)
{
    if (U.written)
        usb_back();
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
    U.state = SURV_USB_COOL;
    U.t_cool = sb_ms() + (fault ? SURV_USB_COOL_FAULT_MS : SURV_USB_COOL_MS);
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
    if (U.written)
        kt_set(KT_SURV, KT_SURV_USB, sb_ms(), 100, 1);    /* the charger's old mode is retried every 100 ms */
    sb_logf("survival: usb out: refused: ", why);
    notif_post("survival", "USB power out did not start", why);
    return -1;
}

int surv_usb_start(void)
{
    uint64_t now = sb_ms();
    if (U.state == SURV_USB_ON)
        return 0;
    if (U.written)
        return refuse("the charger is not back from the last burst yet");
    if (U.state == SURV_USB_COOL && now < U.t_cool)
        return refuse("cooling down after the last burst");
    U.state = SURV_USB_OFF;
    if (surv_last2())
        return refuse("Last 2 % mode is on");
    if (heater_running_ui())
        return refuse("Heat is running (both draw hard on the battery)");
    if (!sb_max_init())
        return refuse("the charger chip does not answer");
    if (input_present())
        return refuse("something is powering the USB-C port: unplug the charger or computer first");
    struct sbos_battery b;
    battery_get(&b);
    U.bat_c10 = bat_temp_c10();
    if (!b.ok || b.pct < 5)
        return refuse("the battery is below 5 %");
    if (b.mv && b.mv < 3500)
        return refuse("the battery is below 3.5 V");
    if (U.bat_c10 <= -1000)
        return refuse("the battery temperature cannot be read");
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
    if (!chg_wr(R_CNFG02, (uint8_t)((U.cnfg02 & 0x3fU) | 0xc0U)))
        sb_log("survival: usb out: OTG_ILIM 1.5 A did not read back (write-protected?): the chip's limit as it is");
    uint8_t c2 = 0;
    U.ilim_ma = chg_rd(R_CNFG02, &c2) ? (int[]){ 500, 900, 1200, 1500 }[c2 >> 6] : -1;
    if (!chg_wr(R_CNFG00, (uint8_t)((U.cnfg00 & ~0x0fU) | MODE_OTG))) {
        usb_back();
        return refuse("the charger did not take OTG mode");
    }
    U.state = SURV_USB_ON;
    U.refused = 0;
    U.t_on = now;
    U.low_ticks = 0;
    U.vbus_seen = 0;
    U.est_ma = -1;
    U.why = 0;
    ++U.bursts;
    kt_set(KT_SURV, KT_SURV_USB, now, 100, 1);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "survival: usb out: ON (burst ");
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
    sb_log(u.b);
    return 0;
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
    if (now - U.t_on >= SURV_USB_BURST_MS) {
        surv_usb_stop("the 10 s burst is over");
        return;
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
        sb_log(u.b);
    }
    U.byp = d2 & 0x0fU;
    U.chg = d1 & 0x0fU;
    if ((c0 & 0x0fU) != MODE_OTG) {
        surv_usb_stop("!the charger switched its output off by itself (its protection)");
        return;
    }
    /* BYP_DTLS is logged at every change (above) and not yet read as a fault: its bits' meaning comes from the first
     * hands-on test (a current limit into a near-short is normal use here). A short is caught by the voltage below */
    vbus_read();
    if (U.vhi >= 5500)
        U.vbus_seen = 1;
    if (U.vbus_seen && U.vhi >= 0 && U.vhi <= 3500) {   /* VBUS below 3.5 V: the load holds it down (a short) */
        if (++U.low_ticks >= 30) {
            surv_usb_stop("!the output was held below 3.5 V for 3 s (a short): off");
            return;
        }
    } else {
        U.low_ticks = 0;
    }
    if (sb_bat_fast(&U.mv, &U.ma)) {
        if (U.mv < 3300) {
            surv_usb_stop("!the battery dropped below 3.3 V");
            return;
        }
        if (U.have_base && now - U.t_on >= 300U) {
            int extra = U.base_ma - U.ma;   /* more discharge: the boost's input */
            int est = extra > 0 ? extra * U.mv / 5000 * 85 / 100 : 0;
            U.est_ma = est;
        }
    }
    if ((now - U.t_on) % 1000U < 100U) {    /* once a second: the battery's temperature and charge */
        U.bat_c10 = bat_temp_c10();
        struct sbos_battery b;
        battery_get(&b);
        if (U.bat_c10 <= -1000 || U.bat_c10 >= limit_c10()) {
            surv_usb_stop("!the battery reached the Survival limit");
            return;
        }
        if (b.ok && b.pct < 3) {
            surv_usb_stop("!the battery is at 3 %");
            return;
        }
        struct ub u;
        ub_init(&u);
        ub_s(&u, "survival: usb out: t ");
        ub_dec(&u, (now - U.t_on) / 1000U);
        ub_s(&u, " s, battery ");
        ub_dec(&u, (uint64_t)U.mv);
        ub_s(&u, " mV ");
        ub_dec(&u, (uint64_t)(U.ma < 0 ? -U.ma : U.ma));
        ub_s(&u, U.ma < 0 ? " mA out, " : " mA in, ");
        ub_dec(&u, (uint64_t)(U.bat_c10 / 10));
        ub_s(&u, " C; VBUS code ");
        ub_dec(&u, (uint64_t)(U.vhi < 0 ? 99 : (U.vhi - 3500) / 1000));
        ub_s(&u, ", about ");
        ub_dec(&u, (uint64_t)(U.est_ma < 0 ? 0 : U.est_ma));
        ub_s(&u, " mA out of the port");
        sb_log(u.b);
    }
}

void surv_usb_status(struct surv_usb *s)
{
    uint64_t now = sb_ms();
    memset(s, 0, sizeof(*s));
    s->state = U.state;
    s->ms_left = U.state == SURV_USB_ON ? (int32_t)(SURV_USB_BURST_MS - (uint32_t)(now - U.t_on)) :
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

/* ---- last 2 % ------------------------------------------------------------------------------------------------ */
int surv_last2_auto = 1;
static int last2;
static uint32_t last2_screen_off_was;
static int last2_bright_was;

int surv_last2(void)
{
    return last2;
}

static void last2_keep(void)
{
    /* everything off but the clock, the torch and SOS; re-checked every 5 s (nothing else may start meanwhile) */
    if (heater_running_ui())
        heater_stop_all("Last 2 % mode");
    if (U.state == SURV_USB_ON)
        surv_usb_stop("Last 2 % mode");
    if (wifi_on())
        (void)wifi_power(0);
    if (kmodem_on())
        kmodem_stop("Last 2 % mode");
    if (guest_owns_m3())
        (void)guest_stop();
}

int surv_last2_set(int on, const char *why)
{
    if (!on == !last2)
        return 0;
    last2 = on != 0;
    if (last2) {
        last2_keep();
        last2_screen_off_was = K.screen_off_s;
        last2_bright_was = panel_bright_level;
        K.screen_off_s = 15;
        (void)panel_brightness(1);
        power_last2_clock(1);
        kt_set(KT_SURV, KT_SURV_TICK, sb_ms(), 5000, 1);
        sb_logf("survival: Last 2 % mode ON: only the clock, the torch and SOS; A55 at 455 MHz; ", why ? why : "");
        notif_post("battery", "Last 2 % mode", "Only the clock, the torch and SOS. Runs to 1 %.");
    } else {
        K.screen_off_s = last2_screen_off_was ? last2_screen_off_was : K.screen_off_s;
        if (last2_bright_was)
            (void)panel_brightness(last2_bright_was);
        power_last2_clock(0);
        kt_cancel(KT_SURV, KT_SURV_TICK);
        sb_logf("survival: Last 2 % mode off: ", why ? why : "");
    }
    return 0;
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
const char *surv_sensors_why(void)
{
    return "The compass and the level need the sensor hub inside the GNSS chip (BCM4775): its driver is not written yet.";
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
}

void surv_timer(unsigned id)
{
    if (id == KT_SURV_USB)
        usb_tick();
    else if (id == KT_SURV_TICK && last2)
        last2_keep();
}

/* the saved options (settings.c's "sbos.surv" record): v1, sos options, last-2 auto, the location */
void surv_pack(uint8_t o[16])
{
    memset(o, 0, 16);
    o[0] = 1;
    o[1] = (uint8_t)surv_sos_opts;
    o[2] = (uint8_t)(surv_last2_auto != 0);
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

static void st_check(const char *what, int ok)
{
    sb_logf(ok ? "selftest: survival: passed: " : "selftest: survival: FAILED: ", what);
    if (!ok)
        ++st_fails;
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
    if (!ok)
        ++st_fails;
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
        /* Last 2 %: on (the A55 to 455 MHz), checked, off */
        surv_last2_set(1, "the self test");
        sb_mdelay(20);
        sb_logx("selftest: survival: A55 kHz in Last 2 %: ", hw_a55_khz());
        st_check("Last 2 % on: the A55 at its lowest step", hw_a55_khz() > 0 && hw_a55_khz() <= 470000U);
        surv_last2_set(0, "the self test");
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
        sb_logx(st_fails ? "selftest: survival: done, FAILED checks: " : "selftest: survival: done, every check passed ",
                st_fails);
        st_phase = 5;
        return 1;
    default:
        return 1;
    }
}
