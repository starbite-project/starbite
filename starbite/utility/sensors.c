/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS Sensors: what the phone can tell without powering anything up. Read only: the fuel gauge and
 * the charger through libsb's MAX77705 readers, the TMU probes and the thermistors (libsb tmu.c, adc.c),
 * the two hall sensors' pins (ALIVE GPIO DAT registers), the keys, every finger on the touchscreen.
 * The motion sensors, heart rate, grip and NFC need power rails or firmware first: listed with the reason
 * (docs/UTILITY-OS.md, "Utility OS 2"). */
#include "sb.h"
#include "sbui.h"
#include "sbui_w.h"
#include "ut.h"
#include "mt.h"

#define ALIVE_GPA0_DAT 0x14050024UL     /* gpa0-2: the second ("certify") hall sensor */
#define ALIVE_GPA3_DAT 0x14050084UL     /* gpa3-6: the flip-cover hall sensor */

static uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

/* ---- charts ------------------------------------------------------------------------------------------------ */
struct schart { struct ui_chart c; const char *title; uint32_t col; };
static struct schart ch[6];             /* 0 current, 1 voltage, 2 hottest CPU, 3 battery temperature */

static void draw_chart(const struct pitem *it, int x, int y, int w, int focused)
{
    (void)focused;
    const struct schart *c = &ch[it->arg];
    ui_round(x, y, w, it->h, 36, C_CARD);
    ui_text(F_HEAD, x + 44, y + 64, c->title, C_L2);
    chart_draw(&c->c, x + 24, y + 90, w - 48, it->h - 120, c->col, C_CARD, "");
}

static void kv_chart(struct page *p, unsigned i)
{
    const struct ui_chart *c = &ch[i].c;
    uint32_t hash = (uint32_t)c->n * 2654435761U ^ (uint32_t)c->head * 40503U ^
                    (uint32_t)(c->n ? c->v[(c->head + CHART_N - 1) % CHART_N] : 0) ^ i;
    page_custom(p, 460, draw_chart, i, hash);
}

static void c10(struct ub *u, int v)                  /* 0.1 C -> "31.4 C" */
{
    if (v < 0) {
        ub_c(u, '-');
        v = -v;
    }
    ub_dec(u, (uint64_t)v / 10);
    ub_c(u, '.');
    ub_dec(u, (uint64_t)v % 10);
    ub_s(u, " " U_DEG "C");
}

/* ---- battery ------------------------------------------------------------------------------------------------- */
static int fgw(uint8_t reg, unsigned lo, unsigned hi)
{
    uint16_t v;
    return sb_fg_word_checked(reg, &v, lo, hi) ? (int)v : -1;
}

static void mins(struct ub *u, unsigned s)
{
    if (s >= 3600) {
        ub_dec(u, s / 3600);
        ub_s(u, " h ");
    }
    ub_dec(u, s / 60 % 60);
    ub_s(u, " min");
}

static void battery_fill(struct page *p)
{
    struct sb_bat b;
    struct ub u;
    page_hdr(p, "Now");
    if (!sb_bat_read(&b)) {
        page_kv(p, "Fuel gauge", "the MAX77705 did not answer", C_ORANGE);
        return;
    }
    ub_init(&u);
    ub_fix(&u, (uint64_t)b.soc_x10 * 100U, 1);
    ub_s(&u, " %");
    page_big(p, "CHARGE", u.b, b.soc_pct < 15 ? C_RED : C_L1);
    int rep = fgw(0x05, 0, 6000), full = fgw(0x35, 500, 6000), nom = fgw(0x23, 500, 6000);
    int design = fgw(0x18, 500, 6000), cyc = fgw(0x17, 0, 0), tte = fgw(0x11, 0, 0), ttf = fgw(0x20, 0, 0);
    if (rep >= 0 && full > 0) {
        ub_init(&u);
        ub_dec(&u, (unsigned)rep);
        ub_s(&u, " of ");
        ub_dec(&u, (unsigned)full);
        ub_s(&u, " mAh");
        page_kv(p, "Remaining", u.b, C_L1);
    }
    ub_init(&u);
    ub_fix(&u, (uint64_t)b.mv, 3);
    ub_s(&u, " V (average ");
    ub_fix(&u, (uint64_t)b.avg_mv, 3);
    ub_s(&u, ")");
    page_kv(p, "Voltage", u.b, C_L1);
    ub_init(&u);
    ub_sdec(&u, b.ma);
    ub_s(&u, " mA (average ");
    ub_sdec(&u, b.avg_ma);
    ub_s(&u, ")");
    page_kv(p, b.avg_ma >= 0 ? "Current in" : "Current out", u.b, b.avg_ma > 0 ? C_GREEN : C_L1);
    ub_init(&u);
    ub_sdec(&u, (int64_t)b.avg_mv * b.avg_ma / 1000);
    ub_s(&u, " mW");
    page_kv(p, "Power", u.b, C_L1);
    /* TTE / TTF: 5.625 s per step (the ModelGauge m5 family), 0xffff = not known */
    if (b.avg_ma < -20 && tte > 0 && tte < 0xffff) {
        ub_init(&u);
        mins(&u, (unsigned)tte * 45U / 8U);
        page_kv(p, "Time to empty", u.b, C_L2);
    } else if (b.avg_ma > 20 && ttf > 0 && ttf < 0xffff) {
        ub_init(&u);
        mins(&u, (unsigned)ttf * 45U / 8U);
        page_kv(p, "Time to full", u.b, C_L2);
    }
    page_hdr(p, "Health");
    if (design > 0) {
        ub_init(&u);
        ub_dec(&u, (unsigned)design);
        ub_s(&u, " mAh");
        page_kv(p, "Design capacity", u.b, C_L1);
    }
    if (nom > 0 && design > 0) {
        unsigned h = (unsigned)nom * 1000U / (unsigned)design;
        ub_init(&u);
        ub_fix(&u, h * 100U, 1);
        ub_s(&u, " % (");
        ub_dec(&u, (unsigned)nom);
        ub_s(&u, " mAh when new-equivalent full)");
        page_kv(p, "Health", u.b, h >= 800 ? C_GREEN : h >= 650 ? C_ORANGE : C_RED);
    }
    if (cyc >= 0) {
        ub_init(&u);
        ub_fix(&u, (uint64_t)cyc * 10U, 2);
        ub_s(&u, " full cycles");
        page_kv(p, "Charge cycles", u.b, C_L1);
    }
    page_note(p, "From the fuel gauge's own model (1 mAh per step; cycles in hundredths). Read only.", C_L3);
    page_hdr(p, "Live");
    kv_chart(p, 0);
    kv_chart(p, 1);
}

/* ---- temperatures ------------------------------------------------------------------------------------------- */
/* the last sample (sample(): once a second): the page shows these instead of converting again */
static struct sb_temps s_t;
static struct sb_therm s_th[8];
static int s_n = -1;
static const char *s_why;

static void temps_fill(struct page *p)
{
    const struct sb_temps *tp = &s_t;
    const struct sb_therm *th = s_th;
    struct ub u;
#define t (*tp)
    page_hdr(p, "Processor (TMU)");
    static const char *const nm[5] = { "Big cores, probe 1", "Big cores, probe 2", "Big cores, probe 3",
                                       "Big cores, probe 4", "Big cores, probe 5" };
    for (int i = 0; i < 5; ++i) {
        if (t.big[i] <= -100)
            continue;
        ub_init(&u);
        c10(&u, t.big[i] * 10);
        page_kv(p, nm[i], u.b, t.big[i] >= 80 ? C_RED : t.big[i] >= 60 ? C_ORANGE : C_L1);
    }
    static const char *const more[3] = { "Little cores", "GPU", "ISP" };
    for (unsigned i = 0; i < 3; ++i) {
        int v = i == 0 ? t.little : i == 1 ? t.gpu : t.isp;
        if (v <= -100)
            continue;
        ub_init(&u);
        c10(&u, v * 10);
        page_kv(p, more[i], u.b, v >= 80 ? C_RED : v >= 60 ? C_ORANGE : C_L1);
    }
    page_hdr(p, "Thermistors");
    int n = s_n;
    if (n < 0)
        page_kv(p, "ADC", s_why ? s_why : "not readable", C_ORANGE);
    for (int i = 0; i < n; ++i) {
        ub_init(&u);
        if (th[i].ok)
            c10(&u, th[i].c10);
        else
            ub_s(&u, th[i].why ? th[i].why : "no sample");
        page_kv(p, th[i].name, u.b, !th[i].ok ? C_L3 : th[i].c10 >= 450 ? C_ORANGE : C_L1);
    }
    page_hdr(p, "Live");
    kv_chart(p, 2);
    kv_chart(p, 3);
    page_note(p, "TMU: the processor's own probes (median of 3). Thermistors: the board's NTCs on the ADC, "
                 "with the vendor's tables.", C_L3);
#undef t
}

static void sample(void)
{
    struct sb_bat b;
    if (sb_bat_read(&b)) {
        chart_push(&ch[0].c, b.ma);
        chart_push(&ch[1].c, b.mv);
    }
    sb_temps_read(&s_t);
    if (s_t.hottest > -100)
        chart_push(&ch[2].c, s_t.hottest);
    s_why = 0;
    s_n = sb_therms_read(s_th, 8, &s_why);
    for (int i = 0; i < s_n; ++i)
        if (s_th[i].ok && s_th[i].ch == 4)            /* the battery's NTC */
            chart_push(&ch[3].c, s_th[i].c10 / 10);
}

/* ---- the multi-touch test --------------------------------------------------------------------------------- */
static void touch_test(void)
{
    static const uint32_t col[MT_N] = { 0xff6fb3ffU, 0xff30d158U, 0xffff9f0aU, 0xffff453aU, 0xffffd60aU,
                                        0xffbf5af2U, 0xff64d2ffU, 0xffff375fU, 0xffac8e68U, 0xffffffffU };
    struct mt m;
    struct mt_ev ev[MT_N];
    int lx[MT_N], ly[MT_N], most = 0;
    mt_reset(&m);
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    uint64_t next = 0;
    for (;;) {
        int n = mt_poll(&m, ev, MT_N);
        ut_awake = ui_idle_ms() < UT_WATCH_MS;        /* touches restart the idle time (mt.c) */
        if ((m.pressed & SB_K_BIXBY) || mt_held_ms(&m, SB_K_POWER) >= UI_POWER_LONG_MS)
            break;
        if (m.pressed & (SB_K_UP | SB_K_DOWN)) {      /* Volume: clear */
            ui_fill(0, 0, UI_W, UI_H, C_BG);
            ui_dirty_all();
            most = 0;
            next = 0;
        }
        for (int e = 0; e < n; ++e) {
            int f = ev[e].id, x = ev[e].x, y = ev[e].y;
            if (ev[e].st == MT_DOWN) {
                ui_ring(x, y, 70, 6, col[f]);
                ui_dirty(x - 80, y - 80, 160, 160);
            } else if (ev[e].st == MT_MOVE) {
                ui_line(lx[f], ly[f], x, y, 10, col[f]);
                int x0 = lx[f] < x ? lx[f] : x, y0 = ly[f] < y ? ly[f] : y;
                int w = lx[f] < x ? x - lx[f] : lx[f] - x, h = ly[f] < y ? y - ly[f] : ly[f] - y;
                ui_dirty(x0 - 12, y0 - 12, w + 24, h + 24);
            } else {
                ui_disc(x, y, 22, col[f]);
                ui_dirty(x - 24, y - 24, 48, 48);
            }
            lx[f] = x;
            ly[f] = y;
            next = 0;
        }
        int c = mt_count(&m);
        if (c > most)
            most = c;
        if (sb_ms() >= next) {
            struct ub u;
            ui_fill(0, 0, UI_W, 330, C_BG);
            ub_init(&u);
            ub_dec(&u, (unsigned)c);
            ub_s(&u, c == 1 ? " finger" : " fingers");
            ub_s(&u, "  (most at once: ");
            ub_dec(&u, (unsigned)most);
            ub_s(&u, ")");
            ui_text(F_TITLE_S, 60, 150, u.b, C_L1);
            ub_init(&u);
            for (int f = 0; f < MT_N; ++f)
                if (m.down[f]) {
                    ub_dec(&u, (unsigned)f);
                    ub_c(&u, ':');
                    ub_dec(&u, (unsigned)m.x[f]);
                    ub_c(&u, ',');
                    ub_dec(&u, (unsigned)m.y[f]);
                    ub_s(&u, "  ");
                }
            ui_text(F_MONO, 60, 230, u.b[0] ? u.b : "touch with up to 10 fingers", C_L2);
            ui_text(F_SMALL, 60, 300, "Volume: clear   Bixby: back", C_L3);
            ui_dirty(0, 0, UI_W, 330);
            next = sb_ms() + 100;
        }
        ui_present();
    }
    ut_awake = 0;
    ui_input_reset();
}

/* ---- the Sensors page ---------------------------------------------------------------------------------------- */
enum { S_MAIN, S_BAT, S_TEMP };
static int r_bat, r_temp, r_touch;

static void main_fill(struct page *p)
{
    struct sb_bat b;
    struct sb_temps t;
    struct ub u;
    page_hdr(p, "Live");
    ub_init(&u);
    if (sb_bat_read(&b)) {
        ub_dec(&u, (unsigned)b.soc_pct);
        ub_s(&u, " %, ");
        ub_sdec(&u, b.avg_ma);
        ub_s(&u, " mA");
    } else {
        ub_s(&u, "fuel gauge not answering");
    }
    r_bat = page_row(p, "Battery", u.b, 0);
    sb_temps_read(&t);
    ub_init(&u);
    if (t.hottest > -100) {
        ub_s(&u, "hottest core ");
        c10(&u, t.hottest * 10);
    } else {
        ub_s(&u, "TMU not running");
    }
    r_temp = page_row(p, "Temperatures", u.b, 0);
    int cover = !(rd32(ALIVE_GPA3_DAT) & (1U << 6)), second = !(rd32(ALIVE_GPA0_DAT) & (1U << 2));
    page_kv(p, "Flip-cover hall sensor", cover ? "pin low (magnet near?)" : "pin high (no magnet?)",
            cover ? C_STAR : C_L1);
    page_kv(p, "Second hall sensor", second ? "pin low (magnet near?)" : "pin high (no magnet?)",
            second ? C_STAR : C_L1);
    unsigned k = sb_keys();
    ub_init(&u);
    if (!k)
        ub_s(&u, "none");
    if (k & SB_K_UP)
        ub_s(&u, "Volume Up  ");
    if (k & SB_K_DOWN)
        ub_s(&u, "Volume Down  ");
    if (k & SB_K_BIXBY)
        ub_s(&u, "Bixby  ");
    if (k & SB_K_POWER)
        ub_s(&u, "Power");
    page_kv(p, "Keys held", u.b, k ? C_STAR : C_L2);
    r_touch = page_row(p, "Multi-touch test", "every finger with its trail, up to 10", 0);
    page_hdr(p, "Need power or firmware first");
    page_row(p, "Motion: accelerometer, gyro, compass", "the BCM4775 sensor hub; firmware from the stock kernel",
             R_DIS | R_NOCHEV);
    page_row(p, "Barometer, light, proximity", "behind the same sensor hub", R_DIS | R_NOCHEV);
    page_row(p, "Heart rate (MAX86915)", "two power rails to switch on (with you present)", R_DIS | R_NOCHEV);
    page_row(p, "Grip (SX9320)", "a second I2C bus in the touchscreen's block", R_DIS | R_NOCHEV);
    page_note(p, "Fingerprint and iris belong to the secure world; GPS speaks a closed protocol. "
                 "NFC: the Radio tile.", C_L3);
}

void ut_sensors(void)
{
    struct page p;
    int view = S_MAIN, focus = 0;
    static const char *const title[3] = { "Sensors", "Battery", "Temperatures" };
    ch[0].title = "BATTERY CURRENT (mA, + = charging)";
    ch[0].col = C_GREEN;
    ch[1].title = "BATTERY VOLTAGE (mV)";
    ch[1].col = C_STAR;
    ch[2].title = "HOTTEST CORE (" U_DEG "C)";
    ch[2].col = C_ORANGE;
    ch[3].title = "BATTERY THERMISTOR (" U_DEG "C)";
    ch[3].col = C_YELLOW;
    page_init(&p, UI_VIEW_Y0, UI_H);
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    uint64_t next = 0, next_sample = 0;
    for (;;) {
        uint64_t now = sb_ms();
        if (now >= next_sample) {
            sample();                                 /* the charts fill while the tile is open */
            next_sample = now + 1000;
        }
        if (now >= next) {
            ui_fill(0, 0, UI_W, UI_VIEW_Y0, C_BG);
            ui_header_draw(title[view], view ? "Sensors" : "Utility", "LIVE", C_GREEN);
            ut_status_bar();
            page_begin(&p);
            if (view == S_BAT)
                battery_fill(&p);
            else if (view == S_TEMP)
                temps_fill(&p);
            else
                main_fill(&p);
            page_end(&p);
            page_set_focus(&p, view == S_MAIN ? focus : -1);
            page_redraw(&p);
            ui_present();
            next = now + (view == S_MAIN ? 250 : 1000);
        }
        struct ui_ev e;
        ui_poll(&e, 0);
        ut_service();
        ut_awake = view != S_MAIN && ui_idle_ms() < UT_WATCH_MS;   /* live charts: watched, for a while */
        if (ut_back_event(&e)) {
            if (view == S_MAIN)
                break;
            view = S_MAIN;
            page_free(&p);
            page_init(&p, UI_VIEW_Y0, UI_H);
            ui_fill(0, 0, UI_W, UI_H, C_BG);
            next = 0;
            continue;
        }
        int pick = -1;
        if (page_scroll_event(&p, &e)) {
            ;
        } else if (view == S_MAIN && (e.type == EV_UP || e.type == EV_DOWN)) {
            int f = page_next_row(&p, focus, e.type == EV_UP ? -1 : 1);
            if (f >= 0)
                focus = f;
            next = 0;
        } else if (e.type == EV_UP || e.type == EV_DOWN) {
            page_set_scroll(&p, p.scroll + (e.type == EV_UP ? -400 : 400));
            ui_present();
        } else if (view == S_MAIN && e.type == EV_SELECT) {
            pick = focus;
        } else if (view == S_MAIN && e.type == EV_TAP) {
            pick = page_hit(&p, e.x, e.y, 0);
        }
        if (pick >= 0) {
            focus = pick;
            if (pick == r_touch) {
                touch_test();
            } else if (pick == r_bat || pick == r_temp) {
                view = pick == r_bat ? S_BAT : S_TEMP;
                page_free(&p);
                page_init(&p, UI_VIEW_Y0, UI_H);
            }
            ui_input_reset();
            ui_fill(0, 0, UI_W, UI_H, C_BG);
            next = 0;
        }
        page_fling_step(&p);
    }
    ut_awake = 0;
    page_free(&p);
}
