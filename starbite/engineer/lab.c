/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite Lab tests (Engineer OS v2, docs/ENGINEER-OS.md section 6): CPU stress on every core,
 * memory stress, display stress, the touch grid, the storage benchmark, the thermal test, the
 * battery tests, every thermal sensor live, and the full health check.
 *
 * Every test: a setup page (options cycle with Power), then a live page (graphs and readouts,
 * rebuilt 4 times a second; only what changed is redrawn), then a result page; the result goes to
 * the test history (pmsg ring) and to the report that Tools > Save report writes into the OS store.
 * Every test stops at once on Power held 0.6 s (or Bixby, a swipe right, the back button), and on
 * its guards: the temperature limit (85 C by default), the battery (20 %, 3.45 V, 45 C for the
 * battery tests), a worker core that faults, the time limit. Every hardware wait below is bounded.
 */
#include "eng.h"
#include "oc.h"

/* ---- options shared by the load tests ------------------------------------------------------ */

static const unsigned dur_s[] = { 30, 60, 300, 900, 1800, 3600, 0 };
static const char *const dur_name[] = { "30 s", "1 min", "5 min", "15 min", "30 min", "60 min", "until stopped" };
static const int limit_c[LIMIT_N] = { 70, 75, 80, 85, 90, 95, 1000 };
static const char *const core_name[] = { "all 8 (A55 + M3)", "the 4 A55 cores", "the 4 M3 cores + cpu0", "cpu0 only" };
static const unsigned core_mask[] = { 0xffU, 0x0fU, 0xf1U, 0x01U };
/* 2.2: the heat kernel first (the default): the old default mix spent most of each batch waiting on DRAM in
 * the memory kernel, one reason the stress stayed cool (docs/STARBITE-OS.md 8.5) */
#define KERN_N 6
static const char *const kern_name[KERN_N] = { "heat (FMA streams, most power)", "INT + FP + NEON + memory", "integer",
                                               "floating point", "NEON", "memory (caches)" };
static const unsigned kern_mask[KERN_N] = { KERN_HEAT, KERN_INT | KERN_FP | KERN_NEON | KERN_MEM, KERN_INT, KERN_FP,
                                            KERN_NEON, KERN_MEM };

static struct {
    int cores, kern, dur, limit, clk;
} opt = { 0, 0, 1, 3, 0 };
static const char *const clk_name[2] = { "as S-Boot left them (about 1050 MHz)", "full: 1794 MHz (tier 3, ACPM)" };

static int nxt(int v, int n)
{
    return (v + 1) % n;
}

/* ---- the temperature limit, and "no limit" ----------------------------------------------------------
 * "No limit" is offered only through a warning that shows what still protects the SoC, read live
 * from the TMU: whether S-Boot armed the hardware trip (CONTROL bit 12, THERM_TRIP_EN) on TMU_TOP and
 * TMU_SUB and the threshold it programmed (THD_TEMP0 [24:16], decoded with probe 0's trim: 115 C on
 * this phone in the host model and in S-Boot's disassembly). What the trip does when it fires (the
 * TMU signals the PMU, which cuts the power) is the vendor's description: never seen here. */
int lab_limit_c(int idx)
{
    return idx >= 0 && idx < LIMIT_N ? limit_c[idx] : 85;
}

const char *lab_limit_label(int idx)
{
    static const char *const l[LIMIT_N] = { "70 " U_DEG "C", "75 " U_DEG "C", "80 " U_DEG "C", "85 " U_DEG "C",
                                            "90 " U_DEG "C", "95 " U_DEG "C", "no limit (hardware trip only)" };
    return idx >= 0 && idx < LIMIT_N ? l[idx] : "?";
}

static char nl_top[96], nl_sub[96];

static void trip_line(char *out, const char *name, int armed, int c)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, name);
    if (armed && c > -100) {
        ub_s(&u, " hardware trip ARMED (CONTROL bit 12), threshold decodes to ");
        ub_sdec(&u, c);
        ub_s(&u, " " U_DEG "C");
    } else if (armed) {
        ub_s(&u, " hardware trip armed, threshold not decodable");
    } else {
        ub_s(&u, " hardware trip NOT armed: nothing on that TMU stops the heat");
    }
    sb_strcpy(out, 96, u.b);
}

static int nolimit_confirm(void)
{
    struct sb_temps t;
    struct sb_auth au;
    sb_temps_read(&t);
    trip_line(nl_top, "TMU_TOP (CPU):", t.top && (t.trip_armed & 1), t.trip_c);
    trip_line(nl_sub, "TMU_SUB (GPU):", t.sub && (t.trip_armed & 2), t.trip_sub_c);
    int none = !(t.trip_armed & 3);
    struct action a = { "No temperature limit", "NOLIMIT",
                        { "The load runs until you stop it or the time ends, however hot the SoC gets", nl_top, nl_sub,
                          none ? "NO hardware trip is armed: only you stop it. Watch the phone."
                               : "Read from the registers now. Not verified on this phone: that the trip fires, "
                                 "and which probes it watches (the vendor says: the PMU then cuts the power)" },
                        "set a limit again (Power on Stop at); a reboot sets 85 C", "", "no limit", "", TIER1,
                        SB_SP_MEM, 0, 0, 0, 0 };
    if (!lab_confirm(&a, &au))
        return 0;
    audit_line(none ? "no temperature limit chosen; no hardware trip armed" : "no temperature limit chosen");
    return 1;
}

int lab_limit_next(int idx)
{
    int n = (idx + 1) % LIMIT_N;
    if (n == LIMIT_NONE && !nolimit_confirm())
        n = 0;
    return n;
}

/* Called once a second by the load loops. With a limit set, 3 s in a row without one valid TMU
 * sample (every probe rejected: a broken sensor, not a glitch) stops the load too: a limit that
 * cannot see is no limit. */
static struct sb_temps T;                   /* (defined with the temperatures below) */

static int over_limit(int t)
{
    static unsigned blind;
    if (opt.limit == LIMIT_NONE)
        return 0;
    if (t <= -100 && T.top) {                   /* TMU running, but no probe gave a valid sample */
        if (++blind >= 3) {
            audit_line("no valid TMU sample for 3 s: the load stops (the limit cannot see)");
            blind = 0;
            return 1;
        }
        return 0;
    }
    blind = 0;
    return t > -100 && t >= limit_c[opt.limit];
}

/* "85 C: the test stops there" or what protects the phone without a limit */
static void limit_text(struct ub *u)
{
    if (opt.limit == LIMIT_NONE) {
        struct sb_temps t;
        sb_temps_read(&t);
        ub_s(u, "none: ");
        if (t.trip_armed & 1) {
            ub_s(u, "the hardware trip (");
            ub_sdec(u, t.trip_c);
            ub_s(u, " " U_DEG "C, decoded) only");
        } else {
            ub_s(u, "no hardware trip armed");
        }
        return;
    }
    ub_dec(u, (uint64_t)limit_c[opt.limit]);
    ub_s(u, " " U_DEG "C: the test stops there");
}

/* ---- the report: every result of this session as text ---------------------------------------- */

#define REP_N 160
static char rep[REP_N][96];
static unsigned rep_n;

static void rep_line(const char *s)
{
    if (rep_n < REP_N)
        sb_strcpy(rep[rep_n++], sizeof(rep[0]), s);
}

unsigned lab_report_lines(char (*out)[96], unsigned max)
{
    unsigned n = 0;
    for (; n < rep_n && n < max; ++n)
        sb_strcpy(out[n], 96, rep[n]);
    return n;
}

void result_line(const char *name, int pass, const char *detail);

static void result(const char *name, int pass, const char *detail)
{
    struct ub u;
    test_record(name, pass, detail);
    ub_init(&u);
    ub_s(&u, pass ? "PASS " : "FAIL ");
    ub_s(&u, name);
    ub_s(&u, ": ");
    ub_s(&u, detail);
    rep_line(u.b);
}

void result_line(const char *name, int pass, const char *detail)
{
    result(name, pass, detail);
}

/* ---- the live page ---------------------------------------------------------------------------- */

static struct page live_p;
static const char *live_title;
static uint64_t live_next;
static int live_open;

static void live_begin(const char *title)
{
    live_title = title;
    page_init(&live_p, UI_VIEW_Y0, UI_VIEW_Y1);
    screen_push(title);
    chrome(title, "RUNNING", C_ORANGE);
    live_open = 1;
    live_next = 0;
    ui_input_reset();
    lab_stop_reset();
}

static void live_end(void)
{
    if (!live_open)
        return;
    live_open = 0;
    screen_pop();
    page_free(&live_p);
    ui_input_reset();
}

/* Poll input, redraw every ms; 1 when the user asked to stop. */
static int live_poll(void (*fill)(void), unsigned every_ms)
{
    struct ui_ev e;
    int stop = 0;
    ui_poll(&e, 0);
    if (e.type == EV_BACK || e.type == EV_HOME || e.type == EV_SWIPE_R)
        stop = 1;
    else if (e.type == EV_TAP && (ui_header_back_hit(e.x, e.y) || e.y >= UI_TAB_Y))
        stop = 1;
    else if (!page_scroll_event(&live_p, &e)) {
        if (e.type == EV_UP)
            page_set_scroll(&live_p, live_p.scroll - 560);
        else if (e.type == EV_DOWN)
            page_set_scroll(&live_p, live_p.scroll + 560);
    }
    if (lab_stop_check())
        stop = 1;
    if (E.redraw) {
        chrome(live_title, "RUNNING", C_ORANGE);
        page_redraw(&live_p);
    }
    page_fling_step(&live_p);
    if (sb_ms() >= live_next) {
        eng_build_page(&live_p, fill);
        ui_present();
        live_next = sb_ms() + every_ms;
    }
    return stop;
}

/* ---- charts ---------------------------------------------------------------------------------- */

struct lchart {
    struct ui_chart c;
    const char *title;
    uint32_t col;
    unsigned every, acc_n;                  /* decimation: one point per `every` samples (the max) */
    int32_t acc;
};
#define CH_TEMP 0
#define CH_RATE 1
#define CH_MV 2
#define CH_MA 3
#define CH_BW 4
#define CH_N 5
static struct lchart ch[CH_N];

static void ch_reset(unsigned i, const char *title, uint32_t col, unsigned total_samples)
{
    struct lchart *c = &ch[i];
    c->c.n = c->c.head = 0;
    c->title = title;
    c->col = col;
    c->every = total_samples > CHART_N ? (total_samples + CHART_N - 1) / CHART_N : 1;
    c->acc_n = 0;
}

static void ch_push(unsigned i, int32_t v)
{
    struct lchart *c = &ch[i];
    if (!c->acc_n || v > c->acc)
        c->acc = v;
    if (++c->acc_n >= c->every) {
        chart_push(&c->c, c->acc);
        c->acc_n = 0;
    }
}

static void draw_lchart(const struct pitem *it, int x, int y, int w, int focused)
{
    (void)focused;
    const struct lchart *c = &ch[it->arg];
    ui_text(F_HEAD, x + 44, y + 64, c->title, C_L2);
    chart_draw(&c->c, x + 24, y + 90, w - 48, it->h - 120, c->col, C_CARD, "");
}

static void kv_chart(unsigned i, int h)
{
    const struct ui_chart *c = &ch[i].c;
    uint32_t hash = (uint32_t)c->n * 2654435761U ^ (uint32_t)c->head * 40503U ^
                    (uint32_t)(c->n ? c->v[(c->head + CHART_N - 1) % CHART_N] : 0) ^ (uint32_t)i;
    kv_custom(h, draw_lchart, i, hash);
}

/* For gpu.c: the same live page and charts. */
void lab_live_begin(const char *title)
{
    live_begin(title);
}

int lab_live_poll(void (*fill)(void), unsigned every_ms)
{
    return live_poll(fill, every_ms);
}

void lab_live_end(void)
{
    live_end();
}

void lab_chart_reset(unsigned i, const char *title, uint32_t col, unsigned total_samples)
{
    if (i < CH_N)
        ch_reset(i, title, col, total_samples);
}

void lab_chart_push(unsigned i, int32_t v)
{
    if (i < CH_N)
        ch_push(i, v);
}

void lab_chart_kv(unsigned i, int h)
{
    if (i < CH_N)
        kv_chart(i, h);
}

/* ---- small formatting helpers --------------------------------------------------------------------- */

static void fmt_mmss(struct ub *u, uint64_t s)
{
    ub_dec(u, s / 60U);
    ub_c(u, ':');
    ub_dec2(u, (unsigned)(s % 60U));
}

static void kv_deg(const char *k, int t)
{
    struct ub u;
    ub_init(&u);
    if (t <= -100) {
        kv_addc(k, "off", C_L3);
        return;
    }
    ub_sdec(&u, t);
    ub_s(&u, " " U_DEG "C");
    kv_addc(k, u.b, t >= 85 ? C_RED : t >= 65 ? C_ORANGE : C_L1);
}

static void kv_deg10(const char *k, int c10, const char *note)
{
    struct ub u;
    ub_init(&u);
    if (c10 < 0) {
        ub_c(&u, '-');
        c10 = -c10;
    }
    ub_dec(&u, (uint64_t)c10 / 10U);
    ub_c(&u, '.');
    ub_dec(&u, (uint64_t)c10 % 10U);
    ub_s(&u, " " U_DEG "C");
    if (note) {
        ub_s(&u, "  ");
        ub_s(&u, note);
    }
    kv_add(k, u.b);
}

static const char *core_label(unsigned w)
{
    static const char *const n[SB_NCORES] = { "cpu0 A55 (this core)", "cpu1 A55", "cpu2 A55", "cpu3 A55",
                                              "cpu4 M3", "cpu5 M3", "cpu6 M3", "cpu7 M3" };
    return w < SB_NCORES ? n[w] : "?";
}

static const char *psci_name(int64_t r)
{
    switch (r) {
    case 0: return "running";
    case 1: return "not used";
    case -1: return "PSCI NOT_SUPPORTED";
    case -2: return "PSCI INVALID_PARAMETERS";
    case -3: return "PSCI DENIED";
    case -4: return "PSCI ALREADY_ON";
    case -5: return "PSCI ON_PENDING";
    case -9: return "PSCI INVALID_ADDRESS";
    case -100: return "no check-in in 1 s";
    case -101: return "faulted at start";
    case -102: return "no memory for its stack";
    default: return "PSCI error";
    }
}

/* ---- load on every core ------------------------------------------------------------------------ */

static struct kern_cfg K;
static int64_t core_res[SB_NCORES];
static uint64_t it0, err0, last_it[SB_NCORES], rate[SB_NCORES], rate_total;
unsigned lab_m3_max_cores = 4;              /* oc.c: Samsung's busy-core limit while M3 is overclocked */
static int m3_state;                        /* 0 not enabled, 1 enabled (confirmed), -1 the PMU write failed */
static unsigned run_mask;                   /* the cores the running test uses (after the offer) */
static struct sb_auth m3_auth;

/* The vendor PMU defaults the M3 cluster needs before CPU_ON (xnu patch 0304; the Linux S9+ port's
 * pmucal_lpm_init values): CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG [31:24] = 0x8d, then MEMORY_CLUSTER1
 * CPU0..3 and NONCPU OPTION [1:0] = 2. Written only when they differ. */
static const struct {
    uint32_t off, mask, val;
} m3_pmu[] = { { 0x1088, 0xff000000U, 0x8d000000U }, { 0x2114, 3, 2 }, { 0x2180, 3, 2 }, { 0x21ec, 3, 2 },
               { 0x2258, 3, 2 }, { 0x2384, 3, 2 } };

static int m3_enable(void);

static void m3_prep(void)
{
    for (unsigned i = 0; i < sizeof(m3_pmu) / sizeof(m3_pmu[0]); ++i) {
        uint64_t a = SB_PMU + m3_pmu[i].off;
        uint32_t v = 0;
        if (!sb_probe32(a, &v) || (v & m3_pmu[i].mask) == m3_pmu[i].val)
            continue;
        if (!lab_poke32(a, (v & ~m3_pmu[i].mask) | m3_pmu[i].val, &m3_auth))
            m3_state = -1;
    }
}

static int m3_confirm(void)
{
    const char *why;
    struct action a;
    a.title = "Start the M3 cores";
    a.target = "PMU";
    a.happens[0] = "Writes the vendor PMU defaults for the Mongoose M3 cluster: 6 registers, as Linux and iOS do";
    a.happens[1] = "on this phone before every M3 CPU_ON (PMU 0x1088, 0x2114, 0x2180, 0x21EC, 0x2258, 0x2384)";
    a.happens[2] = "Then, per M3 core, Samsung's FlexPMU words (CPU_INFORM, GRP2 bits, APM mailbox bit 24) and PSCI "
                   "CPU_ON; before CPU_OFF only the C2 hint (never a cluster power-down)";
    a.happens[3] = 0;
    a.undo = "a reboot resets the PMU; Engineer OS turns the cores off when the test ends";
    a.old_v = "";
    a.new_v = "";
    a.backup = "";
    a.tier = poke_tier(SB_PMU + 0x1088, &why);
    a.space = SB_SP_MEM;
    a.lu = 0;
    a.kind = 0;
    a.first = SB_PMU + 0x1088;
    a.count = 0x2388 - 0x1088;
    return lab_confirm(&a, &m3_auth);
}

/* 2.2: full CPU clocks for the load tests (docs/STARBITE-OS.md 8.5). ACPM DVFS domains 2 (A55) and 3 (M3) at
 * 1794 MHz, the vendor's all-core level (DT quad_freq / policy-max), the M3 domain only while M3 cores run;
 * tier 3, confirmed once per session. When the load stops: the M3 domain back to 1066 MHz before its cores go
 * off, the A55 domain to 1053 MHz (both table levels; S-Boot's ~1050 MHz is not one). */
static int clk_state;                       /* 0 not confirmed, 1 confirmed */
static int clk_a55_up, clk_m3_up;
static struct sb_auth clk_auth;

static int clk_confirm(void)
{
    const char *why;
    struct action a;
    a.title = "Full CPU clocks";
    a.target = "CPUCL";
    a.happens[0] = "ACPM DVFS requests: the A55 cluster (domain 2) and, while M3 cores run, the M3 cluster (3)";
    a.happens[1] = "to 1794 MHz, the vendor's all-core level; ACPM sets the PLL and the voltage from its table";
    a.happens[2] = "When the load stops: M3 back to 1066 MHz, A55 to 1053 MHz (table levels)";
    a.happens[3] = 0;
    a.undo = "the test restores them; a reboot too";
    a.old_v = "about 1050 MHz";
    a.new_v = "1794 MHz";
    a.backup = "";
    a.tier = dvfs_tier(2, 1794000, &why);
    a.space = SB_SP_DVFS;
    a.lu = 0;
    a.kind = 0;
    a.first = 2;
    a.count = 2;
    return lab_confirm(&a, &clk_auth);
}

static void clk_log(const char *what, int ok)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, what);
    ub_s(&u, ok ? ": ACPM answered; now " : ": ACPM did not answer; now ");
    ub_dec(&u, sb_a55_hz() / 1000000U);
    ub_s(&u, " MHz A55");
    if (sb_m3_on()) {
        int off;
        ub_s(&u, ", ");
        ub_dec(&u, sb_m3_hz(&off) / 1000000U);
        ub_s(&u, " MHz M3");
    }
    audit_line(u.b);
}

static void clk_raise(unsigned mask)
{
    if (!opt.clk || clk_state <= 0)
        return;
    if (!sb_gate_open(&clk_auth)) {
        audit_line("full clocks: the confirmation is no longer valid");
        return;
    }
    clk_a55_up = sb_acpm_dvfs(2, 1794000, SB_TIER3);
    if ((mask & 0xf0U) && sb_m3_on())
        clk_m3_up = sb_acpm_dvfs(3, 1794000, SB_TIER3);
    sb_gate_close();
    sb_mdelay(2);
    clk_log("full clocks requested", clk_a55_up);
}

static void clk_restore(int m3_only)
{
    if (!clk_a55_up && !clk_m3_up)
        return;
    if (!sb_gate_open(&clk_auth))
        return;
    if (clk_m3_up && sb_m3_on()) {
        (void)sb_acpm_dvfs(3, 1066000, SB_TIER3);
        clk_m3_up = 0;
    }
    if (!m3_only && clk_a55_up) {
        (void)sb_acpm_dvfs(2, 1053000, SB_TIER3);
        clk_a55_up = 0;
    }
    sb_gate_close();
    if (!m3_only)
        clk_log("clocks restored", 1);
}

static void load_alloc(unsigned kmask)
{
    K.mask = kmask;
    kern_gold(&K);
    if (kmask & KERN_MEM) {
        K.words = (512U << 10) / 8U;
        for (unsigned c = 0; c < SB_NCORES; ++c)
            if (!K.buf[c])
                K.buf[c] = sb_alloc(512U << 10);
    }
}

static void fill_starting(void)
{
    kv_hdr("STARTING");
    kv_add("Cores", "one at a time: PSCI CPU_ON, then each core checks in (1 s at most)");
    kv_add("M3 cores", m3_state > 0 ? "after the vendor PMU sequence" : "not started");
}

/* Start the requested cores (bit w = core w; bit 0 is this core, which works between frames). */
static unsigned load_start(unsigned mask, unsigned kmask)
{
    if (opt.clk && clk_state <= 0) {
        if (clk_confirm()) {
            clk_state = 1;
            audit_line("full CPU clocks confirmed for this session");
        } else {
            opt.clk = 0;
            audit_line("full CPU clocks not confirmed: the test runs at S-Boot's clocks");
        }
    }
    load_alloc(kmask);
    it0 = err0 = 0;
    for (unsigned w = 0; w < SB_NCORES; ++w) {
        last_it[w] = rate[w] = 0;
        core_res[w] = 1;
    }
    if ((mask & 0xf0U) && m3_state <= 0) {
        mask &= 0x0fU;
        audit_line("M3 cores not started (not enabled)");
    }
    {                                       /* Overclocking: Samsung's busy-core limit above 1794 MHz */
        unsigned keep = 0, n3 = 0;
        for (unsigned w = 4; w < SB_NCORES; ++w)
            if ((mask & (1U << w)) && n3 < lab_m3_max_cores) {
                keep |= 1U << w;
                ++n3;
            }
        if ((mask & 0xf0U) != keep)
            audit_line("M3 cores limited by the overclocked M3 step (Samsung's busy-core limit)");
        mask = (mask & 0x0fU) | keep;
    }
    sb_smp_m3_prep = m3_state > 0 ? m3_prep : 0;
    show_now("Starting cores", fill_starting);
    unsigned n = 0;
    core_res[0] = (mask & 1U) ? 0 : 1;
    for (unsigned w = 1; w < SB_NCORES; ++w)
        if (mask & (1U << w)) {
            core_res[w] = sb_smp_start(w, kern_worker, &K);
            n += core_res[w] == 0;
            if (w >= 4 && m3_state < 0)         /* the PMU write failed: do not try the others */
                for (unsigned v = w + 1; v < SB_NCORES; ++v)
                    mask &= ~(1U << v);
        }
    clk_raise(mask);
    ui_input_reset();
    lab_stop_reset();
    if (live_open) {                    /* a prompt or the start page drew over the live page */
        chrome(live_title, "RUNNING", C_ORANGE);
        live_p.drawn = 0;
        live_next = 0;
    }
    return n;
}

/* Without a running TMU the temperature limit cannot work: say so and ask before any load. */
static int guard_ok(const char *what)
{
    struct sb_temps t;
    sb_temps_read(&t);
    if (t.top && t.hottest > -100)
        return 1;
    audit_line("TMU off: the load would run without a temperature guard");
    return dialog_danger(what, "The CPU temperature sensors are off: no temperature limit.",
                         "Run it only while you watch the phone.", "RUN ANYWAY");
}

/* The M3 cores are offered when a test that uses them starts (never in the middle of one): enable
 * them now (the tier 3 PMU step, once per session), run on the A55 cores only, or cancel. */
static int offer_pick;
static const char *offer_test;

static void build_offer(void)
{
    kv_hdr("THIS TEST USES THE M3 CORES");
    kv_add("Test", offer_test);
    kv_add("M3 cores", m3_state < 0 ? "the last power-up write failed (see the audit log)" : "off (powered down)");
    kv_note("The four Mongoose M3 cores (cpu4-7) stay off until 6 PMU registers get the vendor values Linux and "
            "iOS write before every M3 CPU_ON. That is a PMU write: tier 3 (warning, type PMU, slide, hold Power "
            "3 s), once per session. It worked on this phone on 2026-11-06.");
    kv_hdr("CHOOSE");
    row_add("Enable the M3 cores now", "tier 3: the PMU sequence, then PSCI CPU_ON 0x100..0x103", R_RED);
    row_add("Run on the A55 cores only", "cpu0-3; nothing is written", 0);
    row_add("Cancel", "back to the test's page", 0);
}

static int act_offer(int i)
{
    offer_pick = i;
    return 1;
}

/* The OCP probe (oc.c): core 4 (the first M3) reads the GCU / OCP system registers (libsb sb_ocp_worker)
 * and stops. may_ask: 1 the M3 PMU step may be confirmed now (Lab's tier 3 "Start the M3 cores"),
 * 0 (the unattended self-test): only when S-Boot already left the six PMU registers at the vendor
 * values, so nothing is written. 1 read (v[0..5], see kern.c), 0 faulted (*esr), -1 not started
 * (*why); v[] as libsb ocp.c. */
int lab_oc_m3_probe(int may_ask, uint64_t v[6], uint64_t *esr, const char **why)
{
    unsigned diff = 0;
    for (unsigned i = 0; i < sizeof(m3_pmu) / sizeof(m3_pmu[0]); ++i) {
        uint32_t r = 0;
        if (!sb_probe32(SB_PMU + m3_pmu[i].off, &r) || (r & m3_pmu[i].mask) != m3_pmu[i].val)
            ++diff;
    }
    *esr = 0;
    if ((diff || may_ask) && m3_state <= 0) {
        if (!may_ask) {
            *why = "the M3 PMU registers are not at the vendor values (needs the confirmed PMU step)";
            return -1;
        }
        if (!m3_enable()) {
            *why = "the M3 PMU step was not confirmed";
            return -1;
        }
    }
    sb_smp_m3_prep = m3_state > 0 ? m3_prep : 0;
    struct sb_wslot *s = sb_smp_own_slot(4);
    if (s)
        for (unsigned k = 0; k < 8; ++k)
            s->v[k] = 0;
    int64_t r = sb_smp_start(4, sb_ocp_worker, 0);
    if (r) {
        *why = r == -100 ? "the M3 core did not check in" : "PSCI CPU_ON refused";
        return -1;
    }
    const struct sb_wslot *g = 0;
    for (unsigned ms = 0; ms < 1000; ++ms) {
        g = sb_smp_slot(4);
        if (g && (g->v[7] || g->state == SB_WK_FAULT || g->state == SB_WK_DONE || g->state == SB_WK_PARKED))
            break;
        sb_mdelay(1);
    }
    (void)sb_smp_stop(200);
    if (!g || g->state == SB_WK_FAULT || !g->v[7]) {
        *esr = g ? g->esr : 0;
        *why = "the read faulted (trapped) or did not finish";
        return 0;
    }
    for (unsigned k = 0; k < 6; ++k)
        v[k] = g->v[k];
    return 1;
}

static int m3_enable(void)
{
    if (m3_state > 0)
        return 1;
    if (!m3_confirm()) {
        audit_line("M3 cores not enabled (the PMU step was not confirmed)");
        return 0;
    }
    m3_state = 1;
    audit_line("M3 cores enabled for this session (the PMU step runs before their CPU_ON)");
    return 1;
}

static void m3_enable_now(void)
{
    if (m3_enable())
        ui_toast("M3 cores enabled: the next test starts them", C_GREEN, 1100);
}

static unsigned cores_offer(unsigned mask, const char *test)
{
    unsigned a55 = (mask & 0x0fU) ? (mask & 0x0fU) : 0x01U;
    if (!(mask & 0xf0U) || m3_state > 0)
        return mask;
    offer_pick = -1;
    offer_test = test;
    list_screen("Use the M3 cores?", build_offer, act_offer);
    if (E.home || E.tab_req >= 0 || offer_pick == 2 || offer_pick < 0)
        return 0;
    if (offer_pick == 1) {
        audit_line("M3 cores: the user picked the A55 cores only");
        return a55;
    }
    return m3_enable() ? mask : a55;
}

static void load_stop(void)
{
    clk_restore(1);                             /* the M3 domain while its cores still run */
    unsigned left = sb_smp_stop(1500);
    clk_restore(0);
    if (left)
        audit_line("a worker core did not stop within 1.5 s (reboot before starting an OS)");
}

/* This core's share of the work: about ms of kernel batches. */
static void load_slice(unsigned ms)
{
    if (core_res[0])
        return;
    struct sb_wslot *s = sb_smp_own_slot(0);
#ifdef SB_HOST
    err0 += kern_batch(&K, 0, &it0);    /* the preview's clock is fake: one batch per slice */
    sb_udelay((uint64_t)ms * 1000U);
#else
    uint64_t t = sb_ms();
    do
        err0 += kern_batch(&K, 0, &it0);
    while (sb_ms() - t < ms);
#endif
    s->iters = it0;
    s->errors = err0;
    s->state = SB_WK_RUNNING;
}

static uint64_t core_iters(unsigned w)
{
    return w ? sb_smp_slot(w)->iters : it0;
}

static uint64_t core_errors(unsigned w)
{
    return w ? sb_smp_slot(w)->errors : err0;
}

static void load_rates(void)
{
    rate_total = 0;
    for (unsigned w = 0; w < SB_NCORES; ++w) {
        uint64_t i = core_iters(w);
        rate[w] = i - last_it[w];
        last_it[w] = i;
        if (!core_res[w])
            rate_total += rate[w];
    }
}

static unsigned load_faults(void)
{
    unsigned n = 0;
    for (unsigned w = 1; w < SB_NCORES; ++w)
        n += !core_res[w] && sb_smp_slot(w)->state == SB_WK_FAULT;
    return n;
}

static uint64_t load_errors(void)
{
    uint64_t e = 0;
    for (unsigned w = 0; w < SB_NCORES; ++w)
        if (!core_res[w])
            e += core_errors(w);
    return e;
}

static void kv_cores(void)
{
    struct ub k, v;
    kv_hdr("CORES (BATCHES PER SECOND, ERRORS)");
    for (unsigned w = 0; w < SB_NCORES; ++w) {
        ub_init(&k);
        ub_s(&k, core_label(w));
        ub_init(&v);
        uint32_t c = C_L1;
        if (core_res[w] == 1) {
            ub_s(&v, "not used");
            c = C_L3;
        } else if (core_res[w]) {
            ub_s(&v, psci_name(core_res[w]));
            c = C_RED;
        } else {
            const struct sb_wslot *s = sb_smp_slot(w);
            if (w && s->state == SB_WK_FAULT) {
                ub_s(&v, "FAULT esr ");
                ub_hex(&v, s->esr, 8);
                ub_s(&v, " elr ");
                ub_hex(&v, s->elr, 9);
                c = C_RED;
            } else {
                ub_dec(&v, rate[w]);
                ub_s(&v, "/s  ");
                ub_dec(&v, core_iters(w));
                ub_s(&v, " total  ");
                uint64_t e = core_errors(w);
                ub_dec(&v, e);
                ub_s(&v, e == 1 ? " error" : " errors");
                c = e ? C_RED : C_GREEN;
                if (w && s->state == SB_WK_DONE) {
                    ub_s(&v, "  (stopped)");
                    c = C_L2;
                }
            }
        }
        kv_addc(k.b, v.b, c);
    }
}

/* ---- temperatures ------------------------------------------------------------------------------- */

static struct sb_temps T;
static struct sb_therm TH[8];
static int th_n = -2;
static const char *th_why;

static void temps_sample(int adc)
{
    sb_temps_read(&T);
    if (adc || th_n == -2)
        th_n = sb_therms_read(TH, 8, &th_why);
}

static int bat_c10(void)
{
    for (int i = 0; i < th_n; ++i)
        if (TH[i].ok && TH[i].ch == 4)
            return TH[i].c10;
    return -1000;
}

/* One TMU probe: the median of 3 samples, its code, and how many samples were rejected so far. */
static void kv_probe(const char *k, int t, int code, unsigned slot)
{
    uint32_t reads = 0, rej = 0;
    int bad = -1;
    struct ub u;
    sb_tmu_stats(slot, &reads, &rej, &bad);
    ub_init(&u);
    if (t <= -100)
        ub_s(&u, "no valid sample");
    else {
        ub_sdec(&u, t);
        ub_s(&u, " " U_DEG "C");
    }
    ub_s(&u, "  code ");
    ub_dec(&u, (uint64_t)code);
    if (rej) {
        ub_s(&u, "  rejected ");
        ub_dec(&u, rej);
        ub_s(&u, " (code ");
        ub_sdec(&u, bad);
        ub_c(&u, ')');
    }
    kv_addc(k, u.b, t <= -100 ? C_ORANGE : t >= 85 ? C_RED : t >= 65 ? C_ORANGE : rej ? C_YELLOW : C_L1);
}

static void kv_temps(void)
{
    struct ub k;
    kv_hdr("TMU_TOP (CPU CLUSTERS)");
    if (!T.top)
        kv_addc("State", "off (S-Boot did not enable it)", C_L3);
    for (unsigned i = 0; T.top && i < 8; ++i) {
        ub_init(&k);
        ub_s(&k, "Probe ");
        ub_dec(&k, i);
        ub_s(&k, i == 5 ? " (A55)" : (i >= 1 && i <= 4) || i == 6 ? " (M3)" : " (unassigned)");
        kv_probe(k.b, T.top_all[i], T.code_top[i], i);
    }
    if (T.top)
        kv_addc("Hardware trip", (T.trip_armed & 1) ? "armed (CONTROL bit 12)" : "NOT armed", (T.trip_armed & 1) ? C_L1 : C_ORANGE);
    if (T.trip_c > -100)
        kv_deg("Trip threshold (S-Boot, decoded)", T.trip_c);
    kv_hdr("TMU_SUB (GPU, ISP)");
    if (!T.sub)
        kv_addc("State", "off (S-Boot did not enable it)", C_L3);
    for (unsigned i = 0; T.sub && i < 3; ++i) {
        ub_init(&k);
        ub_s(&k, "Probe ");
        ub_dec(&k, i);
        ub_s(&k, i == 1 ? " (GPU)" : i == 2 ? " (ISP)" : " (unassigned)");
        kv_probe(k.b, T.sub_all[i], T.code_sub[i], 8U + i);
    }
    if (T.sub)
        kv_addc("Hardware trip", (T.trip_armed & 2) ? "armed (CONTROL bit 12)" : "NOT armed", (T.trip_armed & 2) ? C_L1 : C_ORANGE);
    if (T.trip_sub_c > -100)
        kv_deg("Trip threshold (decoded)", T.trip_sub_c);
    if (T.flags & 1U)
        kv_note("Some probes have no eFuse trim: values are approximate.");
    kv_note("Each probe is the median of 3 samples; code 0, all ones or a temperature outside -30..150 C is "
            "rejected and counted here (the v2.0 glitch: every probe reading 0 now and then).");
    kv_hdr("THERMISTORS (CMGP ADC)");
    if (th_n < 0)
        kv_addc("ADC", th_why ? th_why : "not readable", C_ORANGE);
    for (int i = 0; i < th_n; ++i) {
        const struct sb_adc_stat *st = sb_adc_stats(TH[i].ch);
        struct ub n;
        ub_init(&n);
        if (!TH[i].ok) {
            ub_s(&n, "no valid sample: ");
            ub_s(&n, TH[i].why ? TH[i].why : "?");
            if (TH[i].bad >= 0) {
                ub_s(&n, " (raw ");
                ub_dec(&n, (uint64_t)TH[i].bad);
                ub_c(&n, ')');
            }
            kv_addc(TH[i].name, n.b, C_ORANGE);
            continue;
        }
        ub_s(&n, "raw ");
        ub_dec(&n, (uint64_t)TH[i].raw);
        if (TH[i].clamped)
            ub_s(&n, " (outside the table)");
        if (st && st->rejected) {
            ub_s(&n, "  rejected ");
            ub_dec(&n, st->rejected);
            ub_s(&n, " of ");
            ub_dec(&n, st->conversions);
            ub_s(&n, " (raw ");
            ub_sdec(&n, st->last_bad);
            ub_c(&n, ')');
        }
        kv_deg10(TH[i].name, TH[i].c10, n.b);
    }
    unsigned flag_only = 0, timeouts = 0;
    for (unsigned c = 0; c < 12; ++c) {
        const struct sb_adc_stat *st = sb_adc_stats(c);
        flag_only += st->flag_only;
        timeouts += st->timeouts;
    }
    if (th_n > 0) {
        struct ub n;
        ub_init(&n);
        ub_s(&n, flag_only ? "INT_ST did not come " : "INT_ST, then DAT (as the kernel)");
        if (flag_only) {
            ub_dec(&n, flag_only);
            ub_s(&n, " times: DAT bit 31 used");
        }
        if (timeouts) {
            ub_s(&n, ", ");
            ub_dec(&n, timeouts);
            ub_s(&n, " timeouts");
        }
        kv_addc("ADC completion", n.b, flag_only || timeouts ? C_ORANGE : C_L1);
    }
    uint16_t v;
    if (sb_fg_word_checked(0x08, &v, 0, 0)) {
        int t10 = (int)(int16_t)v * 10 / 256;
        kv_deg10("Fuel gauge temp register", t10, "(the last value S-Boot wrote)");
    }
    if (sb_fg_stat.reads) {
        struct ub n;
        ub_init(&n);
        ub_dec(&n, sb_fg_stat.reads);
        ub_s(&n, " reads, ");
        ub_dec(&n, sb_fg_stat.retries);
        ub_s(&n, " retried, ");
        ub_dec(&n, sb_fg_stat.failed);
        ub_s(&n, " failed, ");
        ub_dec(&n, sb_fg_stat.rejected);
        ub_s(&n, " implausible");
        kv_addc("Fuel gauge I2C", n.b, sb_fg_stat.failed || sb_fg_stat.rejected ? C_ORANGE : C_L1);
    }
}

/* ---- clocks: throttling ------------------------------------------------------------------------ */

static uint64_t a55_mhz, m3_mhz;
static int m3_off;

static void clocks_sample(void)
{
    a55_mhz = sb_a55_hz() / 1000000U;
    m3_mhz = sb_m3_hz(&m3_off) / 1000000U;
}

/* ---- CPU stress ---------------------------------------------------------------------------------- */

static uint64_t cs_t0, cs_dur;
static int cs_stop;                         /* 0 running, 1 user, 2 temperature, 3 time, 4 fault */
static int cs_peak;
static int cs_done;
static char cs_detail[96];

static void fill_cpu_live(void)
{
    struct ub u;
    uint64_t s = (sb_ms() - cs_t0) / 1000U;
    ub_init(&u);
    fmt_mmss(&u, s);
    if (cs_dur) {
        ub_s(&u, " / ");
        fmt_mmss(&u, cs_dur);
    }
    uint64_t e = load_errors();
    kv_bigc(e ? "ERRORS FOUND" : "running, no errors", u.b, e ? C_RED : C_GREEN);
    ub_init(&u);
    ub_dec(&u, rate_total);
    ub_s(&u, " batches/s on ");
    unsigned n = 0;
    for (unsigned w = 0; w < SB_NCORES; ++w)
        n += !core_res[w];
    ub_dec(&u, n);
    ub_s(&u, " cores");
    kv_add("Work", u.b);
    ub_init(&u);
    ub_dec(&u, a55_mhz);
    ub_s(&u, " MHz A55, ");
    if (m3_off)
        ub_s(&u, "M3 off");
    else {
        ub_dec(&u, m3_mhz);
        ub_s(&u, " MHz M3");
    }
    kv_add("Clocks", u.b);
    kv_deg("Hottest sensor", T.hottest);
    ub_init(&u);
    limit_text(&u);
    kv_addc("Limit", u.b, opt.limit == LIMIT_NONE ? C_ORANGE : C_L1);
    kv_chart(CH_TEMP, 460);
    kv_chart(CH_RATE, 460);
    kv_cores();
    kv_note("Hold Power to stop. Every batch's result is compared with the reference computed at the start.");
}

static void fill_cpu_result(void)
{
    struct ub u;
    uint64_t e = load_errors();
    unsigned f = load_faults();
    kv_bigc(cs_stop == 2 ? "stopped at the temperature limit" : cs_stop == 1 ? "stopped by hand" :
            cs_stop == 4 ? "a core faulted" : "finished",
            e || f ? "FAIL" : "PASS", e || f ? C_RED : C_GREEN);
    kv_add("Result", cs_detail);
    ub_init(&u);
    ub_sdec(&u, cs_peak);
    ub_s(&u, " " U_DEG "C");
    kv_add("Peak temperature", u.b);
    kv_chart(CH_TEMP, 460);
    kv_chart(CH_RATE, 460);
    kv_cores();
}

static void cpu_run(void)
{
    unsigned mask = core_mask[opt.cores];
    if (!guard_ok("CPU stress"))
        return;
    mask = cores_offer(mask, "CPU stress");
    if (!mask)
        return;
    cs_dur = dur_s[opt.dur];
    ch_reset(CH_TEMP, "HOTTEST SENSOR (" U_DEG "C)", C_ORANGE, cs_dur ? (unsigned)cs_dur : 3600);
    ch_reset(CH_RATE, "WORK (BATCHES PER SECOND, ALL CORES)", C_GREEN, cs_dur ? (unsigned)cs_dur : 3600);
    (void)load_start(mask, kern_mask[opt.kern]);
    live_begin("CPU stress");
    cs_t0 = sb_ms();
    cs_stop = 0;
    cs_peak = -100;
    uint64_t next_s = sb_ms() + 1000;
    temps_sample(0);
    clocks_sample();
    while (!cs_stop) {
        load_slice(20);
        if (live_poll(fill_cpu_live, 250))
            cs_stop = 1;
        if (sb_ms() >= next_s) {
            next_s += 1000;
            temps_sample(0);
            clocks_sample();
            load_rates();
            ch_push(CH_TEMP, T.hottest);
            ch_push(CH_RATE, (int32_t)rate_total);
            if (T.hottest > cs_peak)
                cs_peak = T.hottest;
            if (over_limit(T.hottest))
                cs_stop = 2;
            if (load_faults())
                cs_stop = 4;
        }
        if (cs_dur && sb_ms() - cs_t0 >= cs_dur * 1000U)
            cs_stop = 3;
    }
    load_stop();
    live_end();
    uint64_t e = load_errors();
    unsigned f = load_faults(), n = 0;
    for (unsigned w = 0; w < SB_NCORES; ++w)
        n += !core_res[w];
    struct ub u;
    ub_init(&u);
    ub_dec(&u, n);
    ub_s(&u, " cores, ");
    fmt_mmss(&u, (sb_ms() - cs_t0) / 1000U);
    ub_s(&u, ", ");
    ub_dec(&u, e);
    ub_s(&u, " errors, ");
    ub_dec(&u, f);
    ub_s(&u, " faults, peak ");
    ub_sdec(&u, cs_peak);
    ub_s(&u, " C");
    if (cs_stop == 2)
        ub_s(&u, " (temperature limit)");
    sb_strcpy(cs_detail, sizeof(cs_detail), u.b);
    result("CPU stress", !e && !f && n > 0, cs_detail);
    cs_done = 1;
    info_screen("CPU stress result", fill_cpu_result, 0);
}

static void build_cpu(void)
{
    kv_hdr("OPTIONS (POWER CHANGES)");
    row_value("Cores", core_name[opt.cores], 0);
    row_value("Kernels", kern_name[opt.kern], 0);
    row_value("Clocks", clk_name[opt.clk], opt.clk ? R_WARN : 0);
    row_value("Duration", dur_name[opt.dur], 0);
    row_value("Stop at", lab_limit_label(opt.limit), opt.limit == LIMIT_NONE ? R_RED : 0);
    kv_hdr("RUN");
    row_add("Start", m3_state > 0 || !(core_mask[opt.cores] & 0xf0U) ? "hold Power to stop"
                                                                      : "offers to enable the M3 cores first",
            R_WARN);
    row_add("Last result", cs_done ? cs_detail : "not run", cs_done ? 0 : R_DIS);
    row_add("M3 cores (cpu4-7)", m3_state > 0 ? "enabled for this session" : "off: enable them now (tier 3, PMU)",
            m3_state > 0 ? R_DIS : R_RED);
    kv_note("Each core runs the chosen kernels and compares every result with the reference. The heat kernel "
            "keeps every FP pipe busy (the most power); Clocks full asks ACPM for 1794 MHz (tier 3) and restores "
            "1053 / 1066 MHz afterwards. The M3 cores need 6 PMU registers set first (the vendor sequence Linux and iOS use): "
            "a test that uses them offers that step when it starts, or enable them here.");
}

static int act_cpu(int i)
{
    switch (i) {
    case 0: opt.cores = nxt(opt.cores, 4); break;
    case 1: opt.kern = nxt(opt.kern, KERN_N); break;
    case 2: opt.clk = !opt.clk; break;
    case 3: opt.dur = nxt(opt.dur, 7); break;
    case 4: opt.limit = lab_limit_next(opt.limit); break;
    case 5: cpu_run(); break;
    case 6: info_screen("CPU stress result", fill_cpu_result, 0); break;
    case 7: m3_enable_now(); break;
    default: break;
    }
    return 0;
}

void lab_cpu_stress(void)
{
    list_screen("CPU stress", build_cpu, act_cpu);
}

/* ---- thermal test -------------------------------------------------------------------------------- */

#define TLOG_N 4000
static struct tlog {
    uint16_t t;
    int8_t hot, big, little, gpu, bat;
    uint8_t phase;                          /* 0 baseline, 1 load, 2 cool-down */
    uint32_t rate;
    uint16_t a55, m3;
} tlog[TLOG_N];
static unsigned tlog_n;
static int th_phase, th_stop, th_done, th_cool = 1;
static uint64_t th_t0, th_load_t0, th_dur;
static int th_start_c, th_peak_c, th_peak_t, th_throttle_t = -1, th_throttle_c, th_end_load_c, th_cool_c;
static uint64_t th_base_rate, th_base_a55, th_base_m3;
static char th_throttle_why[48], th_detail[96];
static unsigned th_low_streak;

static const char *phase_name(int p)
{
    return p == 0 ? "baseline (idle)" : p == 1 ? "load (all selected cores)" : "cool-down (idle)";
}

static void fill_th_live(void)
{
    struct ub u;
    uint64_t s = (sb_ms() - th_t0) / 1000U;
    ub_init(&u);
    ub_sdec(&u, T.hottest);
    ub_s(&u, " " U_DEG "C");
    kv_bigc(phase_name(th_phase), u.b, opt.limit != LIMIT_NONE && T.hottest >= limit_c[opt.limit] - 5 ? C_RED : C_L1);
    ub_init(&u);
    fmt_mmss(&u, s);
    ub_s(&u, "  " U_DOT "  limit ");
    if (opt.limit == LIMIT_NONE)
        ub_s(&u, "none");
    else {
        ub_dec(&u, (uint64_t)limit_c[opt.limit]);
        ub_s(&u, " " U_DEG "C");
    }
    kv_add("Time", u.b);
    ub_init(&u);
    ub_dec(&u, a55_mhz);
    ub_s(&u, " / ");
    if (m3_off)
        ub_s(&u, "off");
    else
        ub_dec(&u, m3_mhz);
    ub_s(&u, " MHz  " U_DOT "  ");
    ub_dec(&u, rate_total);
    ub_s(&u, " batches/s");
    kv_add("A55 / M3, work", u.b);
    kv_addc("Throttling", th_throttle_t >= 0 ? th_throttle_why : "not seen", th_throttle_t >= 0 ? C_ORANGE : C_GREEN);
    kv_chart(CH_TEMP, 460);
    kv_chart(CH_RATE, 400);
    kv_temps();
    kv_note("Hold Power to stop. Throttling: the clock or the work rate drops 15 % below the first 10 s of load.");
}

static void fill_th_result(void)
{
    struct ub u;
    kv_bigc(th_stop == 2 ? "stopped at the limit" : th_stop == 1 ? "stopped by hand" : "finished",
            th_stop == 2 ? "LIMIT" : th_stop == 4 ? "FAIL" : "DONE", th_stop == 2 || th_stop == 4 ? C_ORANGE : C_GREEN);
    kv_add("Summary", th_detail);
    kv_hdr("TEMPERATURES");
    kv_deg("At the start", th_start_c);
    kv_deg("Peak", th_peak_c);
    ub_init(&u);
    ub_dec(&u, (uint64_t)(th_peak_t > 0 ? th_peak_t : 0));
    ub_s(&u, " s into the load");
    kv_add("Peak reached", u.b);
    uint64_t load_s = 0;
    for (unsigned i = 0; i < tlog_n; ++i)
        load_s += tlog[i].phase == 1;
    ub_init(&u);
    if (load_s > 10) {
        int rise = th_end_load_c - th_start_c;
        ub_sdec(&u, rise * 60 / (int)load_s);
        ub_s(&u, " " U_DEG "C per minute of load");
    } else {
        ub_s(&u, "-");
    }
    kv_add("Rise", u.b);
    if (th_cool && th_cool_c > -100) {
        ub_init(&u);
        ub_sdec(&u, th_end_load_c - th_cool_c);
        ub_s(&u, " " U_DEG "C in the cool-down");
        kv_add("Cool-down", u.b);
    }
    kv_addc("Throttling", th_throttle_t >= 0 ? th_throttle_why : "not seen", th_throttle_t >= 0 ? C_ORANGE : C_GREEN);
    if (th_throttle_t >= 0) {
        ub_init(&u);
        ub_dec(&u, (uint64_t)th_throttle_t);
        ub_s(&u, " s into the load at ");
        ub_sdec(&u, th_throttle_c);
        ub_s(&u, " " U_DEG "C");
        kv_add("First seen", u.b);
    }
    kv_chart(CH_TEMP, 460);
    kv_chart(CH_RATE, 400);
    ub_init(&u);
    ub_dec(&u, tlog_n);
    ub_s(&u, " samples (1 per second): Tools > Save report writes them to the OS store");
    kv_note(u.b);
}

static void th_log(void)
{
    if (tlog_n >= TLOG_N)
        return;
    struct tlog *l = &tlog[tlog_n++];
    int b = bat_c10();
    l->t = (uint16_t)((sb_ms() - th_t0) / 1000U);
    l->hot = (int8_t)(T.hottest < -100 ? -100 : T.hottest > 127 ? 127 : T.hottest);
    l->big = (int8_t)(T.big_max < -100 ? -100 : T.big_max > 127 ? 127 : T.big_max);
    l->little = (int8_t)(T.little < -100 ? -100 : T.little > 127 ? 127 : T.little);
    l->gpu = (int8_t)(T.gpu < -100 ? -100 : T.gpu > 127 ? 127 : T.gpu);
    l->bat = (int8_t)(b <= -1000 ? -100 : b / 10);
    l->phase = (uint8_t)th_phase;
    l->rate = (uint32_t)rate_total;
    l->a55 = (uint16_t)a55_mhz;
    l->m3 = (uint16_t)(m3_off ? 0 : m3_mhz);
}

static void th_run(void)
{
    th_dur = dur_s[opt.dur] ? dur_s[opt.dur] : 3600;
    ch_reset(CH_TEMP, "HOTTEST SENSOR (" U_DEG "C)", C_ORANGE, (unsigned)th_dur + 70);
    ch_reset(CH_RATE, "WORK (BATCHES PER SECOND)", C_GREEN, (unsigned)th_dur + 70);
    tlog_n = 0;
    th_stop = 0;
    th_throttle_t = -1;
    th_low_streak = 0;
    th_cool_c = -100;
    th_base_rate = th_base_a55 = th_base_m3 = 0;
    if (!guard_ok("Thermal test"))
        return;
    run_mask = cores_offer(core_mask[opt.cores], "Thermal test");
    if (!run_mask)
        return;
    load_alloc(kern_mask[opt.kern]);
    for (unsigned w = 0; w < SB_NCORES; ++w)
        core_res[w] = 1;
    temps_sample(1);
    clocks_sample();
    th_start_c = th_peak_c = T.hottest;
    th_peak_t = 0;
    live_begin("Thermal test");
    th_t0 = sb_ms();
    th_phase = 0;
    uint64_t next_s = sb_ms() + 1000, phase_t0 = sb_ms();
    unsigned sec = 0;
    while (!th_stop) {
        if (th_phase == 1)
            load_slice(20);
        else
            sb_mdelay(5);
        if (live_poll(fill_th_live, 250))
            th_stop = 1;
        if (sb_ms() < next_s)
            continue;
        next_s += 1000;
        ++sec;
        temps_sample(sec % 2 == 0);
        clocks_sample();
        if (th_phase == 1)
            load_rates();
        else
            rate_total = 0;
        ch_push(CH_TEMP, T.hottest);
        ch_push(CH_RATE, (int32_t)rate_total);
        th_log();
        uint64_t in_phase = (sb_ms() - phase_t0) / 1000U;
        if (over_limit(T.hottest) && th_phase != 2) {
            th_stop = 2;
            break;
        }
        if (th_phase == 1) {
            if (T.hottest > th_peak_c) {
                th_peak_c = T.hottest;
                th_peak_t = (int)in_phase;
            }
            if (in_phase >= 3 && in_phase < 11) {   /* the baseline of the load: seconds 3..10 */
                th_base_rate += rate_total;
                if (a55_mhz > th_base_a55)
                    th_base_a55 = a55_mhz;
                if (!m3_off && m3_mhz > th_base_m3)
                    th_base_m3 = m3_mhz;
            } else if (in_phase >= 11 && th_throttle_t < 0) {
                uint64_t base = th_base_rate / 8U;
                int clk = (th_base_a55 && a55_mhz * 100U < th_base_a55 * 85U) ||
                          (th_base_m3 && !m3_off && m3_mhz * 100U < th_base_m3 * 85U);
                int work = base && rate_total * 100U < base * 85U;
                th_low_streak = clk || work ? th_low_streak + 1 : 0;
                if (th_low_streak >= 3) {
                    th_throttle_t = (int)in_phase;
                    th_throttle_c = T.hottest;
                    sb_strcpy(th_throttle_why, sizeof(th_throttle_why),
                              clk ? "the CPU clock dropped (DVFS)" : "the work rate dropped (clock or bus)");
                }
            }
            if (load_faults()) {
                th_stop = 4;
                break;
            }
        }
        if (th_phase == 0 && in_phase >= 10) {
            th_start_c = T.hottest;
            (void)load_start(run_mask, kern_mask[opt.kern]);
            th_load_t0 = sb_ms();
            th_phase = 1;
            phase_t0 = sb_ms();
        } else if (th_phase == 1 && in_phase >= th_dur) {
            load_stop();
            th_end_load_c = T.hottest;
            th_phase = 2;
            phase_t0 = sb_ms();
            if (!th_cool)
                th_stop = 3;
        } else if (th_phase == 2 && in_phase >= 60) {
            th_cool_c = T.hottest;
            th_stop = 3;
        }
    }
    if (th_phase == 1) {
        load_stop();
        th_end_load_c = T.hottest;
    }
    if (th_phase == 2 && th_cool_c <= -100)
        th_cool_c = T.hottest;
    live_end();
    (void)th_load_t0;
    struct ub u;
    ub_init(&u);
    ub_s(&u, "start ");
    ub_sdec(&u, th_start_c);
    ub_s(&u, " C, peak ");
    ub_sdec(&u, th_peak_c);
    ub_s(&u, " C, ");
    ub_s(&u, th_throttle_t >= 0 ? "throttling seen" : "no throttling");
    if (th_stop == 2)
        ub_s(&u, ", stopped at the limit");
    if (th_stop == 4)
        ub_s(&u, ", a core faulted");
    sb_strcpy(th_detail, sizeof(th_detail), u.b);
    result("Thermal test", th_stop != 2 && th_stop != 4 && load_errors() == 0, th_detail);
    th_done = 1;
    info_screen("Thermal test result", fill_th_result, 0);
}

static void build_th(void)
{
    kv_hdr("OPTIONS (POWER CHANGES)");
    row_value("Load on", core_name[opt.cores], 0);
    row_value("Load", kern_name[opt.kern], 0);
    row_value("Clocks", clk_name[opt.clk], opt.clk ? R_WARN : 0);
    row_value("Load for", dur_name[opt.dur], 0);
    row_value("Stop at", lab_limit_label(opt.limit), opt.limit == LIMIT_NONE ? R_RED : 0);
    row_value("Cool-down", th_cool ? "60 s, recorded" : "no", 0);
    kv_hdr("RUN");
    row_add("Start", "10 s idle baseline, the load, the cool-down", R_WARN);
    row_add("Last result", th_done ? th_detail : "not run", th_done ? 0 : R_DIS);
    row_add("Every sensor, live", "TMU probes, thermistors, fuel gauge", 0);
    row_add("M3 cores (cpu4-7)", m3_state > 0 ? "enabled for this session" : "off: enable them now (tier 3, PMU)",
            m3_state > 0 ? R_DIS : R_RED);
    kv_note("Logs every sensor once a second, detects throttling (clock or work rate down 15 %), and stops "
            "the load at the limit. The hardware trip S-Boot arms (decoded: 115 " U_DEG "C, see Every thermal "
            "sensor) is far above it.");
}

static int act_th(int i)
{
    switch (i) {
    case 0: opt.cores = nxt(opt.cores, 4); break;
    case 1: opt.kern = nxt(opt.kern, KERN_N); break;
    case 2: opt.clk = !opt.clk; break;
    case 3: opt.dur = nxt(opt.dur, 7); break;
    case 4: opt.limit = lab_limit_next(opt.limit); break;
    case 5: th_cool = !th_cool; break;
    case 6: th_run(); break;
    case 7: info_screen("Thermal test result", fill_th_result, 0); break;
    case 8: lab_sensors(); break;
    case 9: m3_enable_now(); break;
    default: break;
    }
    return 0;
}

void lab_thermal(void)
{
    list_screen("Thermal test", build_th, act_th);
}

static void fill_sensors(void)
{
    temps_sample(1);
    clocks_sample();
    struct ub u;
    ub_init(&u);
    ub_sdec(&u, T.hottest);
    ub_s(&u, " " U_DEG "C");
    kv_big("hottest TMU probe", u.b);
    ub_init(&u);
    ub_dec(&u, a55_mhz);
    ub_s(&u, " MHz A55, ");
    if (m3_off)
        ub_s(&u, "M3 off");
    else {
        ub_dec(&u, m3_mhz);
        ub_s(&u, " MHz M3");
    }
    kv_add("Clocks", u.b);
    kv_temps();
}

void lab_sensors(void)
{
    info_screen("Every thermal sensor", fill_sensors, 1000);
}

/* ---- memory stress -------------------------------------------------------------------------------- */

static const char *const pat_name[] = { "zeros", "ones", "checkerboard 55/AA", "checkerboard AA/55", "address",
                                        "inverse address", "walking ones", "walking zeros", "random" };
#define NPAT 9
static const uint64_t ms_size[] = { 16UL << 20, 64UL << 20, SB_TEST_SIZE };
static const char *const ms_size_name[] = { "16 MiB", "64 MiB", "126 MiB (the whole window)" };
static const unsigned ms_passes[] = { 1, 3, 10, 0 };
static const char *const ms_pass_name[] = { "1", "3", "10", "until stopped" };
static int ms_sz = 0, ms_np = 0, ms_done, ms_stop;
static unsigned ms_pass, ms_pat;
static uint64_t ms_err, ms_first_bad, ms_bad_val, ms_bad_want, ms_pos, ms_t0, ms_bw_w, ms_bw_r, ms_bw_c;
static char ms_detail[96];

static uint64_t pat_val(unsigned p, uint64_t i, uint64_t seed)
{
    switch (p) {
    case 0: return 0;
    case 1: return ~0ULL;
    case 2: return (i & 1U) ? 0xaaaaaaaaaaaaaaaaULL : 0x5555555555555555ULL;
    case 3: return (i & 1U) ? 0x5555555555555555ULL : 0xaaaaaaaaaaaaaaaaULL;
    case 4: return SB_TEST_BASE + i * 8U;
    case 5: return ~(SB_TEST_BASE + i * 8U);
    case 6: return 1ULL << ((i + seed) & 63U);
    case 7: return ~(1ULL << ((i + seed) & 63U));
    default: {
        uint64_t x = (i + 1U) * 0x9e3779b97f4a7c15ULL ^ seed;
        x ^= x >> 31;
        x *= 0xbf58476d1ce4e5b9ULL;
        return x ^ (x >> 29);
    }
    }
}

static void fill_ms_live(void)
{
    struct ub u;
    uint64_t n = ms_size[ms_sz] / 8U;
    ub_init(&u);
    ub_dec(&u, n ? ms_pos * 100U / n : 0);
    ub_s(&u, " %");
    kv_bigc(pat_name[ms_pat], u.b, ms_err ? C_RED : C_L1);
    ub_init(&u);
    ub_dec(&u, ms_pass + 1U);
    if (ms_passes[ms_np]) {
        ub_s(&u, " of ");
        ub_dec(&u, ms_passes[ms_np]);
    }
    ub_s(&u, ", pattern ");
    ub_dec(&u, ms_pat + 1U);
    ub_s(&u, " of 9");
    kv_add("Pass", u.b);
    kv_addc("Errors", ms_err ? "FOUND" : "none", ms_err ? C_RED : C_GREEN);
    if (ms_err) {
        kv_dec("Bad words", ms_err, 0);
        kv_hex("First bad address", ms_first_bad, 9);
        kv_hex("Read", ms_bad_val, 16);
        kv_hex("Expected", ms_bad_want, 16);
    }
    ub_init(&u);
    ub_dec(&u, ms_bw_w);
    ub_s(&u, " write, ");
    ub_dec(&u, ms_bw_r);
    ub_s(&u, " read, ");
    ub_dec(&u, ms_bw_c);
    ub_s(&u, " copy (MiB/s)");
    kv_add("Bandwidth", u.b);
    kv_chart(CH_BW, 420);
    kv_add("Window", "0x0A0100000.., non-cacheable: every access reaches DRAM");
    kv_note("Hold Power to stop (after the current MiB).");
}

static void fill_ms_result(void)
{
    kv_bigc(ms_stop ? "stopped" : "finished", ms_err ? "FAIL" : "PASS", ms_err ? C_RED : C_GREEN);
    kv_add("Result", ms_detail);
    if (ms_err) {
        kv_hex("First bad address", ms_first_bad, 9);
        kv_hex("Read", ms_bad_val, 16);
        kv_hex("Expected", ms_bad_want, 16);
    }
    kv_chart(CH_BW, 420);
}

/* A pattern over the window, 1 MiB at a time: write all, then verify all. 1 = stop requested. */
static int ms_pattern(unsigned p, uint64_t seed)
{
    volatile uint64_t *m = (volatile uint64_t *)SB_TEST_BASE;
    uint64_t n = ms_size[ms_sz] / 8U, chunk = (1U << 20) / 8U;
    ms_pat = p;
    for (int verify = 0; verify < 2; ++verify)
        for (uint64_t i0 = 0; i0 < n; i0 += chunk) {
            uint64_t e = i0 + chunk < n ? i0 + chunk : n;
            if (!verify) {
                for (uint64_t i = i0; i < e; ++i)
                    m[i] = pat_val(p, i, seed);
            } else {
                for (uint64_t i = i0; i < e; ++i) {
                    uint64_t want = pat_val(p, i, seed), got = m[i];
                    if (got != want) {
                        if (!ms_err) {
                            ms_first_bad = SB_TEST_BASE + i * 8U;
                            ms_bad_val = got;
                            ms_bad_want = want;
                        }
                        ++ms_err;
                    }
                }
            }
            ms_pos = verify ? e : e / 2;
            if (live_poll(fill_ms_live, 250))
                return 1;
        }
    return 0;
}

static void ms_bandwidth(void)
{
    volatile uint64_t *m = (volatile uint64_t *)SB_TEST_BASE;
    uint64_t n = (16U << 20) / 8U, sum = 0, t = sb_ticks();
    for (uint64_t i = 0; i < n; ++i)
        m[i] = i;
    uint64_t d = sb_ticks() - t;
    ms_bw_w = d ? n * 8U * sb_freq() / d >> 20 : 0;
    t = sb_ticks();
    for (uint64_t i = 0; i < n; ++i)
        sum += m[i];
    d = sb_ticks() - t;
    ms_bw_r = d ? n * 8U * sb_freq() / d >> 20 : 0;
    t = sb_ticks();
    for (uint64_t i = 0; i < n / 2; ++i)
        m[n / 2 + i] = m[i];
    d = sb_ticks() - t;
    ms_bw_c = d ? n * 4U * sb_freq() / d >> 20 : 0;
    __asm__ volatile("" : : "r"(sum));
    ch_push(CH_BW, (int32_t)ms_bw_r);
}

static void ms_run(void)
{
    ms_err = ms_first_bad = 0;
    ms_stop = 0;
    ms_pos = 0;
    ch_reset(CH_BW, "DRAM READ BANDWIDTH PER PASS (MiB/s)", C_STAR, 240);
    live_begin("Memory stress");
    ms_t0 = sb_ms();
    for (ms_pass = 0; !ms_stop && (!ms_passes[ms_np] || ms_pass < ms_passes[ms_np]); ++ms_pass) {
        ms_bandwidth();
        for (unsigned p = 0; p < NPAT && !ms_stop; ++p)
            if (ms_pattern(p, ms_pass * 0x2545f4914f6cdd1dULL + p))
                ms_stop = 1;
    }
    live_end();
    struct ub u;
    ub_init(&u);
    ub_s(&u, ms_size_name[ms_sz]);
    ub_s(&u, ", ");
    ub_dec(&u, ms_pass);
    ub_s(&u, ms_pass == 1 ? " pass, " : " passes, ");
    ub_dec(&u, ms_err);
    ub_s(&u, " bad words, read ");
    ub_dec(&u, ms_bw_r);
    ub_s(&u, " MiB/s");
    sb_strcpy(ms_detail, sizeof(ms_detail), u.b);
    result("Memory stress", !ms_err, ms_detail);
    ms_done = 1;
    info_screen("Memory stress result", fill_ms_result, 0);
}

static void build_ms(void)
{
    kv_hdr("OPTIONS (POWER CHANGES)");
    row_value("Size", ms_size_name[ms_sz], 0);
    row_value("Passes", ms_pass_name[ms_np], 0);
    kv_hdr("RUN");
    row_add("Start", "9 patterns per pass + bandwidth; hold Power to stop", 0);
    row_add("Last result", ms_done ? ms_detail : "not run", ms_done ? 0 : R_DIS);
    kv_note("Patterns: zeros, ones, both checkerboards, address and its inverse, walking ones and zeros, "
            "random. Only the free window at 0xA0100000 (non-cacheable) is written: S-Boot's ECT below it and "
            "Engineer OS itself are never touched.");
}

static int act_ms(int i)
{
    switch (i) {
    case 0: ms_sz = nxt(ms_sz, 3); break;
    case 1: ms_np = nxt(ms_np, 4); break;
    case 2: ms_run(); break;
    case 3: info_screen("Memory stress result", fill_ms_result, 0); break;
    default: break;
    }
    return 0;
}

void lab_mem_stress(void)
{
    list_screen("Memory stress", build_ms, act_ms);
}

/* ---- overclocking stability checks (oc.c) -------------------------------------------------------
 * The same kernels and patterns as the CPU and memory stress pages, run for a fixed time on a
 * clock that was just changed. 1 pass, 0 fail (errors, a fault), -2 inconclusive (the temperature
 * limit: 95 C for these runs unless the user set a higher one), -1 not
 * run (no cores, cancelled, no temperature guard declined). Nothing goes to the result pages. */

/* Overclocking runs: every TMU probe to the log once a second (the first 30 s, then every 10 s), so
 * a temperature stop can be told apart from a misreading probe (2026-11-08: M3 runs stopped "at
 * once", while A55 1950 MHz on four cores held 65 C). at = ms into the run; ~0 = the stop line. */
static void oc_temps_log(uint64_t at)
{
    struct ub u;
    uint64_t s = at / 1000U;
    if (at != ~0ULL && s > 30 && s % 10U)
        return;
    ub_init(&u);
    ub_s(&u, at == ~0ULL ? "oc temps STOP:" : "oc temps ");
    if (at != ~0ULL) {
        ub_dec(&u, s);
        ub_s(&u, "s:");
    }
    static const char *const nm[5] = { " big1 ", " big2 ", " big3 ", " big4 ", " big6 " };
    static const unsigned bp[5] = { 1, 2, 3, 4, 6 };
    for (unsigned i = 0; i < 5; ++i) {
        ub_s(&u, nm[i]);
        ub_sdec(&u, T.big[i]);
        ub_c(&u, '/');
        ub_dec(&u, (unsigned)T.code_top[bp[i]]);
    }
    ub_s(&u, " lit ");
    ub_sdec(&u, T.little);
    ub_s(&u, " gpu ");
    ub_sdec(&u, T.gpu);
    ub_s(&u, " isp ");
    ub_sdec(&u, T.isp);
    ub_s(&u, sb_m3_on() ? " M3 on " : " M3 off ");
    ub_dec(&u, sb_m3_hz(&(int){ 0 }) / 1000000U);
    sb_log(u.b);
}

int lab_oc_cpu(unsigned mask, unsigned secs, char *detail, unsigned cap)
{
    struct ub u;
    /* an overclocked core reaches Lab's default 85 C in seconds (A55 1950 MHz, 4 cores: 2 s on
     * 2026-11-08): 95 C for this run unless the user set a higher one; "no limit" is never used here */
    int lim = opt.limit;
    if (opt.limit == LIMIT_NONE || opt.limit < 5)
        opt.limit = 5;
    if (!guard_ok("Stability test (CPU)")) {
        opt.limit = lim;
        return -1;
    }
    unsigned want = mask;
    mask = cores_offer(mask, "Overclocking stability test");
    if (!mask || (!(want & 0x0fU) && !(mask & 0xf0U))) {
        opt.limit = lim;
        return -1;                          /* an M3 test without the M3 cores is not a test */
    }
    cs_dur = secs;
    ch_reset(CH_TEMP, "HOTTEST SENSOR (" U_DEG "C)", C_ORANGE, secs);
    ch_reset(CH_RATE, "WORK (BATCHES PER SECOND, ALL CORES)", C_GREEN, secs);
    unsigned started = load_start(mask, KERN_INT | KERN_FP | KERN_NEON | KERN_MEM), over = 0;
    temps_sample(0);
    oc_temps_log(0);                        /* right after the cores started: every probe */
    live_begin("Stability test (CPU)");
    cs_t0 = sb_ms();
    cs_stop = 0;
    cs_peak = -100;
    uint64_t next_s = sb_ms() + 1000;
    temps_sample(0);
    clocks_sample();
    while (!cs_stop) {
        load_slice(20);
        if (live_poll(fill_cpu_live, 250))
            cs_stop = 1;
        if (sb_ms() >= next_s) {
            next_s += 1000;
            temps_sample(0);
            clocks_sample();
            load_rates();
            ch_push(CH_TEMP, T.hottest);
            ch_push(CH_RATE, (int32_t)rate_total);
            if (T.hottest > cs_peak)
                cs_peak = T.hottest;
            oc_temps_log(sb_ms() - cs_t0);
            /* two samples in a row over the limit (one stuck probe right after the M3 cluster powers up
             * must not end the run; the TMU's own trip at 115 C stays armed) */
            over = over_limit(T.hottest) ? over + 1 : 0;
            if (over >= 2) {
                cs_stop = 2;
                oc_temps_log(~0ULL);            /* the stop: every probe, which one tripped */
            }
            if (load_faults() || load_errors())
                cs_stop = 4;
        }
        if (sb_ms() - cs_t0 >= (uint64_t)secs * 1000U)
            cs_stop = 3;
    }
    load_stop();
    live_end();
    uint64_t e = load_errors();
    unsigned f = load_faults(), n = 0;
    for (unsigned w = 0; w < SB_NCORES; ++w)
        n += !core_res[w];
    ub_init(&u);
    ub_dec(&u, n);
    ub_s(&u, " cores (");
    ub_dec(&u, started + ((mask & 1U) ? 1U : 0U));
    ub_s(&u, " asked), ");
    fmt_mmss(&u, (sb_ms() - cs_t0) / 1000U);
    ub_s(&u, ", ");
    ub_dec(&u, e);
    ub_s(&u, " errors, ");
    ub_dec(&u, f);
    ub_s(&u, " faults, peak ");
    ub_sdec(&u, cs_peak);
    ub_s(&u, " C");
    if (cs_stop == 1)
        ub_s(&u, " (stopped by hand)");
    if (cs_stop == 2) {
        ub_s(&u, " (temperature limit ");
        ub_dec(&u, (uint64_t)limit_c[opt.limit]);
        ub_s(&u, " C)");
    }
    sb_strcpy(detail, cap, u.b);
    opt.limit = lim;
    if (cs_stop == 1)
        return -1;
    if (cs_stop == 2 && !e && !f)
        return -2;                          /* inconclusive: the temperature limit, not an error */
    return !e && !f && n > 0 && cs_stop == 3;
}

int lab_oc_mem(unsigned size_idx, unsigned passes, char *detail, unsigned cap, unsigned *bw_read)
{
    int sz = ms_sz, np = ms_np;
    ms_sz = size_idx < 3 ? (int)size_idx : 0;
    ms_np = passes >= 10 ? 2 : passes >= 3 ? 1 : 0;
    ms_err = ms_first_bad = 0;
    ms_stop = 0;
    ms_pos = 0;
    ch_reset(CH_BW, "DRAM READ BANDWIDTH PER PASS (MiB/s)", C_STAR, 16);
    live_begin("Stability test (memory)");
    ms_t0 = sb_ms();
    for (ms_pass = 0; !ms_stop && ms_pass < passes; ++ms_pass) {
        ms_bandwidth();
        for (unsigned p = 0; p < NPAT && !ms_stop && !ms_err; ++p)
            if (ms_pattern(p, ms_pass * 0x2545f4914f6cdd1dULL + p + 0x0c))
                ms_stop = 1;
        if (ms_err)
            break;
    }
    live_end();
    struct ub u;
    ub_init(&u);
    ub_s(&u, ms_size_name[ms_sz]);
    ub_s(&u, ", ");
    ub_dec(&u, ms_pass);
    ub_s(&u, ms_pass == 1 ? " pass, " : " passes, ");
    ub_dec(&u, ms_err);
    ub_s(&u, " bad words, read ");
    ub_dec(&u, ms_bw_r);
    ub_s(&u, " MiB/s");
    if (ms_err) {
        ub_s(&u, ", first at ");
        ub_hex(&u, ms_first_bad, 9);
    }
    if (ms_stop)
        ub_s(&u, " (stopped by hand)");
    sb_strcpy(detail, cap, u.b);
    if (bw_read)
        *bw_read = (unsigned)ms_bw_r;
    int r = ms_err ? 0 : ms_stop ? -1 : 1;
    ms_sz = sz;
    ms_np = np;
    return r;
}

/* ---- display stress (no GPU: CPU fills and scrolls) ------------------------------------------- */

static char ds_detail[96];
static int ds_done;

static int ds_keys_stop(uint64_t *pw)
{
    unsigned k = sb_keys();
    if (k & SB_K_BIXBY)
        return 1;
    if (k & SB_K_POWER) {
        if (!*pw)
            *pw = sb_ms() | 1U;
        return sb_ms() - *pw >= UI_POWER_LONG_MS;
    }
    *pw = 0;
    return 0;
}

static void ds_label(const char *s)
{
    int w = ui_text_w(F_DETAIL, s) + 80;
    ui_round(UI_W / 2 - w / 2, UI_H - 230, w, 96, 48, 0xc0000000U);
    ui_text_c(F_DETAIL, UI_W / 2, UI_H - 168, s, C_L1);
}

void lab_display_stress(void)
{
    static const uint32_t cols[] = { 0xffff0000U, 0xff00ff00U, 0xff0000ffU, 0xffffffffU, 0xff000000U,
                                     0xffffff00U, 0xff00ffffU, 0xffff00ffU, 0xff808080U };
    const char *mode_name[3] = { "full-screen fills", "scrolling bars", "checkerboard flip" };
    uint64_t frames[3] = { 0 }, worst[3] = { 0 }, ms[3] = { 0 };
    uint64_t pw = 0;
    int stop = 0;
    ui_guard(0);
    ui_noclip();
    sb_keys_release_wait();
    for (int mode = 0; mode < 3 && !stop; ++mode) {
        uint64_t t0 = sb_ms();
        unsigned f = 0;
        if (mode == 1)
            for (int y = 0; y < UI_H; y += 40)
                ui_fill(0, y, UI_W, 40, cols[(y / 40) % 9]);
        while (sb_ms() - t0 < 10000 && !stop) {
            uint64_t a = sb_us();
            if (mode == 0) {
                ui_fill(0, 0, UI_W, UI_H, cols[f % 9]);
            } else if (mode == 1) {
                ui_scroll_rows(0, UI_H, -40);
                ui_fill(0, UI_H - 40, UI_W, 40, cols[f % 9]);
            } else {
                for (int y = 0; y < UI_H; y += 80)
                    for (int x = 0; x < UI_W; x += 80)
                        ui_fill(x, y, 80, 80, (((x + y) / 80 + (int)f) & 1) ? 0xffffffffU : 0xff000000U);
            }
            struct ub u;
            ub_init(&u);
            ub_s(&u, mode_name[mode]);
            ub_s(&u, "  " U_DOT "  frame ");
            ub_dec(&u, f);
            ub_s(&u, "  " U_DOT "  hold Power: stop");
            ds_label(u.b);
            ui_present();
            uint64_t d = sb_us() - a;
            if (d > worst[mode])
                worst[mode] = d;
            ++f;
            stop = ds_keys_stop(&pw);
        }
        frames[mode] = f;
        ms[mode] = sb_ms() - t0;
    }
    sb_keys_release_wait();
    ui_input_reset();
    ui_guard(armed_tier() ? 14 : 0);
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    struct ub u;
    ub_init(&u);
    for (int m = 0; m < 3; ++m) {
        ub_dec(&u, ms[m] ? frames[m] * 1000U / ms[m] : 0);
        ub_s(&u, m == 0 ? " fps fill, " : m == 1 ? " fps scroll, " : " fps flip; worst ");
    }
    uint64_t w = worst[0] > worst[1] ? worst[0] : worst[1];
    ub_dec(&u, (w > worst[2] ? w : worst[2]) / 1000U);
    ub_s(&u, " ms");
    sb_strcpy(ds_detail, sizeof(ds_detail), u.b);
    int ok = !stop && dialog("Display stress", "Did the panel show every frame cleanly?",
                             "No tearing lines, flicker bands, dead or stuck pixels.", "PASS");
    result("Display stress", ok, stop ? "stopped by hand" : ds_detail);
    ds_done = 1;
}

/* ---- touch grid ---------------------------------------------------------------------------------- */

#define TG_COLS 6
#define TG_ROWS 12
#define TG_Y0 260

void lab_touch_grid(void)
{
    uint8_t hit[TG_ROWS][TG_COLS] = { { 0 } };
    unsigned n = 0, total = TG_COLS * TG_ROWS;
    int cw = UI_W / TG_COLS, chh = (UI_H - TG_Y0) / TG_ROWS;
    uint64_t pw = 0;
    int stop = 0;
    if (!sb_tp.ok)
        (void)sb_touch_start();
    ui_guard(0);
    ui_noclip();
    sb_keys_release_wait();
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    for (int r = 0; r < TG_ROWS; ++r)
        for (int c = 0; c < TG_COLS; ++c)
            ui_round(c * cw + 6, TG_Y0 + r * chh + 6, cw - 12, chh - 12, 18, C_CARD2);
    for (;;) {
        struct ub u;
        ui_fill(0, 0, UI_W, TG_Y0, C_BG);
        ub_init(&u);
        ub_dec(&u, n);
        ub_s(&u, " of ");
        ub_dec(&u, total);
        ub_s(&u, " cells");
        ui_text(F_TITLE_S, 48, 130, u.b, n == total ? C_GREEN : C_L1);
        ui_text(F_SMALL, 48, 210, sb_tp.ok ? "Touch every cell. Hold Power to stop." : "Touch did not start. Hold Power to leave.",
                sb_tp.ok ? C_L2 : C_RED);
        ui_present();
        if (n == total || stop)
            break;
        int id[10], st[10], x[10], y[10];
        int k = sb_touch_events(id, st, x, y, 10);
        for (int i = 0; i < k; ++i) {
            if (st[i] == 3 || y[i] < TG_Y0)
                continue;
            int c = x[i] / cw, r = (y[i] - TG_Y0) / chh;
            if (c < 0 || c >= TG_COLS || r < 0 || r >= TG_ROWS || hit[r][c])
                continue;
            hit[r][c] = 1;
            ++n;
            ui_round(c * cw + 6, TG_Y0 + r * chh + 6, cw - 12, chh - 12, 18, C_GREEN);
        }
        stop = ds_keys_stop(&pw);
        sb_udelay(4000);
    }
    sb_mdelay(n == total ? 600 : 0);
    sb_keys_release_wait();
    ui_input_reset();
    ui_guard(armed_tier() ? 14 : 0);
    ui_fill(0, 0, UI_W, UI_H, C_BG);
    struct ub d;
    ub_init(&d);
    ub_dec(&d, n);
    ub_c(&d, '/');
    ub_dec(&d, total);
    ub_s(&d, " cells");
    if (n < total) {
        ub_s(&d, "; first missing: row ");
        for (int r = 0; r < TG_ROWS; ++r)
            for (int c = 0; c < TG_COLS; ++c)
                if (!hit[r][c]) {
                    ub_dec(&d, (uint64_t)r + 1U);
                    ub_s(&d, " col ");
                    ub_dec(&d, (uint64_t)c + 1U);
                    r = TG_ROWS;
                    break;
                }
    }
    result("Touch grid", n == total, d.b);
}

/* ---- storage benchmark and verify ---------------------------------------------------------------- */

static const uint64_t sb_mib[] = { 16, 64, 256 };
static const unsigned sb_rnd[] = { 500, 2000, 10000 };
static int sbn_med = 0, sbn_sz = 0, sbn_rn = 0, sbn_done, sbn_stop;
static uint64_t sbn_seq, sbn_iops, sbn_lat_avg, sbn_lat_max, sbn_mism, sbn_err, sbn_pos, sbn_total;
static const char *sbn_phase = "";
static char sbn_detail[96];

static void fill_sbn_live(void)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, sbn_total ? sbn_pos * 100U / sbn_total : 0);
    ub_s(&u, " %");
    kv_bigc(sbn_phase, u.b, sbn_err || sbn_mism ? C_RED : C_L1);
    kv_dec("Sequential read", sbn_seq, " MiB/s");
    kv_dec("Random 4 KiB reads", sbn_iops, " IOPS");
    kv_dec("Read errors", sbn_err, 0);
    kv_dec("Verify mismatches", sbn_mism, " MiB chunks");
    kv_chart(CH_BW, 420);
    kv_note("Hold Power to stop. Reads only: nothing is written.");
}

static void fill_sbn_result(void)
{
    kv_bigc(sbn_stop ? "stopped" : "finished", sbn_err || sbn_mism ? "FAIL" : "PASS", sbn_err || sbn_mism ? C_RED : C_GREEN);
    kv_add("Result", sbn_detail);
    kv_dec("Sequential read", sbn_seq, " MiB/s");
    kv_dec("Random 4 KiB reads", sbn_iops, " IOPS");
    kv_dec("Average latency", sbn_lat_avg, " " U_MICRO "s");
    kv_dec("Worst latency", sbn_lat_max, " " U_MICRO "s");
    kv_dec("Verify mismatches", sbn_mism, " MiB chunks");
    kv_chart(CH_BW, 420);
}

static uint64_t rnd_state = 0x853c49e6748fea9bULL;

static uint64_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return rnd_state;
}

static void sbn_run(void)
{
    unsigned m = sbn_med ? SB_MED_SD : SB_MED_UFS;
    uint64_t first = 0, last = 0;
    if (m == SB_MED_UFS) {
        if (!gpt_find("SYSTEM", &first, &last)) {
            (void)dialog_danger("Storage test", "No SYSTEM partition in the GPT.", 0, "OK");
            return;
        }
    } else {
        sd_allow();
        if (!sd_up()) {
            (void)dialog_danger("Storage test", "The card is not ready.", sd_rep.why ? sd_rep.why : "", "OK");
            return;
        }
        first = 0;
        last = sd_sectors_hint() / 8U;
        if (last < 65536U)
            last = 65536U;
    }
    uint64_t blocks = sb_mib[sbn_sz] * 256U;
    if (blocks > last - first + 1)
        blocks = last - first + 1;
    static uint32_t crc1[256];
    sbn_seq = sbn_iops = sbn_lat_avg = sbn_lat_max = sbn_mism = sbn_err = 0;
    sbn_stop = 0;
    ch_reset(CH_BW, "SEQUENTIAL READ (MiB/s PER MiB)", C_STAR, (unsigned)(blocks / 256U) * 2U);
    live_begin("Storage test");
    sbn_total = blocks * 2U + sb_rnd[sbn_rn];
    sbn_pos = 0;
    /* pass 1: sequential read, timed, a CRC per MiB; pass 2: the same again, CRCs compared */
    for (int pass = 0; pass < 2 && !sbn_stop; ++pass) {
        sbn_phase = pass ? "verify (second read)" : "sequential read";
        uint64_t ticks = 0;
        for (uint64_t i = 0; i < blocks && !sbn_stop; i += 256) {
            uint64_t n = blocks - i < 256 ? blocks - i : 256;
            uint64_t t = sb_ticks();
            int r = sb_blk_read(m, 0, first + i, n, (void *)SB_IO_BUF);
            uint64_t d = sb_ticks() - t;
            ticks += d;
            if (r) {
                ++sbn_err;
            } else {
                uint32_t c = sb_crc32((const void *)SB_IO_BUF, n * 4096U);
                unsigned k = (unsigned)(i / 256U) % 256U;
                if (!pass)
                    crc1[k] = c;
                else if (crc1[k] != c)
                    ++sbn_mism;
            }
            if (d)
                ch_push(CH_BW, (int32_t)(n * 4096U * sb_freq() / d >> 20));
            sbn_pos += n;
            if (live_poll(fill_sbn_live, 250))
                sbn_stop = 1;
        }
        if (!pass && ticks)
            sbn_seq = blocks * 4096U * sb_freq() / ticks >> 20;
    }
    /* random 4 KiB reads across the whole range of the partition / card */
    sbn_phase = "random 4 KiB reads";
    uint64_t span = last - first + 1, tsum = 0, done = 0;
    uint64_t t0 = sb_ticks();
    for (unsigned i = 0; i < sb_rnd[sbn_rn] && !sbn_stop; ++i) {
        uint64_t t = sb_ticks();
        if (sb_blk_read(m, 0, first + rnd() % span, 1, (void *)SB_IO_BUF))
            ++sbn_err;
        uint64_t us = (sb_ticks() - t) * 1000000U / sb_freq();
        tsum += us;
        if (us > sbn_lat_max)
            sbn_lat_max = us;
        ++done;
        ++sbn_pos;
        if ((i & 31U) == 31U && live_poll(fill_sbn_live, 250))
            sbn_stop = 1;
    }
    uint64_t all = sb_ticks() - t0;
    sbn_iops = all ? done * sb_freq() / all : 0;
    sbn_lat_avg = done ? tsum / done : 0;
    live_end();
    struct ub u;
    ub_init(&u);
    ub_s(&u, m == SB_MED_SD ? "microSD: " : "UFS SYSTEM: ");
    ub_dec(&u, sbn_seq);
    ub_s(&u, " MiB/s, ");
    ub_dec(&u, sbn_iops);
    ub_s(&u, " IOPS, ");
    ub_dec(&u, sbn_err);
    ub_s(&u, " errors, ");
    ub_dec(&u, sbn_mism);
    ub_s(&u, " mismatches");
    sb_strcpy(sbn_detail, sizeof(sbn_detail), u.b);
    result(m == SB_MED_SD ? "Storage test (card)" : "Storage test (UFS)", !sbn_err && !sbn_mism, sbn_detail);
    sbn_done = 1;
    info_screen("Storage test result", fill_sbn_result, 0);
}

static void build_sbn(void)
{
    kv_hdr("OPTIONS (POWER CHANGES)");
    row_value("Medium", sbn_med ? "microSD card (from block 0)" : "UFS (the SYSTEM partition)", 0);
    struct ub u;
    ub_init(&u);
    ub_dec(&u, sb_mib[sbn_sz]);
    ub_s(&u, " MiB, read twice");
    row_value("Sequential", u.b, 0);
    ub_init(&u);
    ub_dec(&u, sb_rnd[sbn_rn]);
    ub_s(&u, " reads");
    row_value("Random 4 KiB", u.b, 0);
    kv_hdr("RUN");
    row_add("Start", "read only; hold Power to stop", 0);
    row_add("Last result", sbn_done ? sbn_detail : "not run", sbn_done ? 0 : R_DIS);
    kv_note("Verify: the range is read a second time and every MiB's CRC32 must match the first read.");
}

static int act_sbn(int i)
{
    switch (i) {
    case 0: sbn_med = !sbn_med; break;
    case 1: sbn_sz = nxt(sbn_sz, 3); break;
    case 2: sbn_rn = nxt(sbn_rn, 3); break;
    case 3: sbn_run(); break;
    case 4: info_screen("Storage test result", fill_sbn_result, 0); break;
    default: break;
    }
    return 0;
}

void lab_storage_bench(void)
{
    list_screen("Storage test", build_sbn, act_sbn);
}

/* ---- battery tests -------------------------------------------------------------------------------- */

struct fg {
    int ok;
    int mv, avg_mv, ocv_mv, ma, avg_ma, soc10, vfsoc10, avsoc10;
    int rep_mah, full_mah, fullnom_mah, design_mah, cycles_raw, status, temp10, input, chg_dtls, bat_dtls;
    int iin_ma, vsys_mv;
};

static int fg_s16(uint8_t reg, int *v)
{
    uint16_t w;
    if (!sb_fg_word_checked(reg, &w, 0, 0))
        return 0;
    *v = (int16_t)w;
    return 1;
}

/* the gauge's words with a retry; the voltages also checked against 2.5..4.7 V (VSYS 2.5..5.5 V) */
#define FGW(reg, w) sb_fg_word_checked(reg, w, 0, 0)
#define FGV(reg, w) sb_fg_word_checked(reg, w, 32000, 60160)

static void fg_read(struct fg *f)
{
    uint16_t w;
    uint8_t c;
    int s;
    f->ok = sb_fg_word_checked(0x06, &w, 0, 0x6500);
    if (!f->ok)
        return;
    f->soc10 = (int)(w >> 8) * 10 + (int)((w & 0xffU) * 10U / 256U);
    /* a sample without a plausible cell voltage is not a sample (it used to be 0 mV, which the
     * discharge test's 3.45 V guard and minimum took for an empty cell) */
    f->ok = FGV(0x09, &w);
    if (!f->ok)
        return;
    f->mv = (int)((uint32_t)w * 5U / 64U);
    f->vfsoc10 = FGW(0xff, &w) ? (int)(w >> 8) * 10 + (int)((w & 0xffU) * 10U / 256U) : -1;
    f->avsoc10 = FGW(0x0e, &w) ? (int)(w >> 8) * 10 + (int)((w & 0xffU) * 10U / 256U) : -1;
    f->avg_mv = FGV(0x19, &w) ? (int)((uint32_t)w * 5U / 64U) : f->mv;
    f->ocv_mv = FGV(0xfb, &w) ? (int)((uint32_t)w * 5U / 64U) : 0;
    f->ma = fg_s16(0x0a, &s) ? s * 5 / 16 : 0;
    f->avg_ma = fg_s16(0x0b, &s) ? s * 5 / 16 : f->ma;
    f->rep_mah = FGW(0x05, &w) ? w : 0;
    f->full_mah = FGW(0x35, &w) ? w : 0;
    f->fullnom_mah = FGW(0x23, &w) ? w : 0;
    f->design_mah = FGW(0x18, &w) ? w : 0;
    f->cycles_raw = FGW(0x17, &w) ? w : -1;
    f->status = FGW(0x00, &w) ? w : -1;
    f->temp10 = fg_s16(0x08, &s) ? s * 10 / 256 : -1000;
    f->iin_ma = FGW(0xd0, &w) ? (int)((uint32_t)w / 8U) : -1;
    f->vsys_mv = sb_fg_word_checked(0xb1, &w, 16000, 35200) ? (int)((uint32_t)w * 5U / 32U) : -1;
    f->input = sb_chg_read(0xb2, &c) ? ((c & 0x20U) ? 2 : (c & 0x40U) ? 1 : 0) : -1;
    f->chg_dtls = sb_chg_read(0xb4, &c) ? (int)(c & 0xfU) : -1;
    f->bat_dtls = sb_chg_read(0xb4, &c) ? (int)((c >> 4) & 7U) : -1;
}

static void ub_c10(struct ub *u, int v10, const char *unit)
{
    if (v10 < 0) {
        ub_c(u, '-');
        v10 = -v10;
    }
    ub_dec(u, (uint64_t)v10 / 10U);
    ub_c(u, '.');
    ub_dec(u, (uint64_t)v10 % 10U);
    ub_s(u, unit);
}

static const char *chg_state(int d)
{
    return d < 0 ? "?" : d <= 2 ? "charging" : d <= 4 ? "full / top-off" : d == 0xb ? "WATCHDOG EXPIRED" :
           d == 8 || d == 0xa ? "not charging (off)" : "not charging";
}

static struct fg F;

static void fill_bat_info(void)
{
    struct ub u;
    fg_read(&F);
    temps_sample(1);
    if (!F.ok) {
        kv_addc("Fuel gauge", sb_max_why ? sb_max_why : "no answer", C_RED);
        return;
    }
    ub_init(&u);
    ub_c10(&u, F.soc10, " %");
    kv_big("state of charge (RepSOC)", u.b);
    kv_hdr("NOW");
    ub_init(&u);
    ub_fix(&u, (uint64_t)F.mv, 3);
    ub_s(&u, " V (avg ");
    ub_fix(&u, (uint64_t)F.avg_mv, 3);
    ub_s(&u, ", open circuit ");
    ub_fix(&u, (uint64_t)F.ocv_mv, 3);
    ub_c(&u, ')');
    kv_add("Voltage", u.b);
    ub_init(&u);
    ub_sdec(&u, F.ma);
    ub_s(&u, " mA (avg ");
    ub_sdec(&u, F.avg_ma);
    ub_s(&u, "), + = charging");
    kv_add("Current", u.b);
    ub_init(&u);
    ub_sdec(&u, (int64_t)F.mv * F.ma / 1000);
    ub_s(&u, " mW");
    kv_add("Power", u.b);
    int bt = bat_c10();
    if (bt > -1000)
        kv_deg10("Battery temperature", bt, "(thermistor, live)");
    else
        kv_deg10("Battery temperature", F.temp10, "(fuel-gauge register: S-Boot's last write)");
    kv_hdr("HEALTH");
    ub_init(&u);
    ub_dec(&u, (uint64_t)F.full_mah);
    ub_s(&u, " of ");
    ub_dec(&u, (uint64_t)F.design_mah);
    ub_s(&u, " mAh design");
    kv_add("Full capacity (learned)", u.b);
    if (F.design_mah > 0) {
        int h = F.full_mah * 100 / F.design_mah;
        ub_init(&u);
        ub_dec(&u, (uint64_t)h);
        ub_s(&u, h >= 80 ? " % (good)" : h >= 70 ? " % (worn)" : " % (replace soon)");
        kv_addc("Health (full / design)", u.b, h >= 80 ? C_GREEN : h >= 70 ? C_ORANGE : C_RED);
        ub_init(&u);
        ub_dec(&u, (uint64_t)(F.fullnom_mah * 100 / F.design_mah));
        ub_s(&u, " %");
        kv_add("Age (nominal / design)", u.b);
    }
    kv_dec("Remaining", (uint64_t)F.rep_mah, " mAh");
    ub_init(&u);
    ub_dec(&u, (uint64_t)(F.cycles_raw < 0 ? 0 : F.cycles_raw));
    ub_s(&u, " raw = ");
    ub_c10(&u, (F.cycles_raw < 0 ? 0 : F.cycles_raw) / 10, " cycles (LSB 1 %, unverified)");
    kv_add("Cycles", u.b);
    ub_init(&u);
    ub_c10(&u, F.vfsoc10, " % (voltage) / ");
    ub_c10(&u, F.avsoc10, " % (average)");
    kv_add("Other SOC", u.b);
    kv_add("Battery", F.status >= 0 && (F.status & 8) ? "ABSENT (status bit 3)" : "present");
    kv_hdr("CHARGER");
    kv_add("Input", F.input == 2 ? "wireless" : F.input == 1 ? "USB cable" : F.input == 0 ? "none" : "?");
    kv_addc("State", chg_state(F.chg_dtls), F.chg_dtls == 0xb ? C_ORANGE : C_L1);
    if (F.iin_ma >= 0)
        kv_dec("Input current (IIN)", (uint64_t)F.iin_ma, " mA");
    if (F.vsys_mv >= 0) {
        ub_init(&u);
        ub_fix(&u, (uint64_t)F.vsys_mv, 3);
        ub_s(&u, " V");
        kv_add("System voltage (VSYS)", u.b);
    }
}

/* discharge, sag, charge */
static const unsigned bd_dur[] = { 120, 300, 600, 1800 };
static const char *const bd_dur_name[] = { "2 min", "5 min", "10 min", "30 min" };
static const char *const bd_load_name[] = { "all cores + white screen", "all cores", "white screen only", "idle" };
static int bd_d = 1, bd_l = 0, bd_kind, bd_stop, bd_done[3], bd_screen;
static int64_t bd_uah;                       /* charge moved, uAh (+ into the battery) */
static int bd_mv0, bd_mvmin, bd_soc0, bd_soc1, bd_t0c, bd_tmax;
static uint64_t bd_t0, bd_samples;
static int64_t bd_ma_sum, bd_mw_sum;
static char bd_detail[3][96];
static int sag_cycle, sag_phase, sag_vi[8], sag_vl[8], sag_ii[8], sag_il[8], sag_vmin, sag_r[8];
static int wd_feed, wd_fed, wd_expired;
static struct sb_auth wd_auth;

static void fill_bd_live(void)
{
    struct ub u;
    uint64_t s = (sb_ms() - bd_t0) / 1000U;
    ub_init(&u);
    ub_fix(&u, (uint64_t)F.mv, 3);
    ub_s(&u, " V");
    kv_bigc(bd_kind == 0 ? "discharge under load" : bd_kind == 1 ? (sag_phase ? "sag test: LOAD" : "sag test: idle") :
            "charge test", u.b, F.mv < 3500 ? C_RED : C_L1);
    ub_init(&u);
    fmt_mmss(&u, s);
    kv_add("Time", u.b);
    ub_init(&u);
    ub_sdec(&u, F.ma);
    ub_s(&u, " mA  " U_DOT "  ");
    ub_sdec(&u, (int64_t)F.mv * F.ma / 1000);
    ub_s(&u, " mW");
    kv_add("Current, power", u.b);
    ub_init(&u);
    ub_c10(&u, F.soc10, " %  " U_DOT "  ");
    ub_sdec(&u, bd_uah / 1000);
    ub_s(&u, " mAh moved");
    kv_add("Charge", u.b);
    int bt = bat_c10();
    kv_deg10("Battery temperature", bt > -1000 ? bt : F.temp10, bt > -1000 ? "(thermistor)" : "(FG register, stale)");
    if (bd_kind == 2) {
        kv_addc("Charger", chg_state(F.chg_dtls), F.chg_dtls == 0xb ? C_ORANGE : C_L1);
        kv_add("Watchdog feed", wd_feed ? "every 30 s (CNFG_06)" : "off");
    }
    if (bd_kind == 1) {
        ub_init(&u);
        ub_s(&u, "cycle ");
        ub_dec(&u, (uint64_t)sag_cycle + 1U);
        ub_s(&u, " of 5");
        kv_add("Sag test", u.b);
    }
    kv_chart(CH_MV, 400);
    kv_chart(CH_MA, 400);
    kv_note("Hold Power to stop. Guards: 3.45 V, 10 %, battery 45 " U_DEG "C, CPU limit.");
}

static void fill_bd_result(void)
{
    struct ub u;
    kv_bigc(bd_kind == 0 ? "discharge under load" : bd_kind == 1 ? "voltage sag and stability" : "charge test",
            bd_stop == 2 ? "GUARD" : "DONE", bd_stop == 2 ? C_ORANGE : C_GREEN);
    kv_add("Result", bd_detail[bd_kind]);
    if (bd_kind == 1) {
        kv_hdr("PER CYCLE (IDLE / LOAD)");
        for (int i = 0; i < 5; ++i) {
            struct ub k;
            ub_init(&k);
            ub_s(&k, "Cycle ");
            ub_dec(&k, (uint64_t)i + 1U);
            ub_init(&u);
            ub_dec(&u, (uint64_t)sag_vi[i]);
            ub_s(&u, " / ");
            ub_dec(&u, (uint64_t)sag_vl[i]);
            ub_s(&u, " mV, ");
            ub_sdec(&u, sag_ii[i]);
            ub_s(&u, " / ");
            ub_sdec(&u, sag_il[i]);
            ub_s(&u, " mA, R ");
            ub_sdec(&u, sag_r[i]);
            ub_s(&u, " m" "\xce\xa9");
            kv_add(k.b, u.b);
        }
    }
    kv_chart(CH_MV, 400);
    kv_chart(CH_MA, 400);
}

static int bd_guard(void)
{
    int bt = bat_c10();
    if (bt <= -1000 && F.ok && F.temp10 > -1000)
        bt = F.temp10;                      /* no valid thermistor sample: the gauge's register */
    if (F.ok && F.mv && F.mv < 3450 && bd_kind != 2)
        return 1;
    if (F.ok && F.soc10 < 100 && bd_kind != 2)
        return 1;
    if (bt > -1000 && bt >= 450)
        return 1;
    if (over_limit(T.hottest))
        return 1;
    return 0;
}

static void bd_sample(void)
{
    static uint64_t last;
    uint64_t now = sb_ms();
    fg_read(&F);
    temps_sample(1);
    if (!F.ok)
        return;
    if (bd_samples && last)             /* coulomb counting from the second sample of this run */
        bd_uah += (int64_t)F.ma * (int64_t)(now - last) * 1000 / 3600000;
    last = now;
    ++bd_samples;
    bd_ma_sum += F.ma;
    bd_mw_sum += (int64_t)F.mv * F.ma / 1000;
    if (F.mv < bd_mvmin)
        bd_mvmin = F.mv;
    int bt = bat_c10();
    if (bt / 10 > bd_tmax)
        bd_tmax = bt / 10;
    ch_push(CH_MV, F.mv);
    ch_push(CH_MA, F.ma);
}

static void wd_kick(void)
{
    uint8_t v;
    if (!wd_feed || !sb_chg_read(0xbd, &v))
        return;
    if (lab_max(0x69, 0xbd, (unsigned)((v & 0x0cU) | 1U), &wd_auth))
        ++wd_fed;
}

static void bd_run(int kind)
{
    bd_kind = kind;
    fg_read(&F);
    if (!F.ok) {
        (void)dialog_danger("Battery test", "The fuel gauge does not answer.", sb_max_why ? sb_max_why : "", "OK");
        return;
    }
    if (kind != 2 && F.input > 0 &&
        !dialog("Charger connected", "The charger feeds the phone during this test.",
                "Unplug it for a true discharge measurement.", "RUN ANYWAY"))
        return;
    if (kind == 2 && F.input <= 0) {
        (void)dialog("Charge test", "Plug in a charger first.", "USB or wireless.", "OK");
        return;
    }
    if (kind == 2 && wd_feed && !sb_auth_valid(&wd_auth)) {
        const char *why;
        uint8_t v = 0;
        (void)sb_chg_read(0xbd, &v);
        struct action a = { "Feed the charger watchdog", "CNFG06", { "Writes CNFG_06 WDTCLR = 01 every 30 s, as Linux does",
                            "S-Boot enabled the charger watchdog: without the feed, charging stops after it expires", 0, 0 },
                            "stop the feed; the next OS sets the charger up again", "", "WDTCLR = 01", "", 0,
                            SB_SP_MAX, 0, 0, 0x69U << 8 | 0xbd, 1 };
        a.tier = max_tier(0x69, 0xbd, (v & 0x0cU) | 1U, &why);
        if (!lab_confirm(&a, &wd_auth))
            wd_feed = 0;
    }
    unsigned dur = kind == 0 ? bd_dur[bd_d] : kind == 1 ? 100 : bd_dur[bd_d];
    ch_reset(CH_MV, "CELL VOLTAGE (mV)", C_STAR, dur);
    ch_reset(CH_MA, "CURRENT (mA, + = INTO THE BATTERY)", C_GREEN, dur);
    bd_uah = 0;
    bd_samples = 0;
    bd_ma_sum = bd_mw_sum = 0;
    bd_mv0 = bd_mvmin = F.mv;
    bd_soc0 = F.soc10;
    bd_tmax = -100;
    bd_stop = 0;
    wd_fed = 0;
    wd_expired = 0;
    sag_cycle = 0;
    sag_phase = 0;
    sag_vmin = 99999;
    int want_cores = kind == 0 ? (bd_l <= 1) : kind == 1;
    if (want_cores && !guard_ok("Battery test"))
        return;
    run_mask = want_cores ? cores_offer(core_mask[opt.cores], "Battery test") : 0x0fU;
    if (!run_mask)
        return;
    bd_screen = kind == 0 && (bd_l == 0 || bd_l == 2);
    for (unsigned w = 0; w < SB_NCORES; ++w)
        core_res[w] = 1;
    if (want_cores && kind == 0)
        (void)load_start(run_mask, kern_mask[0]);
    else if (kind == 1)
        load_alloc(kern_mask[0]);
    live_begin(kind == 0 ? "Discharge test" : kind == 1 ? "Sag test" : "Charge test");
    bd_t0 = sb_ms();
    bd_t0c = (int)T.hottest;
    uint64_t next_s = sb_ms() + 1000, phase_t0 = sb_ms(), next_wd = sb_ms();
    int sum_v = 0, sum_i = 0, nsum = 0;
    while (!bd_stop) {
        if (core_res[0] == 0)
            load_slice(20);
        else
            sb_mdelay(5);
        if (bd_screen) {                     /* full white over the page body: the panel's top load */
            ui_fill(0, UI_VIEW_Y0, UI_W, UI_VIEW_Y1 - UI_VIEW_Y0, 0xffffffffU);
            live_next = sb_ms() + 100000;
            ui_present();
            struct ui_ev e;
            ui_poll(&e, 0);
            if (e.type == EV_BACK || e.type == EV_HOME || lab_stop_check())
                bd_stop = 1;
        } else if (live_poll(fill_bd_live, 250)) {
            bd_stop = 1;
        }
        if (sb_ms() < next_s)
            continue;
        next_s += 1000;
        bd_sample();
        if (F.ok) {
            sum_v += F.mv;
            sum_i += F.ma;
            ++nsum;
            if (F.mv < sag_vmin)
                sag_vmin = F.mv;
        }
        if (kind == 2 && F.chg_dtls == 0xb)
            wd_expired = 1;
        if (kind == 2 && wd_feed && sb_ms() >= next_wd) {
            wd_kick();
            next_wd = sb_ms() + 30000;
        }
        if (bd_guard())
            bd_stop = 2;
        uint64_t in_phase = (sb_ms() - phase_t0) / 1000U;
        if (kind == 1 && in_phase >= 10) {
            /* the last 6 s of each 10 s phase (the gauge's current settles) */
            if (sag_phase == 0) {
                sag_vi[sag_cycle] = nsum ? sum_v / nsum : 0;
                sag_ii[sag_cycle] = nsum ? sum_i / nsum : 0;
                (void)load_start(run_mask, kern_mask[0]);
                sag_phase = 1;
            } else {
                sag_vl[sag_cycle] = nsum ? sum_v / nsum : 0;
                sag_il[sag_cycle] = nsum ? sum_i / nsum : 0;
                load_stop();
                for (unsigned w = 0; w < SB_NCORES; ++w)
                    core_res[w] = 1;
                int di = sag_ii[sag_cycle] - sag_il[sag_cycle];          /* more discharge under load */
                sag_r[sag_cycle] = di > 0 ? (sag_vi[sag_cycle] - sag_vl[sag_cycle]) * 1000 / di : 0;
                sag_phase = 0;
                if (++sag_cycle >= 5)
                    bd_stop = 3;
            }
            phase_t0 = sb_ms();
            sum_v = sum_i = nsum = 0;
        } else if (kind == 1 && in_phase < 4) {
            sum_v = sum_i = nsum = 0;
        }
        if (kind != 1 && (sb_ms() - bd_t0) / 1000U >= dur)
            bd_stop = 3;
        if (kind == 0 && load_faults())
            bd_stop = 2;
    }
    if (sb_smp_running() || !core_res[0])
        load_stop();
    live_end();
    bd_soc1 = F.soc10;
    struct ub u;
    ub_init(&u);
    uint64_t secs = (sb_ms() - bd_t0) / 1000U;
    if (kind == 0) {
        int64_t avg = bd_samples ? bd_ma_sum / (int64_t)bd_samples : 0;
        int dsoc = bd_soc0 - bd_soc1;
        ub_s(&u, "avg ");
        ub_sdec(&u, avg);
        ub_s(&u, " mA, ");
        ub_sdec(&u, bd_samples ? bd_mw_sum / (int64_t)bd_samples : 0);
        ub_s(&u, " mW, ");
        ub_sdec(&u, -bd_uah / 1000);
        ub_s(&u, " mAh in ");
        fmt_mmss(&u, secs);
        if (dsoc >= 20) {
            ub_s(&u, "; capacity ~");
            ub_sdec(&u, -bd_uah / 1000 * 1000 / dsoc);
            ub_s(&u, " mAh");
        } else {
            ub_s(&u, "; SOC moved <2 %: no capacity estimate");
        }
        if (avg < 0 && F.rep_mah > 0) {
            ub_s(&u, "; runtime at this load ~");
            ub_dec(&u, (uint64_t)F.rep_mah * 60U / (uint64_t)(-avg));
            ub_s(&u, " min");
        }
    } else if (kind == 1) {
        int rmin = 99999, rmax = 0, rsum = 0, n = 0;
        for (int i = 0; i < sag_cycle && i < 5; ++i)
            if (sag_r[i] > 0) {
                rmin = sag_r[i] < rmin ? sag_r[i] : rmin;
                rmax = sag_r[i] > rmax ? sag_r[i] : rmax;
                rsum += sag_r[i];
                ++n;
            }
        ub_s(&u, "R ");
        ub_dec(&u, (uint64_t)(n ? rsum / n : 0));
        ub_s(&u, " mOhm (");
        ub_dec(&u, (uint64_t)(n ? rmin : 0));
        ub_s(&u, "..");
        ub_dec(&u, (uint64_t)rmax);
        ub_s(&u, "), min ");
        ub_dec(&u, (uint64_t)(sag_vmin == 99999 ? 0 : sag_vmin));
        ub_s(&u, " mV, ");
        ub_dec(&u, (uint64_t)n);
        ub_s(&u, " cycles");
        sag_r[7] = n ? rsum / n : 0;
        sag_r[6] = n ? rmax - rmin : 0;
    } else {
        int64_t avg = bd_samples ? bd_ma_sum / (int64_t)bd_samples : 0;
        ub_s(&u, "avg +");
        ub_sdec(&u, avg);
        ub_s(&u, " mA, ");
        ub_sdec(&u, bd_uah / 1000);
        ub_s(&u, " mAh in ");
        fmt_mmss(&u, secs);
        if (avg > 50 && F.full_mah > F.rep_mah) {
            ub_s(&u, "; full in ~");
            ub_dec(&u, (uint64_t)(F.full_mah - F.rep_mah) * 60U / (uint64_t)avg);
            ub_s(&u, " min");
        }
        if (wd_expired)
            ub_s(&u, "; WATCHDOG EXPIRED");
    }
    if (bd_stop == 2)
        ub_s(&u, " (stopped by a guard)");
    sb_strcpy(bd_detail[kind], sizeof(bd_detail[0]), u.b);
    int pass = kind == 0 ? bd_samples > 0 && bd_mvmin >= 3300 :
               kind == 1 ? sag_cycle >= 5 && sag_r[7] > 0 && sag_r[7] < 300 && sag_vmin >= 3300 :
                           bd_samples > 0 && bd_ma_sum / (int64_t)bd_samples > 50 && !wd_expired;
    result(kind == 0 ? "Battery discharge" : kind == 1 ? "Battery sag" : "Battery charge", pass, bd_detail[kind]);
    bd_done[kind] = 1;
    info_screen(kind == 0 ? "Discharge result" : kind == 1 ? "Sag result" : "Charge result", fill_bd_result, 0);
}

static void fill_capacity(void)
{
    struct ub u;
    fg_read(&F);
    kv_hdr("FUEL GAUGE (MODELGAUGE)");
    kv_dec("Design", (uint64_t)F.design_mah, " mAh");
    kv_dec("Full, learned (FullCapRep)", (uint64_t)F.full_mah, " mAh");
    kv_dec("Full, nominal (FullCapNom)", (uint64_t)F.fullnom_mah, " mAh");
    kv_dec("Remaining now", (uint64_t)F.rep_mah, " mAh");
    kv_hdr("MEASURED HERE");
    kv_add("Discharge test", bd_done[0] ? bd_detail[0] : "not run (Battery > Discharge under load)");
    kv_add("Sag test", bd_done[1] ? bd_detail[1] : "not run");
    kv_add("Charge test", bd_done[2] ? bd_detail[2] : "not run");
    ub_init(&u);
    ub_s(&u, "The estimate needs the state of charge to move at least 2 % during the discharge test: "
             "capacity = mAh counted / SOC drop. A 30 min run under full load is enough.");
    kv_note(u.b);
}

static void build_bat(void)
{
    kv_hdr("INFORMATION");
    row_add("Battery health", "voltage, current, SOC, capacity, cycles, temperature (live)", 0);
    row_add("Capacity estimate", "fuel gauge + this session's measurements", 0);
    kv_hdr("OPTIONS (POWER CHANGES)");
    row_value("Duration", bd_dur_name[bd_d], 0);
    row_value("Discharge load", bd_load_name[bd_l], 0);
    row_value("Charger watchdog feed", wd_feed ? "on (asks once)" : "off", 0);
    kv_hdr("TESTS");
    row_add("Discharge under load", bd_done[0] ? bd_detail[0] : "mAh counted, power, capacity estimate", R_WARN);
    row_add("Voltage sag and stability", bd_done[1] ? bd_detail[1] : "5 x (10 s idle, 10 s full load): internal resistance", R_WARN);
    row_add("Charge test", bd_done[2] ? bd_detail[2] : "charger in: current, watchdog, time to full", 0);
    kv_note("Guards stop every test at 3.45 V, 10 %, a battery at 45 " U_DEG "C or the CPU temperature limit. "
            "Battery temperature: the live thermistor (CMGP ADC) when it answers.");
}

static int act_bat(int i)
{
    switch (i) {
    case 0: info_screen("Battery health", fill_bat_info, 2000); break;
    case 1: info_screen("Capacity estimate", fill_capacity, 0); break;
    case 2: bd_d = nxt(bd_d, 4); break;
    case 3: bd_l = nxt(bd_l, 4); break;
    case 4: wd_feed = !wd_feed; break;
    case 5: bd_run(0); break;
    case 6: bd_run(1); break;
    case 7: bd_run(2); break;
    default: break;
    }
    return 0;
}

void lab_battery(void)
{
    list_screen("Battery tests", build_bat, act_bat);
}

/* ---- full health check ------------------------------------------------------------------------- */

enum { HC_PEND, HC_RUN, HC_PASS, HC_WARN, HC_FAIL, HC_SKIP };
#define HC_N 15
static const char *const hc_name[HC_N] = {
    "Timer and RTC", "PMIC (ACPM)", "Touch controller", "Battery and fuel gauge", "Battery temperature",
    "Thermal sensors", "CPU: every core, 5 s", "Temperature under load", "Memory: 32 MiB patterns",
    "Memory bandwidth", "UFS and GPT", "UFS read and verify", "OS store and catalog", "microSD card", "Display",
};
static int hc_state[HC_N], hc_ran, hc_stop;
static char hc_det[HC_N][72];

static void hc_set(int i, int st, const char *d)
{
    hc_state[i] = st;
    sb_strcpy(hc_det[i], sizeof(hc_det[0]), d ? d : "");
}

static void fill_hc(void)
{
    unsigned p = 0, w = 0, f = 0, s = 0;
    for (int i = 0; i < HC_N; ++i) {
        p += hc_state[i] == HC_PASS;
        w += hc_state[i] == HC_WARN;
        f += hc_state[i] == HC_FAIL;
        s += hc_state[i] == HC_SKIP;
    }
    struct ub u;
    ub_init(&u);
    ub_dec(&u, p);
    ub_s(&u, " pass  ");
    ub_dec(&u, w);
    ub_s(&u, " warn  ");
    ub_dec(&u, f);
    ub_s(&u, " fail");
    kv_bigc(hc_stop ? "stopped" : "full health check", u.b, f ? C_RED : w ? C_ORANGE : C_GREEN);
    kv_hdr("CHECKS");
    for (int i = 0; i < HC_N; ++i) {
        struct ub k;
        ub_init(&k);
        const char *mark = hc_state[i] == HC_PASS ? U_CHECK " " : hc_state[i] == HC_FAIL ? "x " :
                           hc_state[i] == HC_WARN ? "! " : hc_state[i] == HC_RUN ? U_RIGHT " " : "  ";
        ub_s(&k, mark);
        ub_s(&k, hc_name[i]);
        const char *v = hc_det[i][0] ? hc_det[i] : hc_state[i] == HC_PEND ? "waiting" : hc_state[i] == HC_RUN ? "running" :
                        hc_state[i] == HC_SKIP ? "skipped" : "";
        uint32_t c = hc_state[i] == HC_PASS ? C_GREEN : hc_state[i] == HC_FAIL ? C_RED :
                     hc_state[i] == HC_WARN ? C_ORANGE : C_L2;
        kv_addc(k.b, v, c);
    }
    (void)s;
    kv_note("Hold Power to stop. Results go to the test history; Tools > Save report writes them to the OS store.");
}

/* Run a step's own polling; 1 = stop. */
static int hc_poll(void)
{
    if (live_poll(fill_hc, 250))
        hc_stop = 1;
    return hc_stop;
}

static void hc_run(void)
{
    struct ub u;
    for (int i = 0; i < HC_N; ++i)
        hc_set(i, HC_PEND, "");
    hc_stop = 0;
    run_mask = cores_offer(0xffU, "Full health check");
    if (!run_mask)
        return;
    live_begin("Full health check");
    for (int i = 0; i < HC_N && !hc_stop; ++i) {
        hc_state[i] = HC_RUN;
        (void)hc_poll();
        ub_init(&u);
        switch (i) {
        case 0: {
            struct sb_rtc a, b;
            sb_rtc_read(&a);
            uint64_t t0 = sb_ms();
            while (sb_ms() - t0 < 1200 && !hc_poll())
                sb_mdelay(10);
            sb_rtc_read(&b);
            int running = a.ok && b.ok && (a.s != b.s || a.mi != b.mi);
            ub_s(&u, a.ok ? "RTC " : "RTC not readable");
            if (a.ok) {
                ub_dec(&u, b.y);
                ub_c(&u, '-');
                ub_dec2(&u, b.mo);
                ub_c(&u, '-');
                ub_dec2(&u, b.d);
                ub_s(&u, running ? ", running" : ", NOT running");
            }
            ub_s(&u, sb_freq() == 26000000U ? "; timer 26 MHz" : "; timer clock unusual");
            hc_set(i, a.ok && running ? HC_PASS : HC_FAIL, u.b);
            break;
        }
        case 1: {
            uint8_t id = 0;
            char b[25];
            int ok = sb_pmic_read(0, 0, &id);
            ub_s(&u, ok ? "S2MPS18 id " : "no answer");
            if (ok) {
                ub_hex(&u, id, 2);
                if (sb_acpm_build(b)) {
                    ub_s(&u, ", ACPM ");
                    ub_s(&u, b);
                }
            }
            hc_set(i, ok ? HC_PASS : HC_FAIL, u.b);
            break;
        }
        case 2:
            if (!sb_tp.ok)
                (void)sb_touch_start();
            hc_set(i, sb_tp.ok ? HC_PASS : HC_FAIL, sb_tp.ok ? "S6SY761 answers" : sb_tp.why ? sb_tp.why : "not started");
            break;
        case 3: {
            fg_read(&F);
            int h = F.design_mah ? F.full_mah * 100 / F.design_mah : 0;
            int st = !F.ok ? HC_FAIL : (F.status >= 0 && (F.status & 8)) ? HC_FAIL :
                     F.mv < 3300 || F.mv > 4450 ? HC_FAIL : F.design_mah != 0x0b97 || h < 70 ? HC_WARN :
                     h < 80 ? HC_WARN : HC_PASS;
            if (F.ok) {
                ub_c10(&u, F.soc10, " %, ");
                ub_fix(&u, (uint64_t)F.mv, 2);
                ub_s(&u, " V, health ");
                ub_dec(&u, (uint64_t)h);
                ub_s(&u, " %");
                if (F.design_mah != 0x0b97)
                    ub_s(&u, ", model not loaded");
            } else {
                ub_s(&u, "fuel gauge not answering");
            }
            hc_set(i, st, u.b);
            break;
        }
        case 4: {
            temps_sample(1);
            int bt = bat_c10();
            if (bt <= -1000) {
                hc_set(i, HC_SKIP, th_why ? th_why : "ADC not readable");
                break;
            }
            ub_c10(&u, bt, " " U_DEG "C (thermistor)");
            hc_set(i, bt > 550 ? HC_FAIL : bt > 450 || bt < 0 ? HC_WARN : HC_PASS, u.b);
            break;
        }
        case 5:
            temps_sample(0);
            ub_s(&u, T.top ? "TOP on" : "TOP OFF");
            ub_s(&u, T.sub ? ", SUB on, hottest " : ", SUB OFF, hottest ");
            ub_sdec(&u, T.hottest);
            ub_s(&u, " " U_DEG "C");
            hc_set(i, !T.top || !T.sub ? HC_FAIL : T.hottest >= 60 ? HC_WARN : HC_PASS, u.b);
            break;
        case 6: {
            int t_start = T.hottest, t_peak = T.hottest;
            unsigned n = load_start(run_mask, KERN_INT | KERN_FP | KERN_NEON | KERN_MEM);
            uint64_t t0 = sb_ms(), next = sb_ms() + 1000;
            while (sb_ms() - t0 < 5000 && !hc_stop) {
                load_slice(20);
                (void)hc_poll();
                if (sb_ms() >= next) {
                    next += 1000;
                    temps_sample(0);
                    if (T.hottest > t_peak)
                        t_peak = T.hottest;
                    if (T.hottest >= 90)
                        break;
                }
            }
            for (unsigned w = 0; w < SB_NCORES; ++w)
                last_it[w] = 0;
            load_rates();
            load_stop();
            uint64_t e = load_errors();
            unsigned bad = 0, ran = 0;
            for (unsigned w = 0; w < SB_NCORES; ++w) {
                ran += !core_res[w] && rate[w] > 0;
                bad += core_res[w] != 0 && core_res[w] != 1;
            }
            ub_dec(&u, ran);
            ub_s(&u, " of 8 cores ran, ");
            ub_dec(&u, e);
            ub_s(&u, " errors");
            if (!(run_mask & 0xf0U))
                ub_s(&u, " (M3 not enabled)");
            hc_set(i, e || load_faults() ? HC_FAIL : ran == 8 ? HC_PASS : ran >= 4 ? HC_WARN : HC_FAIL, u.b);
            (void)n;
            (void)bad;
            ub_init(&u);
            ub_sdec(&u, t_start);
            ub_s(&u, " " U_RIGHT " ");
            ub_sdec(&u, t_peak);
            ub_s(&u, " " U_DEG "C in 5 s");
            hc_set(7, t_peak >= 90 ? HC_FAIL : t_peak - t_start > 25 ? HC_WARN : HC_PASS, u.b);
            ++i;                                /* step 7 is done with step 6 */
            break;
        }
        case 8: {
            volatile uint64_t *m = (volatile uint64_t *)SB_TEST_BASE;
            uint64_t n = (32U << 20) / 8U, bad = 0;
            for (unsigned p = 0; p < 4 && !hc_stop; ++p) {
                unsigned pp = p == 0 ? 2 : p == 1 ? 3 : p == 2 ? 4 : 8;
                for (uint64_t k = 0; k < n; ++k)
                    m[k] = pat_val(pp, k, 7);
                (void)hc_poll();
                for (uint64_t k = 0; k < n; ++k)
                    bad += m[k] != pat_val(pp, k, 7);
                (void)hc_poll();
            }
            ub_dec(&u, bad);
            ub_s(&u, " bad words (4 patterns)");
            hc_set(i, bad ? HC_FAIL : HC_PASS, u.b);
            break;
        }
        case 9:
            ms_bandwidth();
            ub_dec(&u, ms_bw_r);
            ub_s(&u, " MiB/s read, ");
            ub_dec(&u, ms_bw_w);
            ub_s(&u, " write (non-cacheable)");
            hc_set(i, ms_bw_r < 100 ? HC_WARN : HC_PASS, u.b);
            break;
        case 10: {
            struct gpt_head p, b;
            int up = ufs_up(), hp = up && gpt_header(1, &p);
            int hb = hp && p.alt_lba && gpt_header(p.alt_lba, &b);
            int ok = hp && p.hdr_crc == p.hdr_crc_calc && p.ent_crc == p.ent_crc_calc;
            int okb = hb && b.hdr_crc == b.hdr_crc_calc && b.ent_crc == b.ent_crc_calc;
            ub_s(&u, !up ? "UFS not ready" : !hp ? "no primary GPT" : ok ? "primary GPT ok" : "primary GPT CRC BAD");
            ub_s(&u, okb ? ", backup ok, " : ", backup BAD, ");
            ub_dec(&u, (uint64_t)(gpt_load() > 0 ? gpt_n : 0));
            ub_s(&u, " partitions");
            hc_set(i, !up || !ok ? HC_FAIL : !okb ? HC_WARN : HC_PASS, u.b);
            break;
        }
        case 11: {
            uint64_t f = 0, l = 0;
            if (!gpt_find("SYSTEM", &f, &l)) {
                hc_set(i, HC_SKIP, "no SYSTEM partition");
                break;
            }
            uint32_t c1[16];
            uint64_t ticks = 0, mism = 0, err = 0;
            for (int pass = 0; pass < 2 && !hc_stop; ++pass)
                for (unsigned k = 0; k < 16 && !hc_stop; ++k) {
                    uint64_t t = sb_ticks();
                    int r = sb_blk_read(SB_MED_UFS, 0, f + k * 256U, 256, (void *)SB_IO_BUF);
                    ticks += sb_ticks() - t;
                    if (r) {
                        ++err;
                        continue;
                    }
                    uint32_t c = sb_crc32((const void *)SB_IO_BUF, 1U << 20);
                    if (!pass)
                        c1[k] = c;
                    else
                        mism += c1[k] != c;
                    (void)hc_poll();
                }
            ub_dec(&u, ticks ? (32ULL << 20) * sb_freq() / ticks >> 20 : 0);
            ub_s(&u, " MiB/s, ");
            ub_dec(&u, err);
            ub_s(&u, " errors, ");
            ub_dec(&u, mism);
            ub_s(&u, " mismatches");
            hc_set(i, err || mism ? HC_FAIL : HC_PASS, u.b);
            break;
        }
        case 12: {
            int n = store_open(STORE_UFS);
            struct store_ent e;
            int eng = store_find("engineer.img", &e);
            ub_s(&u, "UFS store v");
            ub_dec(&u, (uint64_t)store_version(STORE_UFS));
            ub_s(&u, ", ");
            ub_dec(&u, (uint64_t)(n > 0 ? n : 0));
            ub_s(&u, " entries, catalog ");
            ub_dec(&u, cat_count());
            ub_s(&u, cat_builtin() ? " (built in)" : "");
            hc_set(i, n <= 0 ? HC_FAIL : cat_builtin() || !eng ? HC_WARN : HC_PASS, u.b);
            break;
        }
        case 13: {
            int present = !(*(volatile uint32_t *)0x14050044UL & (1U << 5));
            if (!present) {
                hc_set(i, HC_SKIP, "no card in the slot");
                break;
            }
            int up = sd_allowed() ? sd_up() : 0;
            hc_set(i, up ? HC_PASS : HC_WARN, up ? "card ready" : sd_allowed() ? (sd_rep.why ? sd_rep.why : "not ready") :
                                                 "card present (not started: Storage > microSD)");
            break;
        }
        case 14:
            hc_set(i, (*(volatile uint32_t *)0x16030070UL & 1U) ? HC_PASS : HC_WARN,
                   "DECON scanning out (look: Tests > Display test)");
            break;
        default:
            break;
        }
    }
    if (sb_smp_running())
        load_stop();
    for (int i = 0; i < HC_N; ++i)
        if (hc_state[i] == HC_RUN || hc_state[i] == HC_PEND)
            hc_set(i, HC_SKIP, hc_stop ? "not run (stopped)" : "not run");
    eng_build_page(&live_p, fill_hc);
    ui_present();
    live_end();
    unsigned f = 0, w = 0, p = 0;
    rep_line("== Full health check ==");
    for (int i = 0; i < HC_N; ++i) {
        f += hc_state[i] == HC_FAIL;
        w += hc_state[i] == HC_WARN;
        p += hc_state[i] == HC_PASS;
        ub_init(&u);
        ub_s(&u, hc_state[i] == HC_PASS ? "  pass " : hc_state[i] == HC_FAIL ? "  FAIL " : hc_state[i] == HC_WARN ? "  warn " :
                                                                                                                   "  skip ");
        ub_s(&u, hc_name[i]);
        ub_s(&u, ": ");
        ub_s(&u, hc_det[i]);
        rep_line(u.b);
    }
    ub_init(&u);
    ub_dec(&u, p);
    ub_s(&u, " pass, ");
    ub_dec(&u, w);
    ub_s(&u, " warn, ");
    ub_dec(&u, f);
    ub_s(&u, " fail");
    if (hc_stop)
        ub_s(&u, " (stopped)");
    test_record("Full health check", !f && !hc_stop, u.b);
    hc_ran = 1;
    info_screen("Health check result", fill_hc, 0);
}

static void build_hc_menu(void)
{
    kv_hdr("RUN");
    row_add("Start the full health check", "15 quick checks, about 30 s; hold Power to stop", 0);
    row_add("Last report", hc_ran ? "this session" : "not run", hc_ran ? 0 : R_DIS);
    row_add("Save report to the OS store", "this session's results as a text entry (asks first)", R_WARN);
    kv_hdr("WHAT IS CHECKED");
    for (int i = 0; i < HC_N; ++i)
        kv_add("", hc_name[i]);
    kv_note("The CPU step starts every core for 5 s (the M3 cores ask for the PMU sequence once per session).");
}

static int act_hc(int i)
{
    if (i == 0)
        hc_run();
    else if (i == 1)
        info_screen("Health check result", fill_hc, 0);
    else if (i == 2)
        lab_save_report();
    return 0;
}

void lab_health(void)
{
    list_screen("Full health check", build_hc_menu, act_hc);
}

/* ---- the report into the OS store ---------------------------------------------------------------- */

static void blob_s(char *b, uint64_t *n, uint64_t cap, const char *s)
{
    while (*s && *n + 1 < cap)
        b[(*n)++] = *s++;
}

void lab_save_report(void)
{
    struct sb_rtc r;
    struct ub u;
    const uint64_t cap = 512U << 10;
    static char *blob;
    if (!blob)
        blob = sb_alloc(cap);
    if (!blob)
        return;
    uint64_t n = 0;
    sb_rtc_read(&r);
    ub_init(&u);
    ub_s(&u, "lab-");
    if (r.ok) {
        ub_dec(&u, r.y);
        ub_dec2(&u, r.mo);
        ub_dec2(&u, r.d);
        ub_c(&u, '-');
        ub_dec2(&u, r.h);
        ub_dec2(&u, r.mi);
        ub_dec2(&u, r.s);
    } else {
        ub_s(&u, "up");
        ub_dec(&u, sb_ms() / 1000U);
    }
    ub_s(&u, ".txt");
    char name[32];
    sb_strcpy(name, sizeof(name), u.b);
    blob_s(blob, &n, cap, "Starbite Lab report (Engineer OS " ENG_VERSION ")\n");
    for (unsigned i = 0; i < rep_n; ++i) {
        blob_s(blob, &n, cap, rep[i]);
        blob_s(blob, &n, cap, "\n");
    }
    if (tlog_n) {
        blob_s(blob, &n, cap, "== Thermal log: s,phase,hottest,big,little,gpu,battery,batches_per_s,a55_mhz,m3_mhz ==\n");
        for (unsigned i = 0; i < tlog_n; ++i) {
            const struct tlog *l = &tlog[i];
            ub_init(&u);
            ub_dec(&u, l->t);
            ub_c(&u, ',');
            ub_dec(&u, l->phase);
            int v[5] = { l->hot, l->big, l->little, l->gpu, l->bat };
            for (int k = 0; k < 5; ++k) {
                ub_c(&u, ',');
                ub_sdec(&u, v[k]);
            }
            ub_c(&u, ',');
            ub_dec(&u, l->rate);
            ub_c(&u, ',');
            ub_dec(&u, l->a55);
            ub_c(&u, ',');
            ub_dec(&u, l->m3);
            ub_c(&u, '\n');
            blob_s(blob, &n, cap, u.b);
        }
    }
    blob_s(blob, &n, cap, "== Engineer OS log ==\n");
    for (unsigned i = 0; i < sb_log_count(); ++i) {
        blob_s(blob, &n, cap, sb_log_line(i));
        blob_s(blob, &n, cap, "\n");
    }
    unsigned m = store_free(STORE_UFS) * 4096U > n + 8192U ? STORE_UFS : STORE_SD;
    struct sb_auth au;
    char sz[32];
    ub_init(&u);
    ub_size(&u, n);
    sb_strcpy(sz, sizeof(sz), u.b);
    struct action a = { "Save the report", "", { "Appends one text entry to the OS store:", name,
                        "Existing entries are not touched (append, then a one-block commit)", 0 },
                        "Starbite > OS store > the entry > Remove (tier 2), or repack on the Mac", "", sz, "", TIER1,
                        m, 0, SB_KIND_STORE, 0, 0 };
    if (!lab_confirm(&a, &au) || !sb_gate_open(&au))
        return;
    struct store_ent e;
    int rr = store_append(m, name, ST_TEXT, blob, n, &e);
    sb_gate_close();
    ub_init(&u);
    ub_s(&u, rr ? "report NOT saved (" : "report saved: ");
    if (rr)
        ub_sdec(&u, rr);
    ub_s(&u, m == STORE_UFS ? (rr ? ")" : "ufs:") : (rr ? ")" : "sd:"));
    if (!rr)
        ub_s(&u, name);
    audit_line(u.b);
    ui_toast(u.b, rr ? C_RED : C_GREEN, 1500);
}
