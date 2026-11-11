/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Host tests of StarbiteRTC's hw/: date maths against the host's timegm/gmtime, decode/encode in binary and BCD,
 * the latch and write sequences, and the whole path through StarbiteACPM's IPC + write policy on the mock APM.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../hw/s2mps18_rtc.h"
#include "../../StarbiteACPM/hw/acpm_ipc.h"
#include "../../StarbiteACPM/hw/s2mps18_policy.h"
#include "../../StarbiteACPM/test/mock_apm.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- date maths ---- */

static void
t_dates(void)
{
	unsigned bad = 0;
	for (uint64_t t = 0; t < 4102444800ull; t += 86400ull * 7 + 3671) {    /* 1970..2100, odd steps */
		struct s2m_tm tm;
		struct tm g;
		time_t tt = (time_t)t;
		s2m_unix_to_tm(t, &tm);
		gmtime_r(&tt, &g);
		if (tm.year != (uint32_t)g.tm_year + 1900 || tm.month != (uint32_t)g.tm_mon + 1 ||
		    tm.day != (uint32_t)g.tm_mday || tm.hour != (uint32_t)g.tm_hour || tm.min != (uint32_t)g.tm_min ||
		    tm.sec != (uint32_t)g.tm_sec || tm.wday != (uint32_t)g.tm_wday || s2m_tm_to_unix(&tm) != t) {
			bad++;
		}
	}
	CHECK(bad == 0);
	struct s2m_tm tm = { 2024, 2, 29, 23, 59, 59, 0 };
	CHECK(s2m_tm_to_unix(&tm) == 1709251199ull);
	tm = (struct s2m_tm){ 2026, 11, 2, 8, 49, 3, 0 };
	CHECK(s2m_tm_to_unix(&tm) == 1793609343ull);  /* an earlier port's log line */
}

/* ---- decode / encode ---- */

static void
t_decode(void)
{
	struct s2m_tm tm;
	const char *why;
	/* binary, 24 h: 2026-11-11 18:30:05 (PM bit set as the vendor driver writes it) */
	uint8_t r[12] = { 0x02, 0, 0, 0xf3, 0, 5, 30, 18 | 0x40, 1u << 3, 11, 11, 26 };
	CHECK(s2m_rtc_decode(r, &tm, &why) == S2M_OK);
	CHECK(tm.year == 2026 && tm.month == 11 && tm.day == 11 && tm.hour == 18 && tm.min == 30 && tm.sec == 5);
	CHECK(tm.wday == 3);
	/* BCD, 24 h */
	uint8_t b[12] = { 0x03, 0, 0, 0xf0, 0, 0x05, 0x30, 0x18 | 0x40, 0x08, 0x11, 0x11, 0x26 };
	CHECK(s2m_rtc_decode(b, &tm, &why) == S2M_OK && tm.hour == 18 && tm.day == 11 && tm.year == 2026);
	b[7] = 0x23 | 0x40;
	CHECK(s2m_rtc_decode(b, &tm, &why) == S2M_OK && tm.hour == 23);
	b[5] = 0x5a;                   /* not a BCD digit */
	CHECK(s2m_rtc_decode(b, &tm, &why) == S2M_E_RANGE);
	/* reset clocks */
	uint8_t z[12] = { 0x02, 0, 0, 0x03, 0, 0, 0, 0, 1, 1, 1, 18 };   /* 2018-01-01, CAPSEL not set */
	CHECK(s2m_rtc_decode(z, &tm, &why) == S2M_E_NOTSET);
	z[3] = 0xf3;
	CHECK(s2m_rtc_decode(z, &tm, &why) == S2M_E_NOTSET);              /* 2018: never set */
	z[11] = 26;
	CHECK(s2m_rtc_decode(z, &tm, &why) == S2M_OK);
	z[0] = 0x00;                    /* 12-hour mode: Linux's "reset" */
	z[7] = 12;
	CHECK(s2m_rtc_decode(z, &tm, &why) == S2M_E_NOTSET && tm.hour == 0);
	z[7] = 12 | 0x40;
	CHECK(s2m_rtc_decode(z, &tm, &why) == S2M_E_NOTSET && tm.hour == 12);
	/* range */
	uint8_t q[12] = { 0x02, 0, 0, 0xf0, 0, 0, 0, 0, 1, 30, 2, 26 };    /* 30 February */
	CHECK(s2m_rtc_decode(q, &tm, &why) == S2M_E_RANGE);
	q[9] = 29;
	q[11] = 28;                     /* 2028 is a leap year */
	CHECK(s2m_rtc_decode(q, &tm, &why) == S2M_OK);
	q[7] = 24;
	CHECK(s2m_rtc_decode(q, &tm, &why) == S2M_E_RANGE);
}

static void
t_encode(void)
{
	uint8_t o[7];
	struct s2m_tm tm;
	s2m_unix_to_tm(1793609343ull, &tm);            /* 2026-11-02 08:49:03, a Monday */
	CHECK(s2m_rtc_encode(&tm, 0x02, o) == S2M_OK);
	CHECK(o[0] == 3 && o[1] == 49 && o[2] == 8 && o[3] == 0x02 && o[4] == 2 && o[5] == 11 && o[6] == 26);
	CHECK(s2m_rtc_encode(&tm, 0x03, o) == S2M_OK);
	CHECK(o[0] == 0x03 && o[1] == 0x49 && o[2] == 0x08 && o[4] == 0x02 && o[5] == 0x11 && o[6] == 0x26);
	s2m_unix_to_tm(1793609343ull + 6 * 3600, &tm);
	CHECK(s2m_rtc_encode(&tm, 0x02, o) == S2M_OK && o[2] == (14 | 0x40));
	CHECK(s2m_rtc_encode(&tm, 0x00, o) == S2M_E_MODE);
	/* round trip, every hour of three years, binary and BCD */
	unsigned bad = 0;
	for (uint64_t t = 1767225600ull; t < 1767225600ull + 3 * 366 * 86400ull; t += 3600 + 7) {
		for (unsigned bcd = 0; bcd < 2; bcd++) {
			uint8_t r[12] = { (uint8_t)(0x02 | bcd), 0, 0, 0xf0 };
			s2m_unix_to_tm(t, &tm);
			s2m_rtc_encode(&tm, r[0], &r[5]);
			struct s2m_tm back;
			if (s2m_rtc_decode(r, &back, NULL) != S2M_OK || s2m_tm_to_unix(&back) != t) {
				bad++;
			}
		}
	}
	CHECK(bad == 0);
	/* every encoded byte passes StarbiteACPM's RTC field policy */
	struct s2mps18_policy p;
	memset(&p, 0, sizeof(p));
	bad = 0;
	for (uint64_t t = 1704067200ull; t < 4102444800ull; t += 86400 * 3 + 3599) {
		for (unsigned bcd = 0; bcd < 2; bcd++) {
			s2m_unix_to_tm(t, &tm);
			s2m_rtc_encode(&tm, (uint8_t)(0x02 | bcd), o);
			for (unsigned i = 0; i < 7; i++) {
				bad += !s2mps18_write_allowed(&p, 2, 0x05 + i, o[i], 0, NULL);
			}
		}
	}
	CHECK(bad == 0);
}

/* ---- the whole path: StarbiteRTC hw -> policy -> ACPM IPC -> mock APM's RTC model ---- */

static struct mock_apm m;
static struct acpm a;
static struct s2mps18_policy pol;
static unsigned refused;

static int
ops_rd(void *ctx, uint8_t reg, uint8_t *v)
{
	(void)ctx;
	if (!s2mps18_read_allowed(2, reg)) {
		return -1;
	}
	return acpm_pmic_read(&a, 2, reg, v) == ACPM_OK ? 0 : -1;
}

static int
ops_wr(void *ctx, uint8_t reg, uint8_t v)
{
	uint8_t cur = 0;
	(void)ctx;
	if (acpm_pmic_read(&a, 2, reg, &cur) != ACPM_OK) {
		return -1;
	}
	if (!s2mps18_write_allowed(&pol, 2, reg, v, cur, NULL)) {
		refused++;
		return -1;
	}
	return acpm_pmic_write_raw(&a, 2, reg, v) == ACPM_OK ? 0 : -1;
}

static void
ops_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	mock_apm_io.delay_us(NULL, us);
}

static const struct s2m_rtc_ops ops = { NULL, ops_rd, ops_wr, ops_delay };

static void
setup(unsigned latch_polls, unsigned write_polls)
{
	mock_apm_reset(&m, 8, 16, 0);
	CHECK(acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA) == ACPM_OK);
	memset(&pol, 0, sizeof(pol));
	refused = 0;
	m.latch_delay_polls = latch_polls;
	m.write_delay_polls = write_polls;
}

static void
t_read_path(void)
{
	uint8_t r[12];
	uint64_t t = 0;
	const char *why;
	setup(2, 2);
	/* the registers hold S-Boot's old latch; the counter moved on 300 s: the read must latch first */
	m.now_us = 300ull * 1000000ull;
	CHECK(s2m_rtc_read(&ops, &t, r, &why) == S2M_OK);
	CHECK(t >= 1793609343ull + 300 && t <= 1793609343ull + 301);
	CHECK(strcmp(why, "ok") == 0);
	CHECK((m.rtc[2] & 1) == 0);
	CHECK(refused == 0 && m.faults == 0);
	/* a latch that never clears: still read, flagged */
	setup(1000000, 2);
	CHECK(s2m_rtc_read(&ops, &t, r, &why) == S2M_OK && strstr(why, "RUDR did not clear") != NULL);
	/* never set (2018) */
	setup(1, 1);
	m.rtc_counter = 1514764800ull;  /* 2018-01-01 */
	CHECK(s2m_rtc_read(&ops, &t, r, &why) == S2M_E_NOTSET);
	/* BCD clock */
	setup(1, 1);
	m.rtc[0] = 0x03;
	CHECK(s2m_rtc_read(&ops, &t, r, &why) == S2M_OK && t >= 1793609343ull && t <= 1793609344ull);
	/* ACPM fails */
	setup(1, 1);
	m.speedy_fail = 1;
	CHECK(s2m_rtc_read(&ops, &t, r, &why) == S2M_E_IO);
}

static void
t_write_path(void)
{
	const char *why;
	uint8_t r[12];
	uint64_t t = 0;
	setup(1, 3);
	m.rtc_counter = 1514764800ull;  /* reset to 2018 */
	CHECK(s2m_rtc_write(&ops, 1794000000ull, &why) == S2M_OK);
	CHECK(refused == 0);
	CHECK(s2m_rtc_read(&ops, &t, r, &why) == S2M_OK && t >= 1794000000ull && t <= 1794000001ull);
	/* only RTC writes happened, none to CTRL / WTSR / CAPSEL / MSEC / alarms */
	CHECK(m.rtc[0] == 0x02 && m.rtc[3] == 0xf3 && m.rtc[1] == 0);
	CHECK(m.pmic[0x6c] == 0x2c && m.pmic[0x74] == 0x30);
	/* refuse implausible years */
	CHECK(s2m_rtc_write(&ops, 1514764800ull, &why) == S2M_E_RANGE);
	CHECK(s2m_rtc_write(&ops, 4200000000ull, &why) == S2M_E_RANGE);
	/* 12-hour mode: no write (CTRL is never written) */
	setup(1, 1);
	m.rtc[0] = 0x00;
	CHECK(s2m_rtc_write(&ops, 1794000000ull, &why) == S2M_E_MODE);
	CHECK(m.rtc_writes == 0);
	/* a WUDR that never takes: verify catches it */
	setup(1, 1000000);
	CHECK(s2m_rtc_write(&ops, 1794000000ull, &why) == S2M_E_VERIFY);
	CHECK(strstr(why, "WUDR did not clear") != NULL);
	/* BCD clock write */
	setup(1, 1);
	m.rtc[0] = 0x03;
	CHECK(s2m_rtc_write(&ops, 1800000000ull, &why) == S2M_OK);
	CHECK(s2m_rtc_read(&ops, &t, r, &why) == S2M_OK && t >= 1800000000ull && t <= 1800000001ull);
	CHECK(m.faults == 0 && m.bad_doorbells == 0);
}

int
main(void)
{
	t_dates();
	t_decode();
	t_encode();
	t_read_path();
	t_write_path();
	printf("StarbiteRTC host tests: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
