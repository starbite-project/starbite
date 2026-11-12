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

/* ---- the list: filled in as each driver is merged ---- */
#define DRV_LIST_BEGIN const struct drv_probe drv_probes[] = {
#define DRV_LIST_END { 0, 0, 0, 0 } };
#ifndef DRV_HOST_LIST
DRV_LIST_BEGIN
    { "bathealth", drv_bathealth_probe, drv_bathealth_abort, 2000 },
    { "codec", drv_codec_probe, drv_codec_abort, 14000 },
    { "audio", drv_audio_probe, drv_audio_abort, 10000 },
    { "nled", drv_nled_probe, drv_nled_abort, 12000 },
    { "bt", drv_bt_probe, drv_bt_abort, DRV_BT_BUDGET_MS },
    { "cam", drv_cam_probe, drv_cam_abort, CAM_PROBE_BUDGET_MS },
DRV_LIST_END
#else
DRV_HOST_LIST                               /* the host test's fake probes */
#endif
const unsigned drv_nprobes = sizeof drv_probes / sizeof drv_probes[0] - 1U;

/* ---- the runner ---- */
static unsigned r_idx, r_level, r_maxlevel, r_timeouts;
static int r_started, r_done;
static uint64_t r_t0;

void drv_run_begin(int attended)
{
    r_idx = 0;
    r_level = 0;
    r_maxlevel = attended ? DRV_LVL_ATTENDED : DRV_LVL_ACTIVE;
    r_timeouts = 0;
    r_started = 0;
    r_done = 0;
    sb_logx("drvprobe: begin; probes ", drv_nprobes);
    sb_logx("drvprobe: highest level ", r_maxlevel);
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
            r_done = 1;
            return 1;
        }
        ++r_level;
        r_idx = 0;
        sb_logx("drvprobe: level ", r_level);
    }
    const struct drv_probe *p = &drv_probes[r_idx];
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
        fin = 1;
    }
    if (fin) {
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
