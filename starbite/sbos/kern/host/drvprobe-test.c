/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of kern/drvprobe.c's runner with fake probes: order of levels, budgets, aborts. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint64_t now_ms;
uint64_t sb_ms(void) { return now_ms; }
static char logbuf[16384];
void sb_log(const char *s) { strncat(logbuf, s, sizeof logbuf - strlen(logbuf) - 2); strcat(logbuf, "\n"); }
void sb_logf(const char *a, const char *b) { char t[256]; snprintf(t, sizeof t, "%s%s", a, b); sb_log(t); }
void sb_logx(const char *a, uint64_t v) { char t[256]; snprintf(t, sizeof t, "%s%llx", a, (unsigned long long)v); sb_log(t); }

static int calls_a[3], calls_b[3], aborts_b, order[64], norder;
static int pa(int level, unsigned *w) { order[norder++] = 10 + level; ++calls_a[level]; *w = 5; return calls_a[level] >= 2; }
static int pb(int level, unsigned *w) { order[norder++] = 20 + level; ++calls_b[level]; *w = 2000; now_ms += 400; return level == 0; }
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
    memset(calls_a, 0, sizeof calls_a); memset(calls_b, 0, sizeof calls_b); aborts_b = 0; norder = 0;
    logbuf[0] = 0;
    run(1);
    CHECK(calls_a[2] == 2, "attended: a at level 2");
    CHECK(aborts_b == 2, "attended: b aborted at 1 and 2");
    unsigned w;
    CHECK(drv_run_step(&w) == 1, "done stays done");
    drv_run_begin(0);
    drv_run_step(&w);                        /* a started */
    drv_run_abort();
    CHECK(drv_run_step(&w) == 1, "abort ends the run");
    printf("drvprobe host test: %s (%d failed)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
