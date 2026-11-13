/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite OS drivers track: the list of on-phone driver probes and their runner (drvprobe.h has the contract).
 *
 * The runner goes through every probe at level 0 (read-only), then every probe at level 1 (active, silent), then, in
 * the attended image only, level 2. Each probe gets its own time budget per level; one that overruns is aborted (its
 * abort puts everything back) and logged as FAIL. Pure C apart from sb_ms / sb_log, so the host test drives it with
 * fake probes. */
#include "sb.h"
#include "drvprobe.h"
#include "nled.h"
#include "bathealth.h"
#include "snd.h"
#include "codec_media.h"
#include "bt.h"
#include "cam.h"
#include "usb.h"
#include "shub_more.h"
#include "haptic.h"
#include "abox_media.h"
#include "sdprobe.h"

/* ---- the list: filled in as each driver is merged ---- */
#define DRV_LIST_BEGIN const struct drv_probe drv_probes[] = {
#define DRV_LIST_END { 0, 0, 0, 0 } };
#ifndef DRV_HOST_LIST
DRV_LIST_BEGIN
    { "bathealth", drv_bathealth_probe, drv_bathealth_abort, 2000 },
    { "codec", drv_codec_probe, drv_codec_abort, 14000 },
    { "nled", drv_nled_probe, drv_nled_abort, 12000 },
    { "sens", drv_sens_probe, drv_sens_abort, DRV_SENS_BUDGET_L1 },
    { "haptic", drv_haptic_probe, drv_haptic_abort, DRV_HAPTIC_BUDGET_L2 },
    { "bt", drv_bt_probe, drv_bt_abort, DRV_BT_BUDGET_MS },
    { "cam", drv_cam_probe, drv_cam_abort, CAM_PROBE_BUDGET_MS },
    { "usb", drv_usb_probe, drv_usb_abort, DRV_USB_BUDGET_MS },
    /* the microSD card's fast reads (sdprobe.c): read only, the store's first 8 MiB on both paths */
    { "sd", drv_sd_probe, drv_sd_abort, DRV_SD_BUDGET_MS },
    /* media through Calliope (abox_media.c), LAST so Calliope's boot and stream lines are in pmsg's tail (run 5: the
     * first Calliope boot on this phone; the bypass "audio" probe before it is known not to move, run 4) */
    { "calliope", drv_calliope_probe, drv_calliope_abort, DRV_CALLIOPE_BUDGET_MS },
    /* "audio" = snd's engine end to end through Calliope, LAST (run 7: its failure lines were cut from pmsg's tail
     * behind calliope's; calliope itself passed runs 6 and 7) */
    { "audio", drv_audio_probe, drv_audio_abort, 20000 },
DRV_LIST_END
#else
DRV_HOST_LIST                               /* the host test's fake probes */
#endif
const unsigned drv_nprobes = sizeof drv_probes / sizeof drv_probes[0] - 1U;

/* ---- the runner ---- */
static unsigned r_idx, r_level, r_maxlevel, r_timeouts;
static int r_started, r_done;
static const char *r_only;                  /* drv_run_select's list, 0 = all */
static uint64_t r_t0;

/* The result of every probe at every level, kept for the summary at the end (pmsg keeps only the run's last few
 * hundred lines): 'P' pass, 'F' fail, 'T' out of time, '?' no PASS/FAIL line found, 0 not run. */
#define DRV_MAXP 16
#define DRV_WHY 104
static char r_res[DRV_MAXP][3];
static char r_why[DRV_MAXP][3][DRV_WHY];

static const char *after(const char *h, const char *n)
{
    for (; *h; ++h) {
        unsigned k = 0;
        while (n[k] && h[k] == n[k])
            ++k;
        if (!n[k])
            return h + k;
    }
    return 0;
}

/* is name one of csv's comma-separated names? */
static int selected(const char *name)
{
    if (!r_only || !*r_only)
        return 1;
    for (const char *c = r_only; *c;) {
        unsigned k = 0;
        while (name[k] && c[k] == name[k])
            ++k;
        if (!name[k] && (c[k] == ',' || !c[k]))
            return 1;
        while (*c && *c != ',')
            ++c;
        if (*c == ',')
            ++c;
    }
    return 0;
}

void drv_run_select(const char *csv) { r_only = csv; }

/* the probe's own last line ("drvprobe: <name>: PASS ..." / "FAIL ..."), searched among the newest log lines */
static void note_result(unsigned idx, unsigned level, int timed_out)
{
    if (idx >= DRV_MAXP || level > 2)
        return;
    char *w = r_why[idx][level];
    w[0] = 0;
    if (timed_out) {
        r_res[idx][level] = 'T';
        return;
    }
    char pass[40], fail[40];
    struct ub u;
    ub_init(&u);
    ub_s(&u, "drvprobe: ");
    ub_s(&u, drv_probes[idx].name);
    ub_s(&u, ": PASS");
    unsigned i = 0;
    for (; u.b[i] && i < sizeof pass - 1; ++i)
        pass[i] = u.b[i];
    pass[i] = 0;
    for (i = 0; pass[i]; ++i)
        fail[i] = pass[i];
    fail[i] = 0;
    if (i >= 4)
        fail[i - 4] = 'F', fail[i - 3] = 'A', fail[i - 2] = 'I', fail[i - 1] = 'L';
    r_res[idx][level] = '?';
    unsigned n = sb_log_count();
    for (unsigned k = 0; k < 40U && k < n; ++k) {
        const char *l = sb_log_line(n - 1U - k);
        if (!l)
            break;
        const char *a = after(l, fail), *ap = a ? 0 : after(l, pass);
        if (a || ap) {
            r_res[idx][level] = a ? 'F' : 'P';
            if (!a)
                a = ap;                     /* the PASS line's facts too (the summary prints them last) */
            {
                while (*a == ' ')
                    ++a;
                unsigned j = 0;
                for (; a[j] && j < DRV_WHY - 1U; ++j)
                    w[j] = a[j];
                w[j] = 0;
            }
            return;
        }
    }
}

static void summary(void)
{
    for (unsigned lv = 0; lv <= r_maxlevel; ++lv) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "drvprobe: summary L");
        ub_dec(&u, lv);
        ub_s(&u, ":");
        for (unsigned i = 0; i < drv_nprobes && i < DRV_MAXP; ++i) {
            ub_s(&u, " ");
            ub_s(&u, drv_probes[i].name);
            ub_s(&u, "=");
            char c[2] = { r_res[i][lv] ? r_res[i][lv] : '-', 0 };
            ub_s(&u, c);
        }
        sb_log(u.b);
    }
    /* each passing probe's facts at the highest level it passed (the run's last lines: pmsg keeps only its tail) */
    for (unsigned i = 0; i < drv_nprobes && i < DRV_MAXP; ++i)
        for (unsigned lv = r_maxlevel + 1U; lv-- > 0;)
            if (r_res[i][lv] == 'P') {
                if (r_why[i][lv][0]) {
                    struct ub u;
                    ub_init(&u);
                    ub_s(&u, "drvprobe: summary: ");
                    ub_s(&u, drv_probes[i].name);
                    ub_s(&u, " L");
                    ub_dec(&u, lv);
                    ub_s(&u, " ok: ");
                    ub_s(&u, r_why[i][lv]);
                    sb_log(u.b);
                }
                break;
            }
    for (unsigned lv = 0; lv <= r_maxlevel; ++lv)
        for (unsigned i = 0; i < drv_nprobes && i < DRV_MAXP; ++i)
            if (r_res[i][lv] == 'F' || r_res[i][lv] == 'T') {
                struct ub u;
                ub_init(&u);
                ub_s(&u, "drvprobe: summary: ");
                ub_s(&u, drv_probes[i].name);
                ub_s(&u, " L");
                ub_dec(&u, lv);
                ub_s(&u, r_res[i][lv] == 'T' ? ": out of time" : ": ");
                ub_s(&u, r_why[i][lv]);
                sb_log(u.b);
            }
}

void drv_run_begin(int attended)
{
    r_idx = 0;
    r_level = 0;
    r_maxlevel = attended ? DRV_LVL_ATTENDED : DRV_LVL_ACTIVE;
    r_timeouts = 0;
    r_started = 0;
    r_done = 0;
    for (unsigned i = 0; i < DRV_MAXP; ++i)
        r_res[i][0] = r_res[i][1] = r_res[i][2] = 0;
    sb_logx("drvprobe: begin; probes ", drv_nprobes);
    sb_logx("drvprobe: highest level ", r_maxlevel);
    if (r_only && *r_only)
        sb_logf("drvprobe: only: ", r_only);
#ifndef DRV_HOST_LIST
    /* the probe images may force UART_BT's / USI10_I2C's CMU gates on for Bluetooth; bt.c restores them */
    bt_allow_clock_gates(1);
#endif
}

/* One slice. Returns 0: call again after *wait_ms; 1: all done. */
int drv_run_step(unsigned *wait_ms)
{
    *wait_ms = 0;
    if (r_done)
        return 1;
    if (r_idx >= drv_nprobes) {
        if (r_level >= r_maxlevel) {
            sb_logx("drvprobe: done; probes that ran out of time: ", r_timeouts);
            summary();
            r_done = 1;
            return 1;
        }
        ++r_level;
        r_idx = 0;
        sb_logx("drvprobe: level ", r_level);
    }
    const struct drv_probe *p = &drv_probes[r_idx];
    if (!r_started && !selected(p->name)) {
        ++r_idx;                                /* not selected: shows '-' in the summary */
        return 0;
    }
    if (!r_started) {
        r_started = 1;
        r_t0 = sb_ms();
        sb_logf("drvprobe: start: ", p->name);
    }
    unsigned w = 0;
    int fin = p->probe((int)r_level, &w);
    if (!fin && sb_ms() - r_t0 > p->budget_ms) {
        if (p->abort)
            p->abort();
        sb_logf("drvprobe: FAIL: out of time (aborted, everything put back): ", p->name);
        ++r_timeouts;
        note_result(r_idx, r_level, 1);
        fin = 2;
    }
    if (fin) {
        if (fin == 1)
            note_result(r_idx, r_level, 0);
        r_started = 0;
        ++r_idx;
        *wait_ms = 50;
        return 0;
    }
    *wait_ms = w > 1000U ? 1000U : w;
    return 0;
}

/* The watchdog / stall path: whatever probe is running is put back. */
void drv_run_abort(void)
{
    if (r_started && r_idx < drv_nprobes && drv_probes[r_idx].abort)
        drv_probes[r_idx].abort();
    r_started = 0;
    r_done = 1;
}
