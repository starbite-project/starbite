/* SPDX-License-Identifier: GPL-2.0-only
 * Track A 9c: charging as the stock phone does it (main, 2026-11-15: S9 #2 gained 10 % in 3 hours on a 100 W USB-C
 * charger while the widget said "Charging").
 *
 * Why it was slow: S-Boot switches the MAX77705 charger's watchdog on (SBL `set_charger_watchdog`, CNFG_00 = 0x15,
 * build/em-research-battery.md) and Android's driver feeds it on every health poll (CNFG_06 WDTCLR = 01). Starbite OS
 * never fed it, so some time after the start the charger stopped (CHG_DTLS 0x0B "watchdog expired") and the input and
 * charge currents were whatever S-Boot left (or the chip's defaults). Now, every 10 s:
 *   - the watchdog is fed while WDTEN is set (CNFG_06: WDTCLR = 01, CHGPROT unlocked as Samsung's driver keeps it,
 *     the other bits as read);
 *   - with a cable in: the input current limit (CNFG_09 [6:0] CHGIN_ILIM, 25 mA steps, bit 7 kept) and the fast
 *     charge current (CNFG_02 [5:0] CHG_CC, 50 mA steps, OTG_ILIM bits 7:6 kept) from the stock table
 *     (ref/starlte-kernel battery_data_starlte_common.dtsi cable-info) for the charger the USB-C block reports:
 *       wall charger (BC1.2 DCP) or Type-C Rp 3.0 A: 1800 mA in, 2100 mA charge (stock "TA");
 *       Type-C Rp 1.5 A or BC1.2 CDP: 1500 / 1500 (stock "USB_CDP");
 *       a computer's port (BC1.2 SDP): 475 / 550 (stock "USB");
 *       anything else: 500 / 500 (stock "PDIC" / "PREPARE_TA");
 *     nothing above 1800 mA in or 2150 mA charge is ever written (the stock 9 V figures), so 5 V tops out at ~9 W;
 *   - the cell's temperature (ADC 4, the battery thermistor) first: at 45 C and above charging stops (CNFG_00 CHG bit
 *     off, BUCK kept so the phone runs from the cable; again at 42 C or below); from 41 C 1090 mA (stock swelling
 *     0.45C; back below 39 C); below 15 C 700 mA (back at 20 C); below 0 C off (again at 5 C); unread: 500 mA;
 *   - the chip's input regulation (CHG_INT_OK AICL_OK low twice in a row: the source sags) lowers the input limit
 *     100 mA at a time, to 500 mA at least, until the cable is pulled (Linux's max77705_aicl_irq does the same).
 * Never written: the battery's PCM, the fuel gauge, the float voltage (CNFG_04), the protections (CNFG_05 B2SOVRC is
 * the heater's, put back by it), the USB-C block. While Survival's USB power out runs (surv_usb_active) nothing here
 * writes: the charger's mode is its. Before a restart or power-off a stopped charge is switched back on.
 * Logged: "charge: ..." at a plug, an unplug, a change, and once a minute with a cable in: the source, VBUS (the USB-C
 * block's 1 V bands and the gauge's VBYP), the gauge's input current, the limits, CHG_DTLS, the cell's current and
 * temperature.
 */
#include "sb.h"
#include "kern.h"
#include "survival.h"

#define CHG 0x69U
#define R_INT_OK 0xb2U                      /* bit 7 AICL_OK, bit 6 CHGIN_OK, bit 5 WCIN_OK */
#define R_DTLS0 0xb3U                       /* [6:5] CHGIN_DTLS: 3 valid */
#define R_DTLS1 0xb4U                       /* [3:0] CHG_DTLS, [6:4] BAT_DTLS */
#define R_CNFG00 0xb7U                      /* [3:0] mode: b0 CHG, b1 OTG, b2 BUCK, b3 BOOST; b4 WDTEN */
#define R_CNFG02 0xb9U                      /* [7:6] OTG_ILIM, [5:0] CHG_CC (50 mA; <= 2: 100 mA) */
#define R_CNFG06 0xbdU                      /* [3:2] CHGPROT (11 unlocked), [1:0] WDTCLR (01 feeds) */
#define R_CNFG09 0xc0U                      /* [7] kept, [6:0] CHGIN_ILIM ((code + 1) * 25 mA; <= 3: 100 mA) */
#define R_CNFG12 0xc3U                      /* logged only: b5 CHGINSEL */
#define FG_VBYP 0xb3U                       /* the gauge's bypass node (VBUS), ~0.427 mV a step */
#define FG_IIN 0xd0U                        /* the gauge's input current, 0.125 mA a step */

#define TICK_MS 10000U
#define LOG_MS 60000U
#define IL_MAX_MA 1800                      /* never more in (stock TA; 9 V's 1650) */
#define CC_MAX_MA 2150                      /* never more charge (stock 9 V's 2150) */
#define IL_MIN_MA 500

enum { TS_OK = 0, TS_WARM, TS_HOT, TS_COOL, TS_COLD, TS_UNREAD };
static const char *const ts_name[] = { "ok", "warm (41 C+): 1090 mA", "HOT (45 C+): charging stopped",
                                       "cool (below 15 C): 700 mA", "COLD (below 0 C): charging stopped",
                                       "unread: 500 mA" };
static const char *const src_name[] = { "none", "a computer's port (SDP)", "a charging port (CDP)",
                                        "a wall charger (DCP)", "USB-C 1.5 A", "USB-C 3.0 A", "unknown" };

static struct {
    int inited, in, src, ts, aicl_low, aicl_cap, mode_off, out_busy_said;
    int il_ma, cc_ma;                       /* what the chip has now (read back) */
    int dtls, vbus_lo, vbus_hi, vbyp_mv, iin_ma;
    uint32_t feeds, feed_fail, expired, writes, wfail;
    uint64_t plug_t, last_log;
    int pct_at_plug;
} C;
static void (*prev_before_reset)(void);

static int rd(uint8_t r, uint8_t *v) { return sb_chg_read(r, v); }

/* a tier-1 write of one charger register: the value is this file's (capped above), the same fixed-value pattern as
 * nled.c / vib.c / usb.c; read back under mask */
static int wr(uint8_t reg, uint8_t val, uint8_t mask, const char *what)
{
    struct sb_auth au;
    sb_auth_init(&au, SB_SP_MAX, 0, (uint64_t)CHG << 8 | reg, 1, SB_TIER1, 0, what);
    sb_auth_step(&au, SB_STEP_WARN);
    sb_auth_step(&au, SB_STEP_HELD);
    int ok = sb_gate_open(&au) && sb_max_write(CHG, reg, val, SB_TIER1);
    sb_gate_close();
    uint8_t back = 0;
    ok = ok && rd(reg, &back) && (back & mask) == (val & mask);
    ++C.writes;
    if (!ok)
        ++C.wfail;
    return ok;
}

static int il_of(uint8_t c9) { unsigned r = c9 & 0x7fU; return r <= 3 ? 100 : (int)(r + 1U) * 25; }
static int cc_of(uint8_t c2) { unsigned r = c2 & 0x3fU; return r <= 2 ? 100 : (int)r * 50; }

/* the watchdog: fed while S-Boot's WDTEN is set (whatever the cable); CHGPROT unlocked (Samsung's driver keeps it so)
 * because CNFG_02's charge current takes no write without it. The feed itself is not logged (every 10 s: the pmsg
 * ring is 16 KB); the counts are in the minute's line */
static void feed(uint8_t c0)
{
    uint8_t c6 = 0;
    int wdt = (c0 & 0x10U) != 0;
    if (!rd(R_CNFG06, &c6)) {
        if (wdt)
            ++C.feed_fail;
        return;
    }
    int locked = ((c6 >> 2) & 3U) != 3U;
    if (!wdt && !locked)
        return;
    if (locked)
        sb_log("charge: CHGPROT unlocked (CNFG_06), as Samsung's driver keeps it: the charge current can be set");
    sb_max_quiet = !locked;
    /* WDTCLR reads back 0 once taken: only CHGPROT is checked */
    int ok = wr(R_CNFG06, (uint8_t)((c6 & ~0x0fU) | 0x0cU | (wdt ? 0x01U : 0U)), 0x0cU, "charge: the charger watchdog feed");
    sb_max_quiet = 0;
    if (wdt) {
        if (ok)
            ++C.feeds;
        else
            ++C.feed_fail;
    }
}

/* the source, from the USB-C block (read only): BC1.2 ChgTyp and the Type-C Rp current */
static int source(void)
{
    uint8_t bc = 0, cc = 0;
    int t = sb_usbc_read(0x08, &bc) ? (int)(bc & 3U) : 0;
    int ci = sb_usbc_read(0x0a, &cc) ? (int)((cc >> 4) & 3U) : 0;   /* 1 500 mA, 2 1.5 A, 3 3.0 A */
    if (t == 3)
        return 3;
    if (ci == 3)
        return 5;
    if (ci == 2)
        return 4;
    if (t == 2)
        return 2;
    if (t == 1)
        return 1;
    return 6;
}

static void table(int src, int *il, int *cc)
{
    switch (src) {
    case 3: case 5: *il = 1800; *cc = 2100; break;
    case 2: case 4: *il = 1500; *cc = 1500; break;
    case 1: *il = 475; *cc = 550; break;
    default: *il = 500; *cc = 500; break;
    }
}

/* the cell's band, with the stock hysteresis */
static int temp_state(int c10, int was)
{
    if (c10 <= -1000)
        return TS_UNREAD;
    if (c10 >= 450 || (was == TS_HOT && c10 > 420))
        return TS_HOT;
    if (c10 < 0 || (was == TS_COLD && c10 < 50))
        return TS_COLD;
    if (c10 >= 410 || ((was == TS_WARM || was == TS_HOT) && c10 >= 390))
        return TS_WARM;
    if (c10 < 150 || ((was == TS_COOL || was == TS_COLD) && c10 < 200))
        return TS_COOL;
    return TS_OK;
}

static void mode_back(const char *why)
{
    uint8_t c0 = 0;
    if (!C.mode_off || !rd(R_CNFG00, &c0))
        return;
    if ((c0 & 0x0fU) == 0x04U && wr(R_CNFG00, (uint8_t)(c0 | 0x01U), 0x0fU, "charge: charging on again"))
        sb_logf("charge: charging switched on again: ", why);
    C.mode_off = 0;                         /* any other mode is someone else's (Survival's output): left */
}

static void say(const char *head, const struct sbos_battery *b)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "charge: ");
    ub_s(&u, head);
    ub_s(&u, C.in ? src_name[C.src] : "no cable");
    if (C.in) {
        ub_s(&u, "; VBUS ");
        if (C.vbus_hi > 0) {
            ub_dec(&u, (uint64_t)(C.vbus_lo / 1000));
            ub_c(&u, '.');
            ub_dec(&u, (uint64_t)(C.vbus_lo % 1000 / 100));
            ub_c(&u, '-');
            ub_dec(&u, (uint64_t)(C.vbus_hi / 1000));
            ub_c(&u, '.');
            ub_dec(&u, (uint64_t)(C.vbus_hi % 1000 / 100));
            ub_s(&u, " V");
        } else {
            ub_s(&u, "--");
        }
        ub_s(&u, " (VBYP ~");
        ub_dec(&u, (uint64_t)(C.vbyp_mv > 0 ? C.vbyp_mv : 0));
        ub_s(&u, " mV), in ~");
        ub_dec(&u, (uint64_t)(C.iin_ma > 0 ? C.iin_ma : 0));
        ub_s(&u, " mA of ");
        ub_dec(&u, (uint64_t)C.il_ma);
        ub_s(&u, " (cap ");
        ub_dec(&u, (uint64_t)C.aicl_cap);
        ub_s(&u, "), charge limit ");
        ub_dec(&u, (uint64_t)C.cc_ma);
        ub_s(&u, " mA, CHG_DTLS ");
        ub_hex(&u, (uint64_t)C.dtls, 1);
        ub_s(&u, ", temp ");
        ub_s(&u, ts_name[C.ts]);
    }
    if (b && b->ok) {
        ub_s(&u, "; cell ");
        ub_dec(&u, (uint64_t)b->mv);
        ub_s(&u, " mV ");
        ub_sdec(&u, b->ma);
        ub_s(&u, " mA (avg ");
        ub_sdec(&u, b->avg_ma);
        ub_s(&u, ") ");
        ub_dec(&u, (uint64_t)b->pct);
        ub_s(&u, " % ");
        ub_sdec(&u, b->temp_c10 / 10);
        ub_s(&u, " C");
    }
    ub_s(&u, "; wdt feeds ");
    ub_dec(&u, C.feeds);
    if (C.feed_fail) {
        ub_s(&u, " (failed ");
        ub_dec(&u, C.feed_fail);
        ub_c(&u, ')');
    }
    if (C.expired) {
        ub_s(&u, ", expired seen ");
        ub_dec(&u, C.expired);
    }
    if (C.wfail) {
        ub_s(&u, ", writes failed ");
        ub_dec(&u, C.wfail);
    }
    sb_log(u.b);
}

static void tick(void)
{
    uint8_t ok = 0, c0 = 0, d1 = 0, c2 = 0, c9 = 0;
    if (!rd(R_INT_OK, &ok) || !rd(R_CNFG00, &c0))
        return;
    if (surv_usb_active()) {                /* Survival's USB power out owns the charger's mode and CNFG_02 */
        if (!C.out_busy_said++)
            sb_log("charge: Survival's USB power out runs: the charger is left to it");
        return;
    }
    C.out_busy_said = 0;
    feed(c0);
    struct sbos_battery b;
    battery_get(&b);
    int in = (ok & 0x40U) != 0;
    uint64_t now = sb_ms();
    if (!in) {
        if (C.in) {
            C.in = 0;
            mode_back("the cable is out");
            struct ub u;
            ub_init(&u);
            ub_s(&u, "unplugged after ");
            ub_dec(&u, (now - C.plug_t) / 60000U);
            ub_s(&u, " min (");
            ub_sdec(&u, b.ok ? b.pct - C.pct_at_plug : 0);
            ub_s(&u, " %): ");
            say(u.b, &b);
        }
        return;
    }
    const char *head = "";
    if (!C.in) {
        C.in = 1;
        C.plug_t = now;
        C.pct_at_plug = b.ok ? b.pct : 0;
        C.aicl_cap = IL_MAX_MA;
        C.aicl_low = 0;
        C.last_log = 0;
        head = "plugged in: ";
    }
    /* VBUS, the input current: logged only */
    uint8_t s1 = 0;
    uint16_t w = 0;
    C.vbus_lo = C.vbus_hi = -1;
    if (sb_usbc_read(0x06, &s1)) {
        unsigned code = (s1 >> 4) & 0xfU;   /* survival.c alpha29: 1 V bands, 2 = 4.5-5.5 V */
        C.vbus_lo = code ? 2500 + 1000 * (int)code : 0;
        C.vbus_hi = 3500 + 1000 * (int)code;
    }
    C.vbyp_mv = sb_fg_word(FG_VBYP, &w) ? (int)((uint32_t)w * 427U / 1000U) : -1;
    C.iin_ma = sb_fg_word(FG_IIN, &w) ? (int)((uint32_t)w / 8U) : -1;
    int src = source();
    if (src != C.src && *head == 0)
        head = "the source changed: ";
    C.src = src;
    int ts = temp_state(b.ok ? b.temp_c10 : -1000, C.ts);
    if (ts != C.ts && *head == 0)
        head = "the cell's temperature band changed: ";
    C.ts = ts;
    if (rd(R_DTLS1, &d1)) {
        int dt = d1 & 0xf;
        if (dt == 0xb && C.dtls != 0xb) {
            ++C.expired;
            sb_log("charge: CHG_DTLS 0x0B (the charger's watchdog expired): fed");
        }
        C.dtls = dt;
    }
    /* AICL: the source cannot hold its voltage at this current */
    if (!(ok & 0x80U)) {
        if (++C.aicl_low >= 2 && C.aicl_cap > IL_MIN_MA) {
            C.aicl_cap = C.aicl_cap - 100 < IL_MIN_MA ? IL_MIN_MA : C.aicl_cap - 100;
            C.aicl_low = 0;
            if (*head == 0)
                head = "the source sags (AICL): input limit lowered: ";
        }
    } else {
        C.aicl_low = 0;
    }
    int il, cc;
    table(src, &il, &cc);
    if (il > C.aicl_cap)
        il = C.aicl_cap;
    if (ts == TS_WARM && cc > 1090)
        cc = 1090;
    if (ts == TS_COOL && cc > 700)
        cc = 700;
    if (ts == TS_UNREAD && cc > 500)
        cc = 500;
    if (il > IL_MAX_MA)
        il = IL_MAX_MA;
    if (cc > CC_MAX_MA)
        cc = CC_MAX_MA;
    if (rd(R_CNFG09, &c9)) {
        uint8_t code = (uint8_t)(il / 25 - 1);
        if ((c9 & 0x7fU) != code)
            (void)wr(R_CNFG09, (uint8_t)((c9 & 0x80U) | code), 0x7fU, "charge: the input current limit");
        if (rd(R_CNFG09, &c9))
            C.il_ma = il_of(c9);
    }
    if (rd(R_CNFG02, &c2)) {
        uint8_t code = (uint8_t)(cc / 50);
        if ((c2 & 0x3fU) != code)
            (void)wr(R_CNFG02, (uint8_t)((c2 & 0xc0U) | code), 0x3fU, "charge: the fast charge current");
        if (rd(R_CNFG02, &c2))
            C.cc_ma = cc_of(c2);
    }
    /* stop / restart for the temperature: only between the modes Starbite OS knows (CHG+BUCK <-> BUCK) */
    if (ts == TS_HOT || ts == TS_COLD) {
        if (!C.mode_off && (c0 & 0x0fU) == 0x05U &&
            wr(R_CNFG00, (uint8_t)(c0 & ~0x01U), 0x0fU, "charge: charging stopped (the cell's temperature)"))
            C.mode_off = 1;
    } else if (C.mode_off) {
        mode_back("the cell's temperature is back in range");
    }
    if (*head || !C.last_log || now - C.last_log >= LOG_MS) {
        C.last_log = now;
        say(head, &b);
    }
}

static void before_reset(void)
{
    mode_back("a restart or power-off");
    if (prev_before_reset)
        prev_before_reset();
}

void charge_timer(unsigned id)
{
    (void)id;
    tick();
}

void charge_init(void)
{
    if (C.inited)
        return;
    C.inited = 1;
    C.ts = TS_OK;
    C.aicl_cap = IL_MAX_MA;
    uint8_t c0 = 0, c2 = 0, c9 = 0, c12 = 0, d1 = 0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "charge: at the start CNFG_00 ");
    if (rd(R_CNFG00, &c0) && rd(R_CNFG02, &c2) && rd(R_CNFG09, &c9) && rd(R_CNFG12, &c12) && rd(R_DTLS1, &d1)) {
        ub_hex(&u, c0, 2);
        ub_s(&u, (c0 & 0x10U) ? " (watchdog on: fed every 10 s)" : " (watchdog off)");
        ub_s(&u, ", input limit ");
        ub_dec(&u, (uint64_t)il_of(c9));
        ub_s(&u, " mA, charge limit ");
        ub_dec(&u, (uint64_t)cc_of(c2));
        ub_s(&u, " mA, CNFG_12 ");
        ub_hex(&u, c12, 2);
        ub_s(&u, ", CHG_DTLS ");
        ub_hex(&u, d1 & 0xfU, 1);
    } else {
        ub_s(&u, "not read");
    }
    sb_log(u.b);
    prev_before_reset = sb_before_reset;
    sb_before_reset = before_reset;
    kt_set(KT_CHARGE, 0, sb_ms(), TICK_MS, 1);
    tick();
}

void charge_get(struct kcharge *k)
{
    k->in = C.in;
    k->src = C.in ? C.src : 0;
    k->stopped_hot = C.in && C.ts == TS_HOT;
    k->stopped_cold = C.in && C.ts == TS_COLD;
    k->il_ma = C.il_ma;
    k->cc_ma = C.cc_ma;
    k->dtls = C.dtls;
    k->iin_ma = C.iin_ma;
}
