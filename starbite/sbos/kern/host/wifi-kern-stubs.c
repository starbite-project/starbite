/* SPDX-License-Identifier: GPL-2.0-only
 * The kernel around kern/wifi.c for its host harness (wifi-kern-test.c): a fake clock (sb_ms follows the chip
 * model's own clock, which udelay advances), a small timer table the test runs, the log kept in memory, and the
 * rest of what wifi.c calls as plain stubs whose results the test can set (wk_* below). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sb.h"
#include "kern.h"
#include "sbapp.h"
#include "host/model.h"
#include "wifi-kern-stubs.h"

struct kstate K;
const int sbos_selftest_enabled = 0;
int selftest_warnings;

uint64_t wk_now_ms = 1000;                 /* the kernel's clock, ms */
int wk_verbose, wk_heater_busy, wk_spi_ok;
unsigned wk_settings_changed, wk_notified, wk_wdt_feeds;
int64_t wk_clock_utc;                       /* 0: no clock */
int wk_clock_approx;
int64_t wk_net_utc;                         /* what NTP set (clock_set_net) */

uint64_t sb_ms(void)
{
    uint64_t m = M.now / 1000U;             /* the model's udelay is time spent by the kernel too */
    if (m > wk_now_ms)
        wk_now_ms = m;
    return wk_now_ms;
}

uint64_t sb_ticks(void)
{
    static uint64_t c;
    return sb_ms() * 26000U + (++c & 0x3fffU);
}

void *sb_alloc(uint64_t size)
{
    void *p = calloc(1, (size_t)size);
    return p;
}

/* ---- the log ---- */
#define WK_LOG_MAX 200000
static char *lines[WK_LOG_MAX];
static unsigned nlines;

void sb_log(const char *s)
{
    if (wk_verbose)
        printf("  [%8llu] %s\n", (unsigned long long)wk_now_ms, s);
    if (nlines < WK_LOG_MAX)
        lines[nlines++] = strdup(s);
}

void sb_logf(const char *a, const char *b)
{
    char t[1024];
    snprintf(t, sizeof(t), "%s%s", a, b);
    sb_log(t);
}

void sb_logx(const char *a, uint64_t v)
{
    char t[512];
    snprintf(t, sizeof(t), v >> 32 ? "%s0x%016llx" : "%s0x%08llx", a, (unsigned long long)v);
    sb_log(t);
}

unsigned wk_log_n(void) { return nlines; }
const char *wk_log_line(unsigned i) { return i < nlines ? lines[i] : ""; }

/* ---- timers (kt_*): a flat table ---- */
static struct { int used; unsigned owner, id; uint64_t at; uint32_t every; } kt[KT_MAX];

void kt_reset(void) { memset(kt, 0, sizeof(kt)); }

int kt_set(unsigned owner, unsigned id, uint64_t now, uint32_t ms, int repeat)
{
    int f = -1;
    for (int i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].owner == owner && kt[i].id == id) {
            f = i;
            break;
        }
    for (int i = 0; f < 0 && i < KT_MAX; ++i)
        if (!kt[i].used)
            f = i;
    if (f < 0)
        return -1;
    kt[f].used = 1, kt[f].owner = owner, kt[f].id = id, kt[f].at = now + ms, kt[f].every = repeat ? ms : 0;
    return 0;
}

void kt_cancel(unsigned owner, unsigned id)
{
    for (int i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].owner == owner && kt[i].id == id)
            kt[i].used = 0;
}

void kt_cancel_owner(unsigned owner)
{
    for (int i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].owner == owner)
            kt[i].used = 0;
}

int kt_pending(unsigned owner, unsigned id)
{
    for (int i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].owner == owner && kt[i].id == id)
            return 1;
    return 0;
}

uint64_t kt_next(void)
{
    uint64_t t = UINT64_MAX;
    for (int i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].at < t)
            t = kt[i].at;
    return t;
}

int kt_pop(uint64_t now, unsigned *owner, unsigned *id)
{
    int b = -1;
    for (int i = 0; i < KT_MAX; ++i)
        if (kt[i].used && kt[i].at <= now && (b < 0 || kt[i].at < kt[b].at))
            b = i;
    if (b < 0)
        return 0;
    *owner = kt[b].owner, *id = kt[b].id;
    if (kt[b].every)
        kt[b].at += kt[b].every;
    else
        kt[b].used = 0;
    return 1;
}

/* ---- the rest of the kernel ---- */
void ui_mark_dirty(void) { wk_notified++; }
int app_by_id(const char *id) { (void)id; return -1; }
void app_event_send(int a, uint32_t type, uint32_t id, int32_t value) { (void)a, (void)type, (void)id, (void)value; }
void hw_wdt_feed(void) { wk_wdt_feeds++; }
int hw_spi(unsigned intid, int on) { (void)intid; return on ? wk_spi_ok : 0; }
int heater_busy(void) { return wk_heater_busy; }
void settings_changed(void) { wk_settings_changed++; }
int kmodem_netif(struct sbnet **net, struct sbtcp **tcp) { (void)net, (void)tcp; return 0; }   /* mobile data off */

int clock_now(struct sbos_time *t)
{
    memset(t, 0, sizeof(*t));
    if (!wk_clock_utc)
        return 0;
    t->utc = wk_clock_utc + (int64_t)(wk_now_ms / 1000U);
    t->approx = wk_clock_approx;
    return 1;
}

int64_t clock_set_net(int64_t utc, int *was_set)
{
    int64_t now = wk_clock_utc ? wk_clock_utc + (int64_t)(wk_now_ms / 1000U) : 0;
    wk_net_utc = utc;
    *was_set = !wk_clock_utc || wk_clock_approx;
    int64_t d = wk_clock_utc ? utc - now : 0;
    wk_clock_utc = utc - (int64_t)(wk_now_ms / 1000U);
    wk_clock_approx = 0;
    return *was_set ? 0 : d;
}

/* store.h: only the phone's load_saved reads the store (SB_HOST takes SBOS_HOST_WIFINET) */
