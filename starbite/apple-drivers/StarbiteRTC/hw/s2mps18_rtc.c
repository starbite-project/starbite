/* SPDX-License-Identifier: APSL-2.0 */
/*
 * S2MPS18 RTC: decode/encode, the RUDR latch before a read, the WUDR update after a write. See s2mps18_rtc.h and
 * ../NOTES.md. The read-only decode follows the facts in an earlier port's drivers/power/s2mps18_rtc.c (APSL-2.0,
 * an earlier port; credited in NOTES.md); the latch and the write are new.
 */
#include "s2mps18_rtc.h"

const char *
s2m_strerror(int e)
{
	switch (e) {
	case S2M_OK: return "ok";
	case S2M_E_IO: return "ACPM access failed";
	case S2M_E_NOTSET: return "clock not set";
	case S2M_E_RANGE: return "field out of range";
	case S2M_E_VERIFY: return "read-back differs";
	case S2M_E_MODE: return "not in 24-hour mode";
	default: return "?";
	}
}

/* days since 1970-01-01 of a civil date (y >= 1970) */
static int64_t
days_from_civil(int64_t y, uint32_t m, uint32_t d)
{
	y -= m <= 2;
	int64_t era = y / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * ((int64_t)m + (m > 2 ? -3 : 9)) + 2) / 5 + (int64_t)d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

uint64_t
s2m_tm_to_unix(const struct s2m_tm *tm)
{
	int64_t days = days_from_civil((int64_t)tm->year, tm->month, tm->day);
	return (uint64_t)days * 86400u + tm->hour * 3600u + tm->min * 60u + tm->sec;
}

void
s2m_unix_to_tm(uint64_t t, struct s2m_tm *tm)
{
	int64_t z = (int64_t)(t / 86400u) + 719468;
	uint64_t rem = t % 86400u;
	int64_t era = z / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t y = yoe + era * 400;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	uint32_t d = (uint32_t)(doy - (153 * mp + 2) / 5 + 1);
	uint32_t m = (uint32_t)(mp < 10 ? mp + 3 : mp - 9);
	tm->year = (uint32_t)(y + (m <= 2));
	tm->month = m;
	tm->day = d;
	tm->hour = (uint32_t)(rem / 3600u);
	tm->min = (uint32_t)(rem / 60u % 60u);
	tm->sec = (uint32_t)(rem % 60u);
	tm->wday = (uint32_t)(((t / 86400u) + 4u) % 7u);    /* 1970-01-01 was a Thursday */
}

static uint32_t
days_in_month(uint32_t y, uint32_t m)
{
	static const uint8_t n[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
	return m == 2 && leap ? 29u : n[(m - 1) % 12];
}

/* a field: binary, or BCD (both nibbles must be digits; bad -> 0xff) */
static uint32_t
field(uint8_t v, uint8_t mask, bool bcd)
{
	v &= mask;
	if (!bcd) {
		return v;
	}
	if ((v & 0xfu) > 9u || (v >> 4) > 9u) {
		return 0xffu;
	}
	return (uint32_t)(v >> 4) * 10u + (v & 0xfu);
}

static uint8_t
to_bcd(uint32_t v)
{
	return (uint8_t)((v / 10u) << 4 | (v % 10u));
}

int
s2m_rtc_decode(const uint8_t r[S2M_RTC_NREGS], struct s2m_tm *tm, const char **why)
{
	uint8_t ctrl = r[S2M_RTC_CTRL];
	bool bcd = (ctrl & S2M_CTRL_BCD) != 0, h24 = (ctrl & S2M_CTRL_24H) != 0;
	const char *dummy;

	if (why == 0) {
		why = &dummy;
	}
	*why = "ok";
	tm->sec = field(r[S2M_RTC_SEC], 0x7f, bcd);
	tm->min = field(r[S2M_RTC_MIN], 0x7f, bcd);
	if (h24) {
		tm->hour = field(r[S2M_RTC_HOUR], bcd ? 0x3f : 0x1f, bcd);
	} else {
		uint32_t h = field(r[S2M_RTC_HOUR], 0x1f, bcd);
		bool pm = (r[S2M_RTC_HOUR] & S2M_HOUR_PM) != 0;
		tm->hour = (h >= 1 && h <= 12) ? h % 12u + (pm ? 12u : 0u) : 0xffu;
	}
	tm->day = field(r[S2M_RTC_DAY], bcd ? 0x3f : 0x1f, bcd);
	tm->month = field(r[S2M_RTC_MONTH], bcd ? 0x1f : 0x0f, bcd);
	uint32_t y = field(r[S2M_RTC_YEAR], bcd ? 0xff : 0x7f, bcd);
	tm->year = y == 0xffu ? 0 : 2000u + y;
	tm->wday = 0;
	if (tm->sec > 59 || tm->min > 59 || tm->hour > 23 || tm->month < 1 || tm->month > 12 || tm->day < 1 ||
	    tm->year == 0 || tm->day > days_in_month(tm->year, tm->month)) {
		*why = "a field out of range";
		return S2M_E_RANGE;
	}
	struct s2m_tm t2;
	s2m_unix_to_tm(s2m_tm_to_unix(tm), &t2);
	tm->wday = t2.wday;
	if (!h24 || (r[S2M_RTC_CAPSEL] & 0xf0u) != 0xf0u) {
		*why = "CTRL not 24-hour or CAPSEL[7:4] != 0xF: the RTC was reset";
		return S2M_E_NOTSET;
	}
	if (tm->year < S2M_YEAR_MIN || tm->year > S2M_YEAR_MAX) {
		*why = "the year is not 2024-2099: the clock was never set";
		return S2M_E_NOTSET;
	}
	return S2M_OK;
}

int
s2m_rtc_encode(const struct s2m_tm *tm, uint8_t ctrl, uint8_t out[7])
{
	bool bcd = (ctrl & S2M_CTRL_BCD) != 0;
	if (!(ctrl & S2M_CTRL_24H)) {
		return S2M_E_MODE;
	}
	if (tm->year < 2000u || tm->year > 2099u || tm->month < 1 || tm->month > 12 || tm->day < 1 ||
	    tm->day > days_in_month(tm->year, tm->month) || tm->hour > 23 || tm->min > 59 || tm->sec > 59 ||
	    tm->wday > 6) {
		return S2M_E_RANGE;
	}
	uint32_t f[6] = { tm->sec, tm->min, tm->hour, tm->day, tm->month, tm->year - 2000u };
	uint8_t e[6];
	for (unsigned i = 0; i < 6; i++) {
		e[i] = bcd ? to_bcd(f[i]) : (uint8_t)f[i];
	}
	out[0] = e[0];                                          /* SEC */
	out[1] = e[1];                                          /* MIN */
	out[2] = (uint8_t)(e[2] | (tm->hour >= 12 ? S2M_HOUR_PM : 0));   /* HOUR (+PM, as the vendor driver) */
	out[3] = (uint8_t)(1u << tm->wday);                     /* WEEK */
	out[4] = e[3];                                          /* DAY */
	out[5] = e[4];                                          /* MONTH */
	out[6] = e[5];                                          /* YEAR */
	return S2M_OK;
}

/* Set one UPDATE bit (the register read first, nothing else changed) and wait for it to clear. */
static int
update_bit(const struct s2m_rtc_ops *ops, uint8_t bit, bool *cleared)
{
	uint8_t u = 0;
	*cleared = false;
	if (ops->rd(ops->ctx, S2M_RTC_UPDATE, &u) != 0) {
		return S2M_E_IO;
	}
	if (ops->wr(ops->ctx, S2M_RTC_UPDATE, (uint8_t)(u | bit)) != 0) {
		return S2M_E_IO;
	}
	for (unsigned i = 0; i < S2M_UPDATE_POLLS; i++) {
		ops->delay_us(ops->ctx, S2M_UPDATE_POLL_US);
		if (ops->rd(ops->ctx, S2M_RTC_UPDATE, &u) != 0) {
			return S2M_E_IO;
		}
		if (!(u & bit)) {
			*cleared = true;
			break;
		}
	}
	return S2M_OK;
}

int
s2m_rtc_read_raw(const struct s2m_rtc_ops *ops, uint8_t r[S2M_RTC_NREGS], bool *latched)
{
	bool l = false;
	int e = update_bit(ops, S2M_UPDATE_RUDR, &l);
	if (latched) {
		*latched = l;
	}
	if (e != S2M_OK) {
		return e;
	}
	for (uint8_t i = 0; i < S2M_RTC_NREGS; i++) {
		if (ops->rd(ops->ctx, i, &r[i]) != 0) {
			return S2M_E_IO;
		}
	}
	return S2M_OK;
}

int
s2m_rtc_read(const struct s2m_rtc_ops *ops, uint64_t *unix_secs, uint8_t r[S2M_RTC_NREGS], const char **why)
{
	struct s2m_tm tm;
	bool latched = false;
	const char *dummy;
	if (why == 0) {
		why = &dummy;
	}
	int e = s2m_rtc_read_raw(ops, r, &latched);
	if (e != S2M_OK) {
		*why = "ACPM access failed";
		return e;
	}
	e = s2m_rtc_decode(r, &tm, why);
	if (e == S2M_OK) {
		*unix_secs = s2m_tm_to_unix(&tm);
		if (!latched) {
			*why = "ok, but RUDR did not clear within 50 ms (the value may be an older latch)";
		}
	}
	return e;
}

int
s2m_rtc_write(const struct s2m_rtc_ops *ops, uint64_t unix_secs, const char **why)
{
	struct s2m_tm tm;
	uint8_t ctrl = 0, out[7], r[S2M_RTC_NREGS];
	bool cleared = false;
	const char *dummy;
	if (why == 0) {
		why = &dummy;
	}
	*why = "ok";
	if (ops->rd(ops->ctx, S2M_RTC_CTRL, &ctrl) != 0) {
		*why = "CTRL unreadable";
		return S2M_E_IO;
	}
	s2m_unix_to_tm(unix_secs, &tm);
	if (tm.year < S2M_YEAR_MIN || tm.year > S2M_YEAR_MAX) {
		*why = "refusing to write a year outside 2024-2099";
		return S2M_E_RANGE;
	}
	int e = s2m_rtc_encode(&tm, ctrl, out);
	if (e != S2M_OK) {
		*why = e == S2M_E_MODE ? "CTRL is not 24-hour (and CTRL is never written)" : "encode failed";
		return e;
	}
	for (uint8_t i = 0; i < 7; i++) {
		if (ops->wr(ops->ctx, (uint8_t)(S2M_RTC_SEC + i), out[i]) != 0) {
			*why = "a time register write failed";
			return S2M_E_IO;
		}
	}
	e = update_bit(ops, S2M_UPDATE_WUDR, &cleared);
	if (e != S2M_OK) {
		*why = "UPDATE (WUDR) failed";
		return e;
	}
	uint64_t back = 0;
	const char *rwhy = 0;
	e = s2m_rtc_read(ops, &back, r, &rwhy);
	if (e != S2M_OK || back < unix_secs || back > unix_secs + 3u) {
		*why = cleared ? "read-back differs from what was written" :
		    "read-back differs (and WUDR did not clear within 50 ms)";
		return S2M_E_VERIFY;
	}
	return S2M_OK;
}
