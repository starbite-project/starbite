/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Host tests of StarbiteWatchdog's hw/: the dead-man contract, and (when the build tree is there) the watchdog
 * maths checked against today's kernel's own build/xnu-12377/bsd/dev/starlte_health.h, which
 * tools/xnu-health-test checks against the loader.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../hw/sbwdt.h"
#ifdef HAVE_STARLTE_HEALTH
#include STARLTE_HEALTH
#endif

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the loader's 60 s arming: prescaler 0xff, divider 128 (WTCON 0xff39), 47607 counts */
#define CON60   0xff39u
#define DAT60   47607u

static struct sbwdt_cfg
cfg60(void)
{
	struct sbwdt_cfg c;
	memset(&c, 0, sizeof(c));
	c.sbwdt_found = true;
	c.sbwdt_s = 60;
	c.wtcon = CON60;
	c.wtdat = DAT60;
	c.noui_s = 300;
	return c;
}

static void
t_maths(void)
{
	CHECK(sbwdt_hw_ms(CON60, DAT60) == 59999);
	CHECK(sbwdt_hw_ms(CON60 & ~1u, DAT60) == 0);            /* no reset */
	CHECK(sbwdt_hw_ms(CON60 & ~0x20u, DAT60) == 0);         /* not enabled */
	CHECK(sbwdt_hw_ms(CON60, 0) == 0);
	CHECK(sbwdt_hw_ms(CON60, 65535) == 82594);              /* the longest: 82.6 s */
	CHECK(sbwdt_period_ms(60, 59999) == 19999);
	CHECK(sbwdt_period_ms(600, 82594) == 27500);
	CHECK(sbwdt_period_ms(1, 59999) == 1000);
	CHECK(sbwdt_period_ms(0, 59999) == 0);
#ifdef HAVE_STARLTE_HEALTH
	unsigned bad = 0;
	for (uint32_t con = 0; con < 0x10000; con += 0x51) {
		for (uint32_t dat = 0; dat < 0x10000; dat += 0x3f1) {
			if (sbwdt_hw_ms(con, dat) != sbh_wdt_hw_ms(con, dat)) {
				bad++;
			}
			for (uint32_t s = 0; s < 100; s += 7) {
				if (sbwdt_period_ms(s, sbwdt_hw_ms(con, dat)) != sbh_wdt_period_ms(s, sbh_wdt_hw_ms(con, dat))) {
					bad++;
				}
			}
		}
	}
	CHECK(bad == 0);
	/* the feed decision agrees with today's kernel when this is not a test boot */
	bad = 0;
	uint32_t pmus[] = { 0, 1u << 23, 1u << 24 };
	for (int found = 0; found < 2; found++) {
		for (uint32_t s = 0; s < 90; s += 15) {
			for (unsigned p = 0; p < 3; p++) {
				for (uint32_t dat = 0; dat < 0x10000; dat += 0x1fff) {
					struct sbwdt_cfg c = cfg60();
					struct sbwdt w;
					c.sbwdt_found = found;
					c.sbwdt_s = s;
					c.wtdat = dat;
					c.pmu_dis = pmus[p];
					sbwdt_plan(&c, &w);
					uint32_t want = sbh_wdt_feed_s(found, s, CON60, dat, pmus[p], 0);
					if ((w.may_feed ? w.feed_s : 0) != want) {
						bad++;
					}
				}
			}
		}
	}
	CHECK(bad == 0);
	printf("  (maths cross-checked against starlte_health.h)\n");
#endif
}

static void
t_contract(void)
{
	struct sbwdt_cfg c = cfg60();
	struct sbwdt w;
	/* normal boot: fed every 20 s */
	sbwdt_plan(&c, &w);
	CHECK(w.may_feed && w.period_ms == 19999 && w.reload == DAT60);
	CHECK(sbwdt_tick(&w, 1000, false, false));
	/* 1: test boot: never */
	c.test_boot = true;
	sbwdt_plan(&c, &w);
	CHECK(!w.may_feed);
	for (uint64_t t = 0; t < 100000; t += 1000) {
		CHECK(!sbwdt_tick(&w, t, false, true));
	}
	sbwdt_checkin(&w, 5);
	CHECK(!sbwdt_tick(&w, 6, false, true));
	/* 2: sbwdt=0 */
	c = cfg60();
	c.sbwdt_s = 0;
	sbwdt_plan(&c, &w);
	CHECK(!w.may_feed);
	/* 3: not armed: never fed (and never armed) */
	c = cfg60();
	c.wtcon = 0;
	sbwdt_plan(&c, &w);
	CHECK(!w.may_feed);
	/* no sbwdt=, S-Boot masked it: not ours */
	c = cfg60();
	c.sbwdt_found = false;
	c.pmu_mask = 1u << 23;
	sbwdt_plan(&c, &w);
	CHECK(!w.may_feed);
	c.pmu_mask = 0;
	sbwdt_plan(&c, &w);
	CHECK(w.may_feed && w.feed_s == 60);
	/* 4: panic is final */
	c = cfg60();
	sbwdt_plan(&c, &w);
	CHECK(sbwdt_tick(&w, 1000, false, false));
	CHECK(!sbwdt_tick(&w, 2000, true, false));
	CHECK(!sbwdt_tick(&w, 3000, false, false));
	CHECK(w.stopped == SBWDT_STOP_PANIC);
	/* 5: 0421 dead-man */
	c = cfg60();
	c.sweetreboot_s = 120;
	sbwdt_plan(&c, &w);
	CHECK(sbwdt_tick(&w, 100000, false, true));
	CHECK(sbwdt_tick(&w, 179999, false, true));
	CHECK(!sbwdt_tick(&w, 180000, false, true) && w.stopped == SBWDT_STOP_DEADMAN);
	c.sweetreboot_s = 600;
	sbwdt_plan(&c, &w);
	CHECK(sbwdt_tick(&w, 299999, false, false));
	CHECK(!sbwdt_tick(&w, 300000, false, false) && w.stopped == SBWDT_STOP_NOUI);
	sbwdt_plan(&c, &w);
	CHECK(sbwdt_tick(&w, 200000, false, true));             /* a frame came: the no-UI limit is gone */
	CHECK(sbwdt_tick(&w, 400000, false, false));
	CHECK(!sbwdt_tick(&w, 660000, false, true) && w.stopped == SBWDT_STOP_DEADMAN);
	/* no sweetreboot: no dead-man */
	c = cfg60();
	sbwdt_plan(&c, &w);
	CHECK(sbwdt_tick(&w, 100000000ull, false, false));
	/* 6: userspace monitoring */
	c = cfg60();
	c.user_timeout_s = 90;
	sbwdt_plan(&c, &w);
	CHECK(sbwdt_tick(&w, 500000, false, true));             /* before the first check-in: kernel liveness only */
	sbwdt_checkin(&w, 500000);
	CHECK(sbwdt_tick(&w, 590000, false, true));
	sbwdt_checkin(&w, 590000);
	CHECK(sbwdt_tick(&w, 680000, false, true));
	CHECK(!sbwdt_tick(&w, 680001, false, true) && w.stopped == SBWDT_STOP_USERSPACE);
	sbwdt_checkin(&w, 680002);                              /* one-way: a late check-in does not resume */
	CHECK(!sbwdt_tick(&w, 680003, false, true));
	/* sbwdtuser unset: check-ins never stop feeding */
	c = cfg60();
	sbwdt_plan(&c, &w);
	sbwdt_checkin(&w, 1);
	CHECK(sbwdt_tick(&w, 10000000, false, true));
}

int
main(void)
{
	t_maths();
	t_contract();
	printf("StarbiteWatchdog host tests: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
