/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS host test (docs/STARBITE-OS.md 12.2): the kernel's pure parts on the host.
 *   sbos-host-test                    every check below
 *   sbos-host-test apps A.sba ...     only: every app file passes the kernel's check (the build runs this)
 * Timers (order, priorities, repeats after a long sleep), the app file check against the real built apps and
 * broken copies of them, the placement and its relocations, the DVFS list, the heater controller against
 * scripted sensor traces (every refusal, every stop, the regulation, the cold start, the M3 ramp, the charger
 * pause), the battery model, the state slot format. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "kern.h"

static int fails, checks;
#define CHECK(c, ...)                                                                                             \
    do {                                                                                                         \
        ++checks;                                                                                                \
        if (!(c)) {                                                                                              \
            ++fails;                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                          \
            printf(__VA_ARGS__);                                                                                 \
            printf("\n");                                                                                        \
        }                                                                                                        \
    } while (0)

/* libsb bits the pure files use */
void sb_log(const char *s) { (void)s; }
void sb_logx(const char *a, uint64_t v) { (void)a; (void)v; }
/* heatk.c's worker side (not run here) */
struct sb_wslot *sb_smp_own_slot(unsigned w) { (void)w; return 0; }
int sb_worker_stop(void) { return 1; }
void sb_worker_publish(struct sb_wslot *s) { (void)s; }
uint64_t sb_ticks(void) { return 0; }

/* ---- the heat batch: inside its buffer, deterministic reference, no NaN ---- */
#include <math.h>
static void t_heatk(void)
{
    static struct heatk_cfg c;
    uint64_t guard = 64;
    uint64_t *mem = malloc((HEATK_WORDS + 2 * guard) * 8);
    for (uint64_t i = 0; i < HEATK_WORDS + 2 * guard; ++i)
        mem[i] = 0x5a5a5a5a5a5a5a5aULL ^ i;
    c.buf[1] = mem + guard;
    c.gold = heatk_batch();
    CHECK(heatk_batch() == c.gold, "the reference block is deterministic");
    unsigned bad = 0;
    for (uint64_t it = 0; it < 400; ++it)
        bad += heatk_mixed(&c, 1, it);
    CHECK(bad == 0, "400 batches, the reference block matched every time (%u)", bad);
    int intact = 1;
    for (uint64_t i = 0; i < guard; ++i)
        intact &= mem[i] == (0x5a5a5a5a5a5a5a5aULL ^ i) &&
                  mem[guard + HEATK_WORDS + i] == (0x5a5a5a5a5a5a5a5aULL ^ (guard + HEATK_WORDS + i));
    CHECK(intact, "the batches stay inside the core's 4 MiB buffer");
    uint64_t changed = 0;
    for (uint64_t i = 0; i < HEATK_WORDS; i += 4096)
        changed += c.buf[1][i] != (0x5a5a5a5a5a5a5a5aULL ^ (guard + i));
    CHECK(changed > HEATK_WORDS / 4096 / 2, "the data changes from batch to batch (%llu of %u sampled words)",
          (unsigned long long)changed, HEATK_WORDS / 4096);
    /* never periodic: after 4 full rounds of the far batches (each far slot visited 4 more times, where the
     * plain xorshift steps had come back to the same data) the words differ again */
    static uint64_t snap[HEATK_WORDS / 4096];
    for (uint64_t i = 0; i < HEATK_WORDS; i += 4096)
        snap[i / 4096] = c.buf[1][i];
    for (uint64_t it = 400; it < 400 + 16 * 7 * 4; ++it)
        bad += heatk_mixed(&c, 1, it);
    uint64_t again = 0, sampled = 0;
    for (uint64_t i = 0; i < 256U * 1024U / 8U + 7U * 512U * 1024U / 8U; i += 4096, ++sampled)
        again += c.buf[1][i] == snap[i / 4096];
    CHECK(again == 0, "the data never comes back (%llu of %llu sampled words repeated)", (unsigned long long)again,
          (unsigned long long)sampled);
    c.gold ^= 1;
    CHECK(heatk_mixed(&c, 1, 16) == 1, "a wrong reference is reported");
    free(mem);
}

/* ---- timers ---------------------------------------------------------------------------------- */
static void t_timers(void)
{
    unsigned o, id;
    kt_reset();
    CHECK(kt_next() == UINT64_MAX, "empty list has no deadline");
    kt_set(KT_UI, 1, 1000, 50, 0);
    kt_set(KT_HEATER, 1, 1000, 50, 1);
    kt_set(KT_APP0 + 2, 7, 1000, 10, 0);
    CHECK(kt_next() == 1010, "next deadline %llu", (unsigned long long)kt_next());
    CHECK(!kt_pop(1009, &o, &id), "nothing due before 1010");
    CHECK(kt_pop(1050, &o, &id) && o == KT_APP0 + 2 && id == 7, "earliest first: app timer");
    CHECK(kt_pop(1050, &o, &id) && o == KT_HEATER, "same deadline: the heater before the UI");
    CHECK(kt_pop(1050, &o, &id) && o == KT_UI, "then the UI");
    CHECK(!kt_pop(1050, &o, &id), "the heater repeats at 1100, not now");
    CHECK(kt_next() == 1100, "repeat re-armed from its deadline: %llu", (unsigned long long)kt_next());
    /* a long sleep: no burst of missed periods */
    CHECK(kt_pop(5000, &o, &id) && o == KT_HEATER, "late repeat fires once");
    CHECK(!kt_pop(5000, &o, &id), "no burst after a long sleep");
    CHECK(kt_next() > 5000 && kt_next() <= 5050, "next period after now: %llu", (unsigned long long)kt_next());
    kt_set(KT_HEATER, 1, 5000, 200, 1);             /* re-set replaces */
    CHECK(kt_next() == 5200, "re-set replaces the timer");
    kt_cancel(KT_HEATER, 1);
    CHECK(kt_next() == UINT64_MAX, "cancel");
    unsigned got = 0;
    for (unsigned i = 0; i < KT_MAX; ++i)
        got += kt_set(KT_APP0 + i / 16, i % 16, 0, 100 + i, 0) == 0;
    CHECK(got == KT_MAX - KT_RESERVE, "apps get at most KT_MAX - KT_RESERVE timers (%u)", got);
    CHECK(kt_set(KT_APP0 + 1, 99, 0, 1, 0) == -1, "a further app timer is refused");
    CHECK(kt_set(KT_APP0, 0, 0, 5, 0) == 0, "an app may re-set a timer it has");
    for (unsigned i = 0; i < 8; ++i)
        CHECK(kt_set(i, 0, 0, 10, 1) == 0, "kernel task %u still gets its timer", i);
    for (unsigned i = 0; i < 4; ++i)
        kt_cancel_owner(KT_APP0 + i);
    for (unsigned i = 0; i < 8; ++i)
        kt_cancel_owner(i);
    CHECK(kt_next() == UINT64_MAX, "cancel_owner");
}

/* ---- app files ------------------------------------------------------------------------------- */
static uint8_t *load(const char *p, uint64_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 64);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        return 0;
    }
    fclose(f);
    *n = (uint64_t)sz;
    return b;
}

static void recrc(uint8_t *b, uint64_t n)
{
    uint32_t c = app_crc(b, n);
    memcpy(b + 0x60, &c, 4);
}

static int t_app_file(const char *path, int full)
{
    uint64_t n;
    uint8_t *b = load(path, &n);
    CHECK(b != 0, "%s: readable", path);
    if (!b)
        return 1;
    const char *why = app_check(b, n, APP_SLOT_SIZE);
    CHECK(why == 0, "%s: accepted (%s)", path, why ? why : "");
    if (!full) {
        free(b);
        return why != 0;
    }
    struct sbapp_header *h = (struct sbapp_header *)b;
    uint8_t *c = malloc(n + 64);
#define BROKEN(desc, mut)                                                                                         \
    do {                                                                                                         \
        memcpy(c, b, n);                                                                                         \
        struct sbapp_header *x = (struct sbapp_header *)c;                                                       \
        (void)x;                                                                                                 \
        mut;                                                                                                     \
        recrc(c, n);                                                                                             \
        CHECK(app_check(c, n, APP_SLOT_SIZE) != 0, "%s: refused when %s", path, desc);                           \
    } while (0)
    BROKEN("the magic is wrong", x->magic[3] = 'X');
    BROKEN("the API is newer", x->api = SBOS_API + 1);
    BROKEN("the header size is wrong", x->header_size = 128);
    BROKEN("the id has a capital", x->id[0] = 'L');
    BROKEN("the id is not terminated", memset(x->id, 'a', sizeof(x->id)));
    BROKEN("the name is empty", x->name[0] = 0);
    BROKEN("file_size is not the file's", x->file_size += 16);
    BROKEN("the entry is inside the header", x->entry = 8);
    BROKEN("the entry is past the file", x->entry = (uint32_t)n);
    BROKEN("the entry is unaligned", x->entry |= 2);
    BROKEN("the bss does not fit the slot", x->bss_size = (uint32_t)(APP_SLOT_SIZE));
    BROKEN("the stack is huge", x->stack_size = 8U << 20);
    if (h->rela_count) {
        BROKEN("the relocations run past the file", x->rela_count = (uint32_t)(n / 24 + 1));
        BROKEN("the relocation table is unaligned", x->rela_off += 4);
        uint64_t off;
        memcpy(&off, b + h->rela_off, 8);
        BROKEN("a relocation targets the header", { uint64_t z = 8; memcpy(c + h->rela_off, &z, 8); });
        BROKEN("a relocation has another type", { uint64_t t = 257; memcpy(c + h->rela_off + 8, &t, 8); });
        BROKEN("a relocation points far away", { int64_t v = 1LL << 40; memcpy(c + h->rela_off + 16, &v, 8); });
        /* placement: the relocation target holds slot + addend */
        static uint8_t slot[4 << 20] __attribute__((aligned(4096)));
        app_place(b, n, (uintptr_t)slot);
        int64_t add;
        memcpy(&add, b + h->rela_off + 16, 8);
        uint64_t got;
        memcpy(&got, slot + off, 8);
        CHECK(got == (uint64_t)(uintptr_t)slot + (uint64_t)add, "%s: relocation applied", path);
        int zero = 1;
        for (uint32_t i = 0; i < h->bss_size; ++i)
            zero &= slot[n + i] == 0;
        CHECK(zero, "%s: bss zeroed", path);
    }
    memcpy(c, b, n);
    c[n / 2] ^= 0x40;
    CHECK(app_check(c, n, APP_SLOT_SIZE) != 0, "%s: refused with one bit flipped (CRC)", path);
    CHECK(app_check(b, n - 4, APP_SLOT_SIZE) != 0, "%s: refused when cut short", path);
    CHECK(app_check(b, 100, APP_SLOT_SIZE) != 0, "%s: refused when shorter than its header", path);
    free(c);
    free(b);
    return 0;
}

/* ---- DVFS list --------------------------------------------------------------------------------- */
static void t_dvfs(void)
{
    CHECK(sb_dvfs_level_listed(2, 1794000), "A55 1794 MHz listed");
    CHECK(sb_dvfs_level_listed(2, 1053000) && sb_dvfs_level_listed(2, 1248000), "A55 restore and cold levels");
    CHECK(sb_dvfs_level_listed(2, 1898000) && sb_dvfs_level_listed(2, 1950000) && !sb_dvfs_level_listed(2, 2002000),
          "A55: 1898 / 1950 MHz listed (Manual only, above the ECT's 1794 cap), 2002 never");
    CHECK(!sb_dvfs_level_listed(3, 1924000) && !sb_dvfs_level_listed(3, 2704000), "M3 above 1794 MHz never (no OCP handler)");
    CHECK(sb_dvfs_level_listed(3, 1794000) && sb_dvfs_level_listed(3, 1469000) && sb_dvfs_level_listed(3, 1066000),
          "M3 heater and restore levels listed");
    CHECK(!sb_dvfs_level_listed(3, 2704000) && !sb_dvfs_level_listed(3, 1924000), "M3 above the all-core 1794 never");
    CHECK(!sb_dvfs_level_listed(3, 455000), "455 MHz is not an M3 level");
    CHECK(!sb_dvfs_level_listed(2, 1794001), "only exact levels");
    /* 1.1-beta6: Samsung's M3 core-count table (exynos-ocp): 1 core 2704, 2 cores 2314, 3-4 cores 1794 */
    CHECK(sb_m3_ocp_level_ok(2704000, 1) && !sb_m3_ocp_level_ok(2704000, 2) && sb_m3_ocp_level_ok(2314000, 2) &&
          !sb_m3_ocp_level_ok(2314000, 3) && sb_m3_ocp_level_ok(1794000, 4) && !sb_m3_ocp_level_ok(2886000, 1) &&
          !sb_m3_ocp_level_ok(2860000, 1) && !sb_m3_ocp_level_ok(1924000, 1) && !sb_m3_ocp_level_ok(2704000, 0 + 4),
          "M3: 2704 for 1 core, 2314 for 2, 1794 for 3-4, nothing else above 1794");
    CHECK(sb_dvfs_level_listed(4, 572000) && sb_dvfs_level_listed(4, 260000), "GPU 572 / 260 MHz listed");
    CHECK(sb_dvfs_level_listed(4, 598000), "GPU 598 MHz listed (the ECT's top, 0.6.2)");
    CHECK(!sb_dvfs_level_listed(4, 611000) && !sb_dvfs_level_listed(4, 650000) && !sb_dvfs_level_listed(4, 702000) &&
          !sb_dvfs_level_listed(4, 1794000), "GPU above 598 MHz never");
    for (unsigned d = 0; d < 16; ++d)
        if (d != 2 && d != 3 && d != 4)
            CHECK(!sb_dvfs_level_listed(d, 1794000) && !sb_dvfs_level_listed(d, 455000) &&
                  !sb_dvfs_level_listed(d, 572000), "domain %u refused (MIF 0, INT 1 among them)", d);
}

/* ---- heater controller ------------------------------------------------------------------------- */
static struct heat_in base_in(uint64_t now)
{
    struct heat_in in;
    memset(&in, 0, sizeof(in));
    in.now = now;
    in.bat_ok = 1, in.bat_c10 = 260, in.bat_raw = 1950;
    in.skin_ok = 1, in.skin_c10 = 300;
    in.usb_ok = 1, in.usb_c10 = 255;
    in.chg_ok = 1, in.chg_c10 = 262;
    in.soc_ok = 1, in.soc_c10 = 380;
    in.tmu_on = 1, in.trip_armed = 1;
    in.gauge_ok = 1, in.pct = 80, in.mv = 3950, in.ma = -400;
    in.charger = 0;
    return in;
}

static int refuse(struct heat_in in, int level, const char *what)
{
    struct heat_ctl c;
    heat_ctl_init(&c, 0);
    int r = heat_ctl_request(&c, level, 30, &in);
    CHECK(r == SBOS_EAGAIN && c.state == HEAT_ST_REFUSED, "refused when %s (got %d, %s)", what, r, c.why);
    return r;
}

/* step the controller every 250 ms for ms, calling mut(&in, t) first; the controller after */
typedef void (*mut_fn)(struct heat_in *, uint64_t t);
static void run(struct heat_ctl *c, struct heat_in *in, uint64_t from, uint64_t ms, mut_fn mut)
{
    for (uint64_t t = from; t <= from + ms; t += 250) {
        in->now = t;
        in->bat_raw += 1;                       /* a live sensor changes */
        if (mut)
            mut(in, t);
        heat_ctl_step(c, in);
    }
}

static int stopped_with(struct heat_ctl *c, int st, const char *word)
{
    return c->state == st && strstr(c->why, word) != 0 && c->cores == 0 && c->a55_khz == 0 && c->m3_khz == 0;
}

static void m_hot_battery(struct heat_in *in, uint64_t t) { if (t > 20000) in->bat_c10 = 421; }
static void m_band(struct heat_in *in, uint64_t t) { in->bat_c10 = t > 20000 ? 402 : 300; }
static void m_hot_soc(struct heat_in *in, uint64_t t) { if (t > 10000) in->soc_c10 = 851; }
static void m_hot_skin(struct heat_in *in, uint64_t t) { if (t > 10000) in->skin_c10 = 461; }
static void m_blind_bat(struct heat_in *in, uint64_t t) { if (t > 10000) in->bat_ok = 0; }
static void m_blind_soc(struct heat_in *in, uint64_t t) { if (t > 10000) in->soc_ok = 0; }
static void m_stuck(struct heat_in *in, uint64_t t) { in->bat_raw = 1950; in->soc_c10 = 380 + (int)(t / 1000); }
static void m_lag(struct heat_in *in, uint64_t t) { in->soc_c10 = 380 + (int)(t / 1200); in->bat_c10 = 260 + (int)(t / 300000); }
static void m_low_pct(struct heat_in *in, uint64_t t) { if (t > 10000) in->pct = 14; }
static void m_low_mv(struct heat_in *in, uint64_t t) { if (t > 10000) in->mv = 3440; }
static void m_high_ma(struct heat_in *in, uint64_t t) { if (t > 10000) in->ma = -3600; }
static void m_bad(struct heat_in *in, uint64_t t) { if (t > 10000) in->bad_results = 1; }
static void m_fault(struct heat_in *in, uint64_t t) { if (t > 10000) in->worker_fault = 1; }
static void m_charger(struct heat_in *in, uint64_t t) { in->charger = (t > 10000 && t < 20000) ? 1 : 0; }
static void m_charger_unknown(struct heat_in *in, uint64_t t) { in->charger = (t > 10000 && t < 20000) ? -1 : 0; }
static void m_soft_soc(struct heat_in *in, uint64_t t) { in->soc_c10 = t > 20000 ? 760 : 400; }
static void m_sag(struct heat_in *in, uint64_t t) { in->mv = t > 20000 ? 3600 : 3950; }
static void m_cold(struct heat_in *in, uint64_t t) { in->soc_c10 = 150 + (int)(t / 2000); in->mv = 3920; }

static void stop_case(mut_fn m, uint64_t ms, int st, const char *word)
{
    struct heat_ctl c;
    struct heat_in in = base_in(0);
    heat_ctl_init(&c, 0);
    CHECK(heat_ctl_request(&c, HEAT_HIGH, 30, &in) == 0, "start for the '%s' case", word);
    run(&c, &in, 0, ms, m);
    CHECK(stopped_with(&c, st, word), "stop '%s': state %d, cores %d, why \"%s\"", word, c.state, c.cores, c.why);
}

static void t_heater(void)
{
    struct heat_in in = base_in(0);
    /* refusals */
    in = base_in(0); in.bat_ok = 0; refuse(in, HEAT_LOW, "no battery temperature");
    in = base_in(0); in.bat_c10 = 380; refuse(in, HEAT_LOW, "the battery is 38 C");
    in = base_in(0); in.usb_c10 = 320; in.chg_c10 = 200; refuse(in, HEAT_LOW, "the battery sensor disagrees");
    in = base_in(0); in.usb_ok = 0; in.chg_ok = 0; refuse(in, HEAT_LOW, "nothing to cross-check");
    in = base_in(0); in.soc_ok = 0; refuse(in, HEAT_LOW, "no SoC temperature");
    in = base_in(0); in.tmu_on = 0; refuse(in, HEAT_LOW, "the TMU is off");
    in = base_in(0); in.trip_armed = 0; refuse(in, HEAT_LOW, "the hardware trip is not armed");
    in = base_in(0); in.gauge_ok = 0; refuse(in, HEAT_LOW, "no gauge");
    in = base_in(0); in.pct = 19; refuse(in, HEAT_LOW, "19 % for Low");
    in = base_in(0); in.pct = 25; refuse(in, HEAT_HIGH, "25 % for High");
    in = base_in(0); in.charger = 1; refuse(in, HEAT_LOW, "a charger is connected");
    in = base_in(0); in.charger = -1; refuse(in, HEAT_LOW, "the charger state is unknown");

    /* a normal start: one core more per second, up to the level's cores */
    struct heat_ctl c;
    heat_ctl_init(&c, 0);
    in = base_in(0);
    CHECK(heat_ctl_request(&c, HEAT_LOW, 30, &in) == 0 && c.state == HEAT_ST_RUNNING, "Low starts");
    CHECK(c.cores == 1 && c.a55_khz == 1794000 && c.m3_khz == 0, "Low: one A55 worker at 1794 MHz first");
    run(&c, &in, 0, 10000, 0);
    CHECK(c.cores == 3 && c.m3_khz == 0, "Low: three A55 workers, no M3 (cores %d)", c.cores);
    CHECK(heat_ctl_request(&c, HEAT_OFF, 0, &in) == 0 && c.state == HEAT_ST_OFF && c.cores == 0, "Off stops");

    /* High: the M3 ramp 1469 -> 1794 MHz after 60 s */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    heat_ctl_request(&c, HEAT_HIGH, 30, &in);
    run(&c, &in, 0, 10000, 0);
    CHECK(c.cores == 7 && c.m3_khz == 1469000, "High: 7 workers, the M3s start at 1469 MHz (%d, %u)", c.cores, c.m3_khz);
    run(&c, &in, 10250, 60000, 0);
    CHECK(c.m3_khz == 1794000, "High: the M3s at 1794 MHz after the ramp (%u)", c.m3_khz);

    /* Extreme (0.6.1): flat out: every worker, the A55 table's top, the M3s at 1794 MHz at once; no shedding at the
     * band, the skin, a hot SoC, the sag or the soft current; no timer; the stops: 55 C battery, 110 C SoC */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    CHECK(heat_ctl_request(&c, HEAT_EXTREME, 30, &in) == 0 && c.a55_khz == 1794000, "Extreme: the A55s at 1794 MHz (the ECT's cap)");
    run(&c, &in, 0, 10000, 0);
    CHECK(c.cores == 7 && c.m3_khz == 1794000, "Extreme: 7 workers, the M3s at 1794 MHz at once (%d, %u)", c.cores, c.m3_khz);
    in.bat_c10 = 520; in.skin_c10 = 490; in.soc_c10 = 1000; in.mv = 3500; in.ma = -3300;
    run(&c, &in, 10250, 120000, 0);
    CHECK(c.state == HEAT_ST_RUNNING && c.cores == 7, "Extreme: no regulation at 52 C battery, 49 C skin, 100 C SoC, "
          "sag and 3.3 A (state %d, cores %d, why \"%s\")", c.state, c.cores, c.why);
    run(&c, &in, 130250, 3600000, 0);
    CHECK(c.state == HEAT_ST_RUNNING, "Extreme: no timer (still running after an hour)");
    in.bat_c10 = 550;
    run(&c, &in, 3730250, 1000, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "55"), "Extreme: stops at 55 C battery (\"%s\")", c.why);
    in = base_in(0); in.bat_c10 = 450; in.usb_c10 = 450; in.chg_c10 = 450;
    heat_ctl_init(&c, 0);
    CHECK(heat_ctl_request(&c, HEAT_EXTREME, 30, &in) == 0, "Extreme starts at 45 C battery (Low would not)");
    in.soc_c10 = 1120; in.mv = 3300; in.ma = -3400;
    run(&c, &in, 0, 10000, 0);
    CHECK(c.state == HEAT_ST_RUNNING && c.cores == 7, "Extreme (0.6.2): no stop of ours at 112 C inside (the hardware "
          "trip's) or 3.3 V (state %d, \"%s\")", c.state, c.why);
    in.ma = -3600;
    run(&c, &in, 10250, 1000, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "3.5 A"), "Extreme: the 3.5 A stop stays (\"%s\")", c.why);
    heat_ctl_init(&c, 0);
    in = base_in(0);
    c.no_current_stop = 1;
    heat_ctl_request(&c, HEAT_EXTREME, 0, &in);
    in.ma = -3600;
    run(&c, &in, 0, 2000, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "3.5 A"), "Extreme: the switch is Manual's only");
    heat_ctl_init(&c, 0);
    in = base_in(0);
    c.no_current_stop = 1;
    heat_ctl_request(&c, HEAT_MANUAL, 0, &in);
    in.ma = -4500;
    run(&c, &in, 0, 5000, 0);
    CHECK(c.state == HEAT_ST_RUNNING, "Manual with the current stop off: runs at 4.5 A");
    heat_ctl_init(&c, 0);
    in = base_in(0);
    c.soc_stop_c10 = 1000;
    heat_ctl_request(&c, HEAT_MANUAL, 0, &in);
    in.soc_c10 = 1005;
    run(&c, &in, 0, 1000, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "SoC stop"), "Manual: the user's SoC stop at 100 C (\"%s\")", c.why);
    heat_ctl_init(&c, 0);
    in = base_in(0);
    c.soc_stop_c10 = 1000;
    heat_ctl_request(&c, HEAT_EXTREME, 0, &in);
    in.soc_c10 = 1005;
    run(&c, &in, 0, 1000, 0);
    CHECK(c.state == HEAT_ST_RUNNING, "Extreme: no SoC stop (Manual's only)");
    in = base_in(0);
    heat_ctl_init(&c, 0);
    heat_ctl_request(&c, HEAT_EXTREME, 0, &in);
    in.trip_armed = 0;
    run(&c, &in, 0, 1000, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "trip"), "Extreme stops if the hardware trip reads unarmed (\"%s\")", c.why);
    /* 1.0-rc2: a lost fuel gauge stops heat after 10 s (it holds the reserve, current and voltage stops) */
    for (int lv = HEAT_HIGH; lv <= HEAT_KEEPWARM; lv += HEAT_EXTREME - HEAT_HIGH) {
        heat_ctl_init(&c, 0);
        in = base_in(0); in.bat_c10 = 50; in.usb_c10 = 50; in.chg_c10 = 50;
        heat_ctl_request(&c, lv, 30, &in);
        in.gauge_ok = 0;
        run(&c, &in, 0, 9000, 0);
        CHECK(c.state != HEAT_ST_STOPPED, "level %d: a 9 s gauge gap is tolerated", lv);
        run(&c, &in, 9250, 2000, 0);
        CHECK(stopped_with(&c, HEAT_ST_STOPPED, "charge and current"), "level %d: a lost gauge stops heat (\"%s\")", lv, c.why);
    }
    in = base_in(0); in.pct = 15; refuse(in, HEAT_EXTREME, "15 % (the reserve) for Extreme");
    in = base_in(0); in.pct = 16;
    heat_ctl_init(&c, 0);
    CHECK(heat_ctl_request(&c, HEAT_EXTREME, 30, &in) == 0, "Extreme starts at 16 %% (above the reserve)");
    in.pct = 14;
    run(&c, &in, 0, 1000, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "Reserve"), "Extreme stops at the reserve (\"%s\")", c.why);
    heat_ctl_init(&c, 0);
    c.reserve_pct = 0;                      /* the user's override */
    in.pct = 14;
    CHECK(heat_ctl_request(&c, HEAT_EXTREME, 30, &in) == 0, "Extreme at 14 %% with the reserve overridden");
    in.pct = 2;
    run(&c, &in, 0, 5000, 0);
    CHECK(c.state == HEAT_ST_RUNNING, "the overridden reserve: runs at 2 %%");
    /* Keep warm: the battery's target */
    heat_ctl_init(&c, 0);
    in = base_in(0); in.bat_c10 = 50; in.usb_c10 = 50; in.chg_c10 = 50;
    c.target_c10 = 200;
    CHECK(heat_ctl_request(&c, HEAT_KEEPWARM, 0, &in) == 0, "Keep warm starts");
    run(&c, &in, 0, 10000, 0);
    CHECK(c.cores == 7, "Keep warm: every core while the battery is below its target (%d)", c.cores);
    in.bat_c10 = 215;
    run(&c, &in, 10250, 10000, 0);
    CHECK(c.cores < 7 && c.state != HEAT_ST_STOPPED, "Keep warm: cores off above the target (%d)", c.cores);
    run(&c, &in, 20250, 10000, 0);
    CHECK(c.cores == 0 && c.state == HEAT_ST_HOLDING, "Keep warm: holding at the target (%d, %d)", c.cores, c.state);
    in.bat_c10 = 180;
    run(&c, &in, 30250, 3000, 0);
    CHECK(c.cores > 0 && c.state == HEAT_ST_RUNNING, "Keep warm: warming again below the target");
    in = base_in(0); in.bat_c10 = 500; refuse(in, HEAT_EXTREME, "the battery is 50 C for Extreme");
    /* Manual: the picked steps */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    c.man_a55 = 1248000; c.man_m3 = 1066000;
    CHECK(heat_ctl_request(&c, HEAT_MANUAL, 0, &in) == 0 && c.a55_khz == 1248000, "Manual: the A55 step picked");
    run(&c, &in, 0, 10000, 0);
    CHECK(c.cores == 7 && c.m3_khz == 1066000, "Manual: the M3 step picked (%u)", c.m3_khz);
    in.skin_c10 = 470;
    run(&c, &in, 10250, 5000, 0);
    CHECK(c.state == HEAT_ST_RUNNING && c.cores == 7, "Manual: no skin stop or regulation");

    /* the battery band: cores come off at 40 C, never all at once */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    heat_ctl_request(&c, HEAT_HIGH, 30, &in);
    run(&c, &in, 0, 20000, m_band);
    int before = c.cores;
    run(&c, &in, 20250, 1500, m_band);
    CHECK(c.cores == before - 1 || c.cores == before - 2, "the band takes one core per step (%d -> %d)", before, c.cores);
    run(&c, &in, 21750, 20000, m_band);
    CHECK(c.cores == 0 && c.state == HEAT_ST_HOLDING, "at the band: holding with no load (%d, %d)", c.cores, c.state);

    /* soft SoC limit and current sag step down too */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    heat_ctl_request(&c, HEAT_HIGH, 30, &in);
    run(&c, &in, 0, 30000, m_soft_soc);
    CHECK(c.cores < 7 && c.state != HEAT_ST_STOPPED, "75 C SoC: fewer cores, not a stop (%d)", c.cores);
    heat_ctl_init(&c, 0);
    in = base_in(0);
    heat_ctl_request(&c, HEAT_HIGH, 30, &in);
    run(&c, &in, 0, 30000, m_sag);
    CHECK(c.cores < 7 && c.state != HEAT_ST_STOPPED, "350 mV sag: fewer cores (%d)", c.cores);

    /* hard stops */
    stop_case(m_hot_battery, 30000, HEAT_ST_STOPPED, "Battery reached 42");
    stop_case(m_hot_soc, 20000, HEAT_ST_STOPPED, "Processor reached 85");
    stop_case(m_hot_skin, 20000, HEAT_ST_STOPPED, "back reached 46");
    stop_case(m_blind_bat, 14000, HEAT_ST_STOPPED, "Lost the battery");
    stop_case(m_blind_soc, 14000, HEAT_ST_STOPPED, "Lost the processor");
    stop_case(m_stuck, 70000, HEAT_ST_STOPPED, "stopped changing");
    stop_case(m_lag, 310000, HEAT_ST_STOPPED, "does not follow");
    stop_case(m_low_pct, 12000, HEAT_ST_STOPPED, "below 15");
    stop_case(m_low_mv, 12000, HEAT_ST_STOPPED, "voltage too low");
    stop_case(m_high_ma, 12000, HEAT_ST_STOPPED, "current too high");
    stop_case(m_bad, 12000, HEAT_ST_STOPPED, "wrong result");
    stop_case(m_fault, 12000, HEAT_ST_STOPPED, "fault");
    stop_case(0, 30 * 60000 + 1000, HEAT_ST_OFF, "Finished");

    /* blind for less than 3 s is not a stop */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    heat_ctl_request(&c, HEAT_LOW, 30, &in);
    run(&c, &in, 0, 5000, 0);
    in.bat_ok = 0;
    run(&c, &in, 5250, 2500, 0);
    CHECK(c.state == HEAT_ST_RUNNING || c.state == HEAT_ST_HOLDING, "2.5 s without a sample: still running");

    /* after a battery stop: a new start only below 36 C */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    heat_ctl_request(&c, HEAT_LOW, 30, &in);
    run(&c, &in, 0, 30000, m_hot_battery);
    in = base_in(40000);
    in.bat_c10 = 370;
    in.usb_c10 = 368;
    CHECK(heat_ctl_request(&c, HEAT_LOW, 30, &in) == SBOS_EAGAIN, "37 C after a battery stop: refused");
    in.bat_c10 = 350;
    in.usb_c10 = 352;
    CHECK(heat_ctl_request(&c, HEAT_LOW, 30, &in) == 0, "35 C after a battery stop: starts again");

    /* the charger pauses and the load resumes after */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    heat_ctl_request(&c, HEAT_MEDIUM, 30, &in);
    run(&c, &in, 0, 15000, m_charger);
    CHECK(c.state == HEAT_ST_PAUSED && c.cores == 0 && c.m3_khz == 0, "a charger pauses: no load (%d)", c.state);
    run(&c, &in, 15250, 15000, m_charger);
    CHECK(c.state == HEAT_ST_RUNNING && c.cores > 0, "unplugged: running again (%d, %d)", c.state, c.cores);
    heat_ctl_init(&c, 0);
    in = base_in(0);
    heat_ctl_request(&c, HEAT_LOW, 30, &in);
    run(&c, &in, 0, 15000, m_charger_unknown);
    CHECK(c.state == HEAT_ST_PAUSED && strstr(c.why, "cannot read"), "an unreadable charger pauses too");

    /* cold: A55 at 1248 MHz, no M3 until the A55s run at full clock */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    in.soc_c10 = 150;
    heat_ctl_request(&c, HEAT_HIGH, 30, &in);
    CHECK(c.cold && c.a55_khz == 1248000, "cold start at 1248 MHz");
    run(&c, &in, 0, 20000, m_cold);
    CHECK(c.cores <= 3 && c.m3_khz == 0, "cold: no M3 core yet (%d)", c.cores);
    run(&c, &in, 20250, 30000, m_cold);
    CHECK(!c.cold && c.a55_khz == 1794000, "cold over after 30 s without sag");
    run(&c, &in, 50250, 10000, m_cold);
    CHECK(c.cores > 3, "then the M3s join (%d)", c.cores);

    /* the stop check (PSCI 0.1 on this phone: AFFINITY_INFO unknown, answered in w0) */
    CHECK(heat_core_state(0xffffffffLL, 0, 0) == 1, "never started, PSCI not supported (w0 = -1): off");
    CHECK(heat_core_state(0xffffffffLL, 2, 0) == 1, "left the load, PSCI not supported: off");
    CHECK(heat_core_state(-1, 4, 0) == 1, "parked, PSCI -1: off");
    CHECK(heat_core_state(0xffffffffLL, 1, 0) == 0, "still running: on (a restart after 1.5 s)");
    CHECK(heat_core_state(1, 1, 0) == 0, "running although PSCI says off: trust the slot");
    CHECK(heat_core_state(1, 2, 0) == 1, "PSCI off: off");
    CHECK(heat_core_state(0, 0, 0) == 1, "never started, PSCI on (a firmware quirk): off");
    CHECK(heat_core_state(0, 4, 0) == 2, "parked while PSCI says on: logged, no restart");
    CHECK(heat_core_state(2, 2, 0) == 2, "CPU_OFF pending: logged, no restart");
    CHECK(heat_core_state(0xffffffffLL, 0, 1) == 2, "a core that never checked in: logged, no restart");
    CHECK(heat_core_state(0xffffffffLL, 1, 1) == 0, "running the heat kernel: a restart after 1.5 s");
    CHECK(heat_core_state(0x100000001LL, 2, 0) == 1, "only w0 counts");

    /* the minutes are capped */
    heat_ctl_init(&c, 0);
    in = base_in(0);
    heat_ctl_request(&c, HEAT_LOW, 600, &in);
    CHECK(c.t_end == 60ULL * 60000U, "at most 60 minutes");
}

/* ---- battery model --------------------------------------------------------------------------- */
static double sim_m3_w[3] = { 1.6, 2.9, 4.4 };
static double sim_lag;                      /* 1.1-beta7: the reading's lag behind the die, s (0: none) */
/* ---- 1.1 Survival heat: a simulated phone (SoC and battery heat, the gauge, the chip's trip at our 128 C) ---- */
struct sim {
    double soc, bat, pct, w, mah, rd;               /* rd: the TMU's reading, lagging the die (sim_lag s) */
    int max_soc, gpu_flips, cut, secs, min_cores_hot, ocp_bad;
    int held, hold_min, hold_max, hold_lim;     /* after the first reach of the hold limit (minus 1 C): its swing */
    double w_sum;
    int ticks;
};

/* the load's power: base 1.2 W, the A55s 0.6 W, the M3s 1.6 W, the GPU 2.5 W (the tours: ~10 W flat out); the SoC's
 * steady temperature 30 C + 9 C per W (so flat out would pass the trip) with an 8 s time constant; the cell 3.7 V
 * less 60 mV per A, 16 mAh per gauge percent (1.0-rc3's measurement) */
static void sim_tick(struct sim *s, const struct heat_ctl *c, struct heat_in *in, int trip_armed)
{
    int a55 = c->cores < 3 ? c->cores : 3, m3 = c->cores > 3 ? c->cores - 3 : 0;
    /* 1.1-beta6: an M3 core's power by its clock (V^2 f: about 1.6 / 2.9 / 4.4 W); sim_m3_w overrides it */
    double m3w = c->m3_khz >= 2704000U ? sim_m3_w[2] : c->m3_khz >= 2314000U ? sim_m3_w[1] : sim_m3_w[0];
    double w = 1.2 + 0.6 * a55 + m3w * m3 + (c->gpu_want && c->state == HEAT_ST_RUNNING ? 2.5 : 0.0);
    if (c->m3_khz > 1794000U && m3 > (c->m3_khz <= 2314000U ? 2 : 1))
        s->ocp_bad++;                       /* more M3 cores than Samsung's table allows at this clock */
    if (c->state != HEAT_ST_RUNNING && c->state != HEAT_ST_HOLDING)
        w = 0.4;
    double ss = 30.0 + 9.0 * w;
    s->soc += (ss - s->soc) * 0.25 / 8.0;
    s->bat += (w * 0.006 - (s->bat - 0.0) * 0.004) * 0.25;
    double amps = w / 3.7;
    s->mah += amps * 1000.0 * 0.25 / 3600.0;
    s->pct = 80.0 - s->mah / 16.0;
    s->w_sum += w;
    s->ticks++;
    s->rd = sim_lag > 0 ? s->rd + (s->soc - s->rd) * 0.25 / sim_lag : s->soc;
    in->soc_c10 = (int)(s->rd * 10.0);
    in->bat_c10 = (int)(s->bat * 10.0);
    in->pct = (int)s->pct;
    in->ma = -(int)(amps * 1000.0);
    in->mv = 3700 - (int)(amps * 60.0);
    in->trip_armed = trip_armed;
    if (in->soc_c10 > s->max_soc)
        s->max_soc = in->soc_c10;
    if (s->hold_lim && in->soc_c10 >= s->hold_lim - 10 && !s->held)
        s->held = 1, s->hold_min = s->hold_max = in->soc_c10;
    if (s->held && in->pct > 5) {
        if (in->soc_c10 < s->hold_min)
            s->hold_min = in->soc_c10;
        if (in->soc_c10 > s->hold_max)
            s->hold_max = in->soc_c10;
    }
    if (trip_armed && in->soc_c10 >= 1280)
        s->cut = 1;                         /* the chip's own trip would have cut the power here */
}

static void sim_run(struct heat_ctl *c, struct sim *s, int trip_armed, int max_s)
{
    struct heat_in in = base_in(0);
    in.bat_c10 = 100, in.usb_c10 = in.chg_c10 = 100;
    s->soc = s->rd = 38.0, s->bat = 10.0, s->pct = 80.0;
    int gpu = 1;
    uint64_t t = 0;
    for (; t < (uint64_t)max_s * 1000U && (c->state == HEAT_ST_RUNNING || c->state == HEAT_ST_HOLDING); t += 250) {
        in.now = t;
        in.bat_raw += 1;
        sim_tick(s, c, &in, trip_armed);
        if (s->cut)
            break;
        heat_ctl_step(c, &in);
        if (c->gpu_want != gpu)
            s->gpu_flips++, gpu = c->gpu_want;
    }
    s->secs = (int)(t / 1000U);
}

static void t_survival(void)
{
    /* 1.1-beta7: the ABSOLUTE limits: no hold, target or setting above 145 C SoC or 60 C battery */
    {
        struct heat_ctl c;
        struct heat_in in = base_in(0);
        heat_ctl_init(&c, 0);
        c.surv_stop_c10 = 1451;
        CHECK(heat_ctl_request(&c, HEAT_SURVIVAL_X, 0, &in) == SBOS_EAGAIN, "hard limit: a 145.1 C hold is refused");
        c.surv_stop_c10 = 1550;
        CHECK(heat_ctl_request(&c, HEAT_SURVIVAL_X, 0, &in) == SBOS_EAGAIN, "hard limit: 155 C (asked 2026-11-08) refused");
        heat_ctl_init(&c, 0);
        c.surv_stop_c10 = 1450;
        CHECK(heat_ctl_request(&c, HEAT_SURVIVAL_X, 0, &in) == 0, "hard limit: 145 C itself is accepted");
        c.surv_stop_c10 = 1750;                 /* corrupted after the start: the hold still never passes 145 */
        c.level = HEAT_SURVIVAL_X;
        CHECK(heat_surv_target(&c) == SBOS_HEAT_SOC_HARD_C10 && heat_surv_estop(&c) <= 1490,
              "hard limit: a corrupted limit holds at 145 C (target %d, stop %d)", heat_surv_target(&c), heat_surv_estop(&c));
        c.trip_off = 1;
        c.cal = 5;
        run(&c, &in, 0, 10000, 0);
        in.soc_c10 = 1455;
        int before = c.cores + c.gpu_want;
        run(&c, &in, 10250, 1000, 0);
        CHECK(c.cores + c.gpu_want < before, "hard limit: at 145.5 C it sheds even with a 175 C setting (%d -> %d)", before,
              c.cores + c.gpu_want);
        heat_ctl_init(&c, 0);
        c.surv_bat_c10 = 610;
        CHECK(heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in) == SBOS_EAGAIN, "hard limit: a 61 C battery limit is refused");
        CHECK(SBOS_HEAT_SOC_HARD_C10 == 1450 && SBOS_HEAT_BAT_HARD_C10 == 600, "hard limits: 145 C SoC, 60 C battery");
    }
    struct heat_ctl c;
    struct heat_in in = base_in(0);
    /* the start: the tables' tops, no reserve, refused at 2 % */
    heat_ctl_init(&c, 0);
    CHECK(heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in) == 0 && c.a55_khz == 1950000U && c.t_end == ~(uint64_t)0,
          "Survival starts: the A55 at its table top, no end time (%s)", c.why);
    run(&c, &in, 0, 10000, 0);
    CHECK(c.cores >= 5 && c.m3_khz == 1794000U, "Survival: the M3s at 1794 MHz (cores %d, %u)", c.cores, c.m3_khz);
    in = base_in(0);
    in.pct = 2;
    refuse(in, HEAT_SURVIVAL, "Survival at 2 %");
    in = base_in(0);
    in.pct = 10;                        /* below every reserve: Survival still starts */
    heat_ctl_init(&c, 0);
    CHECK(heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in) == 0, "Survival starts below the reserve (%s)", c.why);
    in = base_in(0);
    in.trip_armed = 0;
    refuse(in, HEAT_SURVIVAL_X, "the trip is not armed at the start (trip-off Survival too)");
    in = base_in(0);
    heat_ctl_init(&c, 0);
    c.surv_stop_c10 = 1500;
    CHECK(heat_ctl_request(&c, HEAT_SURVIVAL_X, 0, &in) == SBOS_EAGAIN, "trip-off Survival refuses a limit above 145 C");
    heat_ctl_init(&c, 0);
    CHECK(c.surv_stop_c10 == 1350 && heat_surv_estop(&c) == 1400, "trip off: the default limit 135 C, the stop 140 C");
    c.surv_stop_c10 = 1450;
    CHECK(heat_surv_estop(&c) == 1490, "trip off: the stop past 145 C is 149 C (150 does not read as valid)");

    /* the regulation, the whole battery: held below the trip, run to 2 % */
    struct sim s;
    memset(&s, 0, sizeof(s));
    in = base_in(0);
    in.bat_c10 = 100, in.usb_c10 = in.chg_c10 = 100;
    heat_ctl_init(&c, 0);
    CHECK(heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in) == 0, "the simulated Survival run starts (%s)", c.why);
    sim_run(&c, &s, 1, 3 * 3600);
    printf("survival sim: %d s, %.1f W average, peak SoC %.1f C, %d GPU switches, ended: %s\n", s.secs,
           s.w_sum / (s.ticks ? s.ticks : 1), s.max_soc / 10.0, s.gpu_flips, c.why);
    CHECK(!s.cut, "the chip's trip never acted (peak %d)", s.max_soc);
    CHECK(s.max_soc < 1260, "the SoC held below 126 C (peak %d)", s.max_soc);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "2 %"), "the run ended at 2 %% battery (%s)", c.why);
    CHECK(s.w_sum / s.ticks > 8.0, "most of the load ran (%.1f W average)", s.w_sum / s.ticks);
    CHECK(s.gpu_flips < 40, "the GPU is not switched on and off all the time (%d)", s.gpu_flips);
    CHECK(s.ocp_bad == 0, "never more M3 cores than Samsung's table allows at the clock (%d ticks)", s.ocp_bad);
    CHECK(c.cal_mw[0] > 0 && c.cal_mw[1] > 0 && c.cal_mw[2] > 0 && c.cal_mw[3] > 0,
          "every M3 candidate measured (%d %d %d %d mW)", c.cal_mw[0], c.cal_mw[1], c.cal_mw[2], c.cal_mw[3]);
    CHECK(c.m3_n == 4 && c.surv_m3_khz == 1794000U, "the pick: 4 @ 1794 has the most watts here (%d @ %u)", c.m3_n,
          c.surv_m3_khz);
    printf("survival M3 candidates: 4@1794 %d mW, 3@1794 %d, 2@2314 %d, 1@2704 %d; pick %d @ %u\n", c.cal_mw[0],
           c.cal_mw[1], c.cal_mw[2], c.cal_mw[3], c.m3_n, c.surv_m3_khz);
    /* a phone where two fast cores make more heat: the pick follows the measurement */
    sim_m3_w[1] = 3.6;
    memset(&s, 0, sizeof(s));
    in = base_in(0);
    in.bat_c10 = 100, in.usb_c10 = in.chg_c10 = 100;
    heat_ctl_init(&c, 0);
    heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in);
    sim_run(&c, &s, 1, 600);
    CHECK(c.m3_n == 2 && c.surv_m3_khz == 2314000U, "the pick follows the measurement: 2 @ 2314 (%d @ %u; %d %d %d %d mW)",
          c.m3_n, c.surv_m3_khz, c.cal_mw[0], c.cal_mw[1], c.cal_mw[2], c.cal_mw[3]);
    CHECK(s.ocp_bad == 0 && !s.cut, "and within Samsung's table, no trip (%d, peak %d)", s.ocp_bad, s.max_soc);
    sim_m3_w[1] = 2.9;

    /* trip off: holds below the user's stop minus 3 C, never reaches it in the model, runs to 2 % */
    memset(&s, 0, sizeof(s));
    in = base_in(0);
    heat_ctl_init(&c, 0);
    c.surv_stop_c10 = 1310;
    CHECK(heat_ctl_request(&c, HEAT_SURVIVAL_X, 0, &in) == 0, "trip-off Survival starts (%s)", c.why);
    c.trip_off = 1;                     /* heater.c after it cleared the trip */
    s.hold_lim = 1310;
    sim_run(&c, &s, 0, 3 * 3600);
    printf("survival sim (trip off): the hold swings %.1f .. %.1f C at a 131 C limit\n", s.hold_min / 10.0, s.hold_max / 10.0);
    printf("survival sim (trip off): %d s, %.1f W average, peak SoC %.1f C, ended: %s\n", s.secs,
           s.w_sum / (s.ticks ? s.ticks : 1), s.max_soc / 10.0, c.why);
    CHECK(s.max_soc >= 1300 && s.max_soc < 1335, "trip off: held at the 131 C limit (peak %d)", s.max_soc);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "2 %"), "trip off: the limit holds, heat to 2 %% (%s)", c.why);
    CHECK(s.secs > 900, "trip off: the whole battery (%d s)", s.secs);

    /* 1.1-beta7: the phone's hold swung 133 .. 142 C at a 145 C limit (beta6, 2@2314): with a 2 s lagging reading and
     * 2 fast M3 cores the hold must stay within 4 C below the limit and never pass it by more than 1.5 C */
    sim_lag = 2.0;
    sim_m3_w[1] = 3.6;
    memset(&s, 0, sizeof(s));
    in = base_in(0);
    heat_ctl_init(&c, 0);
    c.surv_stop_c10 = 1310;
    heat_ctl_request(&c, HEAT_SURVIVAL_X, 0, &in);
    c.trip_off = 1;
    s.hold_lim = 1310;
    sim_run(&c, &s, 0, 3 * 3600);
    printf("survival sim (trip off, lagging reading, 2@2314): the hold swings %.1f .. %.1f C at a 131 C limit, %d s, %s\n",
           s.hold_min / 10.0, s.hold_max / 10.0, s.secs, c.why);
    CHECK(s.hold_max <= 1325 && s.hold_min >= 1250, "the hold's swing stays within 125 .. 132.5 C (%d .. %d; beta6's hold: "
          "112.9 .. 131.5 in this model)", s.hold_min, s.hold_max);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "2 %"), "and heats to 2 %% (%s)", c.why);
    sim_lag = 0;
    sim_m3_w[1] = 2.9;

    /* the stops that stay */
    in = base_in(0);
    heat_ctl_init(&c, 0);
    c.surv_stop_c10 = 1310;
    heat_ctl_request(&c, HEAT_SURVIVAL_X, 0, &in);
    c.trip_off = 1;
    in.trip_armed = 0;
    run(&c, &in, 0, 5000, 0);
    CHECK(c.state == HEAT_ST_RUNNING, "trip off: the unarmed trip is expected, no stop (%s)", c.why);
    run(&c, &in, 5250, 18000, 0);         /* 1.1-beta6: past the M3 measurement (4 s + 4 x 4 s) */
    CHECK(c.cal == 5 && c.cores == 7, "trip off: all 7 workers below the limit after the measurement (%d, cal %d)", c.cores,
          c.cal);
    in.soc_c10 = 1312;
    run(&c, &in, 23500, 3000, 0);         /* 1.1-beta7: one load a second near the limit */
    CHECK(c.state != HEAT_ST_STOPPED, "trip off: at the limit it sheds, no stop (%s)", c.why);
    CHECK(c.gpu_want == 1 && c.cores == 3, "trip off: the M3s shed first, the GPU kept (%d cores, gpu %d)", c.cores, c.gpu_want);
    run(&c, &in, 27500, 0, 0);
    CHECK(c.gpu_want == 0 && c.cores == 3, "trip off: then the GPU (%d cores, gpu %d)", c.cores, c.gpu_want);
    in.soc_c10 = 1250;
    run(&c, &in, 28000, 4000, 0);
    CHECK(c.gpu_want == 1 && c.cores > 3, "trip off: back below the limit: the GPU, then the M3s (%d cores)", c.cores);
    in.soc_c10 = 1361;
    run(&c, &in, 32500, 300, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "past your limit"), "trip off: 5 C past the limit stops (%s)", c.why);
    in = base_in(0);
    heat_ctl_init(&c, 0);
    heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in);
    in.trip_armed = 0;
    run(&c, &in, 0, 1000, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "trip is not armed"), "Survival (trip on): the trip must stay armed (%s)", c.why);
    /* 1.1-beta5: the battery limit (55 default, 58, 60): held from 3 C below, a stop at it, both modes */
    for (int x = 0; x < 2; ++x)
        for (int lim = 550; lim <= 600; lim += 50) {
            in = base_in(0);
            heat_ctl_init(&c, 0);
            c.surv_bat_c10 = lim;
            CHECK(heat_ctl_request(&c, x ? HEAT_SURVIVAL_X : HEAT_SURVIVAL, 0, &in) == 0, "Survival %d starts (%s)", x, c.why);
            c.trip_off = x;
            run(&c, &in, 0, 9000, 0);
            int before = c.cores;
            in.bat_c10 = lim - 25;
            run(&c, &in, 9250, 1000, 0);
            CHECK(c.state != HEAT_ST_STOPPED && c.cores < before, "Survival %d: the battery at its limit - 2.5 C sheds, no stop "
                  "(limit %d, %d -> %d cores, %s)", x, lim, before, c.cores, c.why);
            in.bat_c10 = lim - 35;
            run(&c, &in, 10500, 3000, 0);
            CHECK(c.cores < before || !c.gpu_want, "Survival %d: no load back between limit - 4 and - 3 C (%d cores)", x, c.cores);
            in.bat_c10 = lim;
            run(&c, &in, 13750, 500, 0);
            CHECK(stopped_with(&c, HEAT_ST_STOPPED, "battery limit"), "Survival %d: the stop at the battery limit %d (%s)", x,
                  lim, c.why);
        }
    in = base_in(0);
    heat_ctl_init(&c, 0);
    c.surv_bat_c10 = 650;
    CHECK(heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in) == SBOS_EAGAIN, "Survival refuses a battery limit above 60 C");
    in = base_in(0);
    in.bat_c10 = 490;
    in.usb_c10 = in.chg_c10 = 490;
    heat_ctl_init(&c, 0);
    CHECK(heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in) == 0, "Survival starts at 49 C with a 55 C limit (%s)", c.why);
    in.bat_c10 = 530;
    in.usb_c10 = in.chg_c10 = 530;
    heat_ctl_init(&c, 0);
    CHECK(heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in) == SBOS_EAGAIN, "Survival refuses to start within 3 C of the limit");
    in = base_in(0);
    heat_ctl_init(&c, 0);
    heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in);
    run(&c, &in, 0, 3000, 0);
    in.gauge_ok = 0;
    run(&c, &in, 3000, 11000, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "charge and current"), "Survival: the gauge-lost stop (%s)", c.why);
    in = base_in(0);
    heat_ctl_init(&c, 0);
    heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in);
    run(&c, &in, 0, 10000, 0);
    int before = c.cores;
    in.ma = -3700;
    run(&c, &in, 10000, 1000, 0);
    CHECK(c.state == HEAT_ST_RUNNING && c.cores < before, "Survival: 3.7 A sheds load, no stop (%d -> %d, %s)", before,
          c.cores, c.why);
    in.ma = -4600;
    run(&c, &in, 11000, 500, 0);
    CHECK(stopped_with(&c, HEAT_ST_STOPPED, "4.5 A"), "Survival: above 4.5 A stops (%s)", c.why);
    in = base_in(0);
    heat_ctl_init(&c, 0);
    heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in);
    run(&c, &in, 0, 10000, 0);
    before = c.cores;
    in.mv = 3380;
    run(&c, &in, 10000, 1000, 0);
    CHECK(c.state == HEAT_ST_RUNNING && c.cores < before, "Survival: a low voltage sheds load, no stop (%d -> %d)",
          before, c.cores);
    in = base_in(0);
    heat_ctl_init(&c, 0);
    heat_ctl_request(&c, HEAT_SURVIVAL, 0, &in);
    run(&c, &in, 0, 2000, 0);
    heat_ctl_request(&c, HEAT_OFF, 0, &in);
    CHECK(stopped_with(&c, HEAT_ST_OFF, "Stopped"), "Survival: Off always stops (%s)", c.why);
}

static void t_battery(void)
{
    struct bat_model m;
    int learned;
    bat_model_init(&m);
    CHECK(bat_model_ma(&m, PS_ACTIVE, &learned) == 300 && !learned, "typical until learned");
    bat_model_feed(&m, PS_ACTIVE, -500, 10000);
    CHECK(m.samples[PS_ACTIVE] == 0, "a reading in the first 30 s of a state is ignored");
    bat_model_feed(&m, PS_ACTIVE, 800, 60000);
    CHECK(m.samples[PS_ACTIVE] == 0, "charging is ignored");
    for (int i = 0; i < 5; ++i)
        bat_model_feed(&m, PS_ACTIVE, -200 - 20 * i, 60000);
    CHECK(bat_model_ma(&m, PS_ACTIVE, &learned) == 240 && learned, "the mean of the first five: %d",
          bat_model_ma(&m, PS_ACTIVE, 0));
    for (int i = 0; i < 200; ++i)
        bat_model_feed(&m, PS_ACTIVE, -400, 60000);
    int v = bat_model_ma(&m, PS_ACTIVE, 0);
    CHECK(v > 380 && v <= 400, "then a moving average follows: %d", v);
    CHECK(bat_minutes(2000, 200) == 600, "2000 mAh at 200 mA: 10 h");
    CHECK(bat_minutes(3000, 0) == 60 * 24 * 60, "capped at 60 days");
    CHECK(bat_minutes(0, 100) == 0, "nothing left");
}

/* ---- state slot ------------------------------------------------------------------------------ */
static void t_state(void)
{
    static uint8_t a[STATE_HALF], b[STATE_HALF];
    struct state_buf s;
    state_begin(&s, a, 7);
    CHECK(state_add(&s, "light", "abc", 3) == 0, "add a record");
    CHECK(state_add(&s, "kernel", "0123456789", 10) == 0, "add another");
    static uint8_t big[STATE_REC_MAX + 1];
    CHECK(state_add(&s, "big", big, sizeof(big)) != 0, "a record over 4 KiB is refused");
    state_finish(&s);
    uint64_t seq = 0;
    CHECK(state_valid(a, &seq) && seq == 7, "valid, sequence 7");
    const uint8_t *d;
    CHECK(state_find(a, "kernel", &d) == 10 && !memcmp(d, "0123456789", 10), "find a record");
    CHECK(state_find(a, "nothere", &d) == -1, "a missing record");
    memcpy(b, a, STATE_HALF);
    b[30] ^= 1;
    CHECK(!state_valid(b, 0), "one bit flipped: invalid");
    CHECK(state_pick(a, b) == 0, "the valid half wins");
    state_begin(&s, b, 8);
    state_add(&s, "light", "xyz", 3);
    state_finish(&s);
    CHECK(state_pick(a, b) == 1, "the higher sequence wins");
    memset(b, 0, 64);
    memset(a, 0xff, 64);
    CHECK(state_pick(a, b) == -1, "neither valid: a fresh start");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "apps")) {
        int bad = 0;
        for (int i = 2; i < argc; ++i)
            bad += t_app_file(argv[i], 0);
        printf("sbos apptest: %d app files, %d refused\n", argc - 2, bad);
        return bad ? 1 : 0;
    }
    t_timers();
    t_heatk();
    t_dvfs();
    t_heater();
    t_survival();
    t_battery();
    t_state();
    for (int i = 1; i < argc; ++i)
        t_app_file(argv[i], 1);
    printf("sbos host test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
