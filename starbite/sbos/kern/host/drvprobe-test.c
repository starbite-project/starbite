/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/drvprobe.c's runner with fake probes: order of levels, budgets, aborts. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "sb.h"
#include "drvprobe.h"

static uint64_t now_ms;
uint64_t sb_ms(void) { return now_ms; }
static char logbuf[32768];
static char lines[512][256];
static unsigned nlines;
unsigned sb_log_count(void) { return nlines; }
const char *sb_log_line(unsigned i) { return i < nlines ? lines[i] : 0; }
void sb_log(const char *s)
{
    strncat(logbuf, s, sizeof logbuf - strlen(logbuf) - 2);
    strcat(logbuf, "\n");
    if (nlines < 512)
        snprintf(lines[nlines++], 256, "eng: %s", s);
}
void ub_init(struct ub *u) { u->n = 0; u->b[0] = 0; }
void ub_s(struct ub *u, const char *s) { while (*s && u->n < sizeof u->b - 1) u->b[u->n++] = *s++; u->b[u->n] = 0; }
void ub_dec(struct ub *u, uint64_t v) { char t[24]; snprintf(t, sizeof t, "%llu", (unsigned long long)v); ub_s(u, t); }
void sb_logf(const char *a, const char *b) { char t[256]; snprintf(t, sizeof t, "%s%s", a, b); sb_log(t); }
void sb_logx(const char *a, uint64_t v) { char t[256]; snprintf(t, sizeof t, "%s%llx", a, (unsigned long long)v); sb_log(t); }

static int calls_a[3], calls_b[3], aborts_b, order[64], norder;
static int pa(int level, unsigned *w)
{
    order[norder++] = 10 + level; ++calls_a[level]; *w = 5;
    if (calls_a[level] >= 2) { sb_log("drvprobe: a: PASS fine"); return 1; }
    return 0;
}
static int pb(int level, unsigned *w)
{
    order[norder++] = 20 + level; ++calls_b[level]; *w = 2000; now_ms += 400;
    if (level == 0) { sb_log("drvprobe: b: FAIL the widget did not move"); return 1; }
    return 0;
}
static void ab(void) { ++aborts_b; }
#define DRV_HOST_LIST const struct drv_probe drv_probes[] = { { "a", pa, 0, 1000 }, { "b", pb, ab, 1000 }, { 0, 0, 0, 0 } };
#include "../drvprobe.c"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); ++fails; } } while (0)

static int run(int attended)
{
    unsigned w;
    int n = 0;
    drv_run_begin(attended);
    while (!drv_run_step(&w) && n < 1000) {
        CHECK(w <= 1000, "wait capped at 1 s");
        now_ms += w;
        ++n;
    }
    return n;
}

int main(void)
{
    run(0);
    CHECK(drv_nprobes == 2, "two probes");
    CHECK(calls_a[0] == 2 && calls_a[1] == 2 && calls_a[2] == 0, "a ran at levels 0 and 1 only");
    CHECK(calls_b[0] == 1 && calls_b[2] == 0, "b level 0 once");
    CHECK(aborts_b == 1, "b level 1 aborted after its budget");
    CHECK(strstr(logbuf, "drvprobe: FAIL: out of time (aborted, everything put back): b") != 0, "timeout logged");
    CHECK(order[0] == 10 && order[2] == 20 && order[3] == 11, "level 0 for all before level 1");
    CHECK(strstr(logbuf, "drvprobe: summary L0: a=P b=F") != 0, "summary L0");
    CHECK(strstr(logbuf, "drvprobe: summary L1: a=P b=T") != 0, "summary L1");
    CHECK(strstr(logbuf, "drvprobe: summary: b L0: the widget did not move") != 0, "summary FAIL reason");
    CHECK(strstr(logbuf, "drvprobe: summary: b L1: out of time") != 0, "summary timeout");
    CHECK(strstr(logbuf, "drvprobe: summary: a L1 ok: fine") != 0, "summary: a passing probe's facts at its highest level");
    CHECK(strstr(logbuf, "drvprobe: summary: a L0 ok") == 0, "summary: only the highest passing level");
    memset(calls_a, 0, sizeof calls_a); memset(calls_b, 0, sizeof calls_b); aborts_b = 0; norder = 0;
    logbuf[0] = 0;
    run(1);
    CHECK(calls_a[2] == 2, "attended: a at level 2");
    CHECK(strstr(logbuf, "drvprobe: summary L2: a=P b=T") != 0, "summary L2");
    CHECK(aborts_b == 2, "attended: b aborted at 1 and 2");
    unsigned w;
    CHECK(drv_run_step(&w) == 1, "done stays done");
    drv_run_begin(0);
    drv_run_step(&w);                        /* a started */
    drv_run_abort();
    CHECK(drv_run_step(&w) == 1, "abort ends the run");
    /* drv_run_select: only b runs; a shows '-' */
    memset(calls_a, 0, sizeof calls_a); memset(calls_b, 0, sizeof calls_b); aborts_b = 0; norder = 0;
    logbuf[0] = 0;
    drv_run_select("x,b");
    run(0);
    CHECK(calls_a[0] == 0 && calls_a[1] == 0, "select: a not run");
    CHECK(calls_b[0] == 1, "select: b run");
    CHECK(strstr(logbuf, "drvprobe: summary L0: a=- b=F") != 0, "select: summary shows a as -");
    CHECK(strstr(logbuf, "drvprobe: only: x,b") != 0, "select: logged");
    drv_run_select("bb,a");                  /* a prefix or a longer name is not a match */
    memset(calls_a, 0, sizeof calls_a); memset(calls_b, 0, sizeof calls_b);
    run(0);
    CHECK(calls_a[0] == 2 && calls_b[0] == 0, "select: exact names only");
    drv_run_select(0);
    printf("drvprobe host test: %s (%d failed)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
