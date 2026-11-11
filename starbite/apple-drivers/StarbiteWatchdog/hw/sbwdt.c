/* SPDX-License-Identifier: APSL-2.0 */
/* Cluster 1 watchdog maths and the dead-man contract (see sbwdt.h). */
#include "sbwdt.h"

uint32_t
sbwdt_hw_ms(uint32_t wtcon, uint32_t wtdat)
{
	uint64_t pre = ((wtcon >> 8) & 0xffu) + 1u;
	uint64_t div = 16u << ((wtcon >> 3) & 3u);
	if (!(wtcon & SBWDT_WTCON_ENABLE) || !(wtcon & SBWDT_WTCON_RSTEN) || (wtdat & 0xffffu) == 0) {
		return 0;
	}
	return (uint32_t)((uint64_t)(wtdat & 0xffffu) * pre * div * 1000u / SBWDT_CLK_HZ);
}

uint32_t
sbwdt_period_ms(uint32_t secs, uint32_t hw_ms)
{
	uint64_t t = (uint64_t)secs * 1000u;
	if (secs == 0 || hw_ms == 0) {
		return 0;
	}
	if (hw_ms < t) {
		t = hw_ms;
	}
	t /= 3u;
	if (t < SBWDT_PERIOD_MIN_MS) {
		t = SBWDT_PERIOD_MIN_MS;
	}
	if (t > SBWDT_PERIOD_MAX_MS) {
		t = SBWDT_PERIOD_MAX_MS;
	}
	return (uint32_t)t;
}

const char *
sbwdt_stop_name(int stopped)
{
	switch (stopped) {
	case SBWDT_STOP_NONE: return "feeding";
	case SBWDT_STOP_PANIC: return "stopped: panic";
	case SBWDT_STOP_DEADMAN: return "stopped: sweetreboot + 60 s dead-man (0421)";
	case SBWDT_STOP_NOUI: return "stopped: no frame on the panel by sbnoui (0421)";
	case SBWDT_STOP_USERSPACE: return "stopped: watchdogd missed its check-ins";
	default: return "?";
	}
}

void
sbwdt_plan(const struct sbwdt_cfg *cfg, struct sbwdt *w)
{
	struct sbwdt z = { 0 };
	*w = z;
	w->hw_ms = sbwdt_hw_ms(cfg->wtcon, cfg->wtdat);
	w->reload = cfg->wtdat & 0xffffu;
	if (cfg->test_boot) {
		w->why = "sbstop= test boot (0423): XNU never feeds; the loader's watchdog resets the phone";
		return;
	}
	if (cfg->sbwdt_found && cfg->sbwdt_s == 0) {
		w->why = "sbwdt=0: never fed";
		return;
	}
	if (w->hw_ms == 0) {
		w->why = "the watchdog is not armed (left off; never armed here)";
		return;
	}
	if (cfg->sbwdt_found) {
		w->feed_s = cfg->sbwdt_s;
	} else if ((cfg->pmu_dis | cfg->pmu_mask) & (1u << SBWDT_PMU_BIT)) {
		w->why = "no sbwdt= and the PMU masks its reset: not the loader's, not fed";
		return;
	} else {
		w->feed_s = (w->hw_ms + 999u) / 1000u;
	}
	w->period_ms = sbwdt_period_ms(w->feed_s, w->hw_ms);
	if (w->period_ms == 0) {
		w->why = "no feed period";
		return;
	}
	if (cfg->sweetreboot_s) {
		w->deadman_ms = ((uint64_t)cfg->sweetreboot_s + 60u) * 1000u;
		w->noui_ms = (uint64_t)cfg->noui_s * 1000u;
	}
	w->user_timeout_ms = cfg->user_timeout_s * 1000u;
	w->may_feed = true;
	w->why = cfg->sbwdt_found ? "sbwdt=N: fed until a panic, a dead-man or a userspace stop" :
	    "armed by the loader without sbwdt=: fed for its own timeout";
}

void
sbwdt_checkin(struct sbwdt *w, uint64_t now_ms)
{
	w->checkins++;
	w->last_checkin_ms = now_ms;
	w->user_armed = true;
}

bool
sbwdt_tick(struct sbwdt *w, uint64_t now_ms, bool panic, bool first_frame)
{
	if (!w->may_feed || w->stopped != SBWDT_STOP_NONE) {
		return false;
	}
	if (panic) {
		w->stopped = SBWDT_STOP_PANIC;
		return false;
	}
	if (w->deadman_ms && now_ms >= w->deadman_ms) {
		w->stopped = SBWDT_STOP_DEADMAN;
		return false;
	}
	if (w->noui_ms) {
		if (first_frame) {
			w->noui_ms = 0;
		} else if (now_ms >= w->noui_ms) {
			w->stopped = SBWDT_STOP_NOUI;
			return false;
		}
	}
	if (w->user_timeout_ms && w->user_armed && now_ms - w->last_checkin_ms > w->user_timeout_ms) {
		w->stopped = SBWDT_STOP_USERSPACE;
		return false;
	}
	w->feeds++;
	return true;
}
