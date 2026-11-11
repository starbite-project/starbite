/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Cluster 1's Exynos watchdog (0x10060000, samsung,exynos7-wdt, OSCCLK 26 MHz) and the loader's dead-man contract,
 * plain C. The register maths and the "may XNU feed it" rule are the same as our build/xnu-12377/bsd/dev/
 * starlte_health.h (0399, host-tested in tools/xnu-health-test against the loader); re-stated here so the driver and
 * its tests stand alone. Facts and the contract: ../NOTES.md.
 *
 * The contract (never broken here):
 *   1. sbstop=<stage> (0423): a test boot. XNU NEVER feeds; the loader's watchdog resets the phone in wdt s.
 *   2. sbwdt=0: never fed.
 *   3. Never armed, never re-programmed: WTCON / WTDAT are only read. A watchdog the loader did not arm stays off.
 *   4. Once a panic starts: never fed again.
 *   5. 0421 dead-man: with sweetreboot=S, feeding stops at S + 60 s, and at sbnoui= s (default 300) if no frame
 *      reached the panel.
 *   6. Userspace monitoring (new): once watchdogd has checked in and sbwdtuser=T is set, a gap of T s between
 *      check-ins stops feeding for good (the phone then resets where INFORM3 says).
 *   Stopping is one-way: nothing here ever resumes feeding.
 */
#ifndef STARBITE_SBWDT_H
#define STARBITE_SBWDT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBWDT_WTCON             0x00u
#define SBWDT_WTDAT             0x04u
#define SBWDT_WTCNT             0x08u
#define SBWDT_WTCON_RSTEN       (1u << 0)
#define SBWDT_WTCON_ENABLE      (1u << 5)
#define SBWDT_CLK_HZ            26000000u
#define SBWDT_PMU_BIT           23u     /* cluster 1 in AUTOMATIC_WDT_RESET_DISABLE / MASK_WDT_RESET_REQUEST */
#define SBWDT_PERIOD_MIN_MS     1000u
#define SBWDT_PERIOD_MAX_MS     27500u

/* What the boot gives: boot-args and the registers as found (read only). */
struct sbwdt_cfg {
	bool sbwdt_found;
	uint32_t sbwdt_s;
	bool test_boot;                 /* sbstop= present */
	uint32_t sweetreboot_s;         /* 0 = none */
	uint32_t noui_s;                /* sbnoui (default 300), 0 = off; only with sweetreboot */
	uint32_t user_timeout_s;        /* sbwdtuser, 0 = no userspace monitoring */
	uint32_t wtcon, wtdat, pmu_dis, pmu_mask;
};

#define SBWDT_STOP_NONE         0
#define SBWDT_STOP_PANIC        1
#define SBWDT_STOP_DEADMAN      2
#define SBWDT_STOP_NOUI         3
#define SBWDT_STOP_USERSPACE    4

struct sbwdt {
	bool may_feed;                  /* decided once at start */
	const char *why;                /* why (not) */
	uint32_t hw_ms, feed_s, period_ms, reload;
	uint64_t deadman_ms, noui_ms;   /* uptime limits, 0 = none */
	uint32_t user_timeout_ms;
	bool user_armed;                /* watchdogd checked in at least once */
	uint64_t last_checkin_ms;
	uint64_t checkins, feeds;
	int stopped;                    /* SBWDT_STOP_* */
};

uint32_t sbwdt_hw_ms(uint32_t wtcon, uint32_t wtdat);
uint32_t sbwdt_period_ms(uint32_t secs, uint32_t hw_ms);

/* Decide at start whether this boot's watchdog may be fed at all, and how often. */
void sbwdt_plan(const struct sbwdt_cfg *cfg, struct sbwdt *w);

/* At each timer tick: true = write WTCNT = reload now. first_frame: a frame has reached the panel. */
bool sbwdt_tick(struct sbwdt *w, uint64_t now_ms, bool panic, bool first_frame);

/* watchdogd checked in. */
void sbwdt_checkin(struct sbwdt *w, uint64_t now_ms);

const char *sbwdt_stop_name(int stopped);

#ifdef __cplusplus
}
#endif
#endif
