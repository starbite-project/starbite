/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of starbite/sbos/kern/bathealth.c (battery health from the MAX77705 fuel gauge, read only) against a fake
 * gauge. Build and run: starbite/sbos/kern/host/bathealth-test.sh [-v] (the script also checks, with nm, that
 * bathealth.c references no writer at all).
 *
 * The register values: the phone's DesignCap as Samsung's model load programs it (DT fuelgauge,capacity 0xB97 =
 * 2967 mAh) with FullCapNom / FullCapRep / Cycles sets for a new, a typical two-year and a worn cell; then the edge cases
 * (a gauge after power-on reset, no battery, an implausible DesignCap, FullCapNom not learned, words not answered). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "bathealth.h"

static int fails, checks, verbose;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(c)) {                                                                                                    \
            ++fails;                                                                                                   \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- the fake libsb: the gauge's words, reads only ---- */
static char lastlog[300];
void sb_log(const char *s)
{
    snprintf(lastlog, sizeof lastlog, "%s", s);
    if (verbose)
        printf("    | %s\n", s);
}
void sb_logf(const char *a, const char *b)
{
    char t[600];
    snprintf(t, sizeof t, "%s%s", a, b);
    sb_log(t);
}
struct sb_i2c sb_max_bus;
const char *sb_max_why;
static int bus_ok = 1;
static uint16_t fg[256];
static uint8_t fg_dead[256];
static unsigned reads;
int sb_max_init(void)
{
    sb_max_why = bus_ok ? 0 : "no answer on HSI2C 0x14360000";
    return bus_ok;
}
int sb_fg_word_checked(uint8_t reg, uint16_t *v, unsigned lo, unsigned hi)
{
    (void)lo, (void)hi;
    ++reads;
    if (!bus_ok || fg_dead[reg])
        return 0;
    *v = fg[reg];
    return 1;
}

static void gauge(uint16_t status, uint16_t design, uint16_t fullnom, uint16_t fullrep, uint16_t cycles)
{
    memset(fg, 0, sizeof fg);
    memset(fg_dead, 0, sizeof fg_dead);
    bus_ok = 1;
    fg[0x00] = status, fg[0x18] = design, fg[0x23] = fullnom, fg[0x35] = fullrep, fg[0x17] = cycles;
}

static void t_maths(void)
{
    struct bathealth h;
    /* a two-year-old cell: 2581 / 2967 = 86.99 % -> 87 %, 412.35 cycles */
    gauge(0x0000, 0x0B97, 2581, 2603, 41235);
    CHECK(bathealth_read(&h) && h.ok, "read");
    CHECK(h.design_mah == 2967 && !h.design_from_dt, "design 2967 (%u)", h.design_mah);
    CHECK(h.max_pct == 87 && h.shown_pct == 87, "87 %% (%d)", h.max_pct);
    CHECK(h.rep_pct == 88, "reported 2603 / 2967 = 88 %% (%d)", h.rep_pct);
    CHECK(h.cycles == 412 && h.cycles_x100 == 41235, "412 cycles (%u)", h.cycles);
    CHECK(!strcmp(h.words, "Your battery is in good health."), "words: %s", h.words);
    /* a new cell above design: shown 100 */
    gauge(0x0000, 0x0B97, 3050, 3010, 312);
    bathealth_read(&h);
    CHECK(h.max_pct == 103 && h.shown_pct == 100 && h.cycles == 3, "new: 103 %% shown 100 (%d %d)", h.max_pct,
          h.shown_pct);
    /* rounding: 2373 / 2967 = 79.98 -> 80 (good); 2372 -> 79.94 -> 80; 2358 -> 79.47 -> 79 */
    gauge(0, 2967, 2373, 0, 0);
    bathealth_read(&h);
    CHECK(h.max_pct == 80 && strstr(h.words, "good"), "80 %% is good (%d)", h.max_pct);
    gauge(0, 2967, 2358, 0, 0);
    bathealth_read(&h);
    CHECK(h.max_pct == 79 && strstr(h.words, "noticeably less"), "79 %% (%d): %s", h.max_pct, h.words);
    CHECK(h.rep_pct == -1, "FullCapRep 0: not shown");
    /* worn */
    gauge(0, 2967, 1600, 1580, 65000);
    bathealth_read(&h);
    CHECK(h.max_pct == 54 && strstr(h.words, "new battery is recommended") && h.cycles == 650, "worn 54 %% (%d)",
          h.max_pct);
    /* FullCapNom not plausible (0, or above 1.5 x design): FullCapRep instead */
    gauge(0, 2967, 0, 2700, 100);
    bathealth_read(&h);
    CHECK(h.max_pct == 91, "FullCapNom 0: FullCapRep's 91 %% (%d)", h.max_pct);
    gauge(0, 2967, 0xFFFF, 2700, 100);
    bathealth_read(&h);
    CHECK(h.max_pct == 91, "FullCapNom 0xFFFF: FullCapRep's 91 %% (%d)", h.max_pct);
    gauge(0, 2967, 0, 0, 100);
    bathealth_read(&h);
    CHECK(h.max_pct == -1 && h.shown_pct == -1 && strstr(h.words, "not known"), "neither: unknown: %s", h.words);
    /* DesignCap implausible: the DT's 2967 */
    gauge(0, 0, 2581, 2603, 0);
    bathealth_read(&h);
    CHECK(h.design_from_dt && h.design_mah == 2967 && h.max_pct == 87, "design 0: the DT's");
    gauge(0, 0xFFFF, 2581, 2603, 0);
    bathealth_read(&h);
    CHECK(h.design_from_dt && h.max_pct == 87, "design 0xFFFF: the DT's");
    /* STATUS: POR (the gauge restarted) and Bst (no battery) */
    gauge(0x0002, 2967, 2967, 2967, 0);
    bathealth_read(&h);
    CHECK(h.gauge_restarted && strstr(h.words, "restarted") && h.max_pct == 100, "POR: %s", h.words);
    gauge(0x0008, 2967, 2581, 2603, 0);
    bathealth_read(&h);
    CHECK(h.no_battery && !strcmp(h.words, "No battery found."), "Bst: %s", h.words);
    /* the pure maths directly, status not read */
    bathealth_compute(&h, -1, 3000, 2400, 0, 0);
    CHECK(h.max_pct == 80 && !h.gauge_restarted && !h.no_battery, "compute: 80 %%");
}

static void t_errors(void)
{
    struct bathealth h;
    gauge(0, 2967, 2581, 2603, 41235);
    bus_ok = 0;
    CHECK(!bathealth_read(&h) && !h.ok && strstr(bathealth_why(), "HSI2C"), "no bus: %s", bathealth_why());
    const uint8_t need[] = { 0x18, 0x23, 0x17 };
    for (unsigned i = 0; i < 3; ++i) {
        gauge(0, 2967, 2581, 2603, 41235);
        fg_dead[need[i]] = 1;
        CHECK(!bathealth_read(&h) && !h.ok && strstr(bathealth_why(), "fuel gauge did not answer"), "word %02x dead",
              need[i]);
    }
    gauge(0, 2967, 2581, 2603, 41235);
    fg_dead[0x35] = 1;
    fg_dead[0x00] = 1;
    CHECK(bathealth_read(&h) && h.max_pct == 87 && h.rep_pct == -1 && !h.gauge_restarted, "FullCapRep, STATUS dead: ok");
}

static void t_probe(void)
{
    unsigned w = 99;
    gauge(0, 0x0B97, 2581, 2603, 41235);
    CHECK(drv_bathealth_probe(0, &w) == 1 && w == 0, "level 0: one call");
    CHECK(!strcmp(lastlog, "drvprobe: bathealth: PASS maximum capacity 87 %, 412 cycles"), "level 0: %s", lastlog);
    CHECK(drv_bathealth_probe(1, &w) == 1 && strstr(lastlog, "PASS maximum capacity 87 %"), "level 1: %s", lastlog);
    CHECK(drv_bathealth_probe(2, &w) == 1 && strstr(lastlog, "PASS nothing at level 2"), "level 2: %s", lastlog);
    bus_ok = 0;
    CHECK(drv_bathealth_probe(0, &w) == 1 && strstr(lastlog, "drvprobe: bathealth: FAIL "), "no bus: %s", lastlog);
    gauge(0x0008, 2967, 2581, 2603, 0);
    CHECK(drv_bathealth_probe(0, &w) == 1 && strstr(lastlog, "FAIL No battery found."), "no battery: %s", lastlog);
    gauge(0, 2967, 0, 0, 0);
    CHECK(drv_bathealth_probe(0, &w) == 1 && strstr(lastlog, "FAIL The battery's capacity is not known"), "unknown: %s",
          lastlog);
    drv_bathealth_abort();
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    t_maths();
    t_errors();
    t_probe();
    printf("bathealth-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
