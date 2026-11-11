/* SPDX-License-Identifier: APSL-2.0 */
/* Mock APM for the host tests (see mock_apm.h). */
#define _DEFAULT_SOURCE
#include "mock_apm.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct mock_apm *mock;
int mock_verbose;

#define INITDATA        ACPM_S9_INITDATA
#define TABLE           0x8000u
#define QAREA           0x9000u

struct mch {
	uint32_t id, len, elem, desc;
	uint32_t in_rear, in_front, in_base, out_rear, out_front, out_base;
};
static struct mch chans[4];
static unsigned nch;

static uint32_t
S(uint32_t off)
{
	return mock->sram[off / 4];
}

static void
W(uint32_t off, uint32_t v)
{
	mock->sram[off / 4] = v;
}

static uint32_t
io_sram_rd(void *ctx, uint32_t off)
{
	(void)ctx;
	if (off < ACPM_S9_SRAM_FIRST || off >= MOCK_SRAM_SIZE || (off & 3)) {
		mock->faults++;
		return 0;
	}
	return S(off);
}

static void
io_sram_wr(void *ctx, uint32_t off, uint32_t v)
{
	(void)ctx;
	if (off < ACPM_S9_SRAM_FIRST || off >= MOCK_SRAM_SIZE || (off & 3)) {
		mock->faults++;
		return;
	}
	W(off, v);
}

static uint32_t
io_mbox_rd(void *ctx, uint32_t off)
{
	(void)ctx;
	if (off == ACPM_MB_INTSR1) {
		return mock->intsr1;
	}
	if (off == ACPM_MB_INTMR1) {
		return mock->intmr1;
	}
	return 0;
}

static void
io_mbox_wr(void *ctx, uint32_t off, uint32_t v)
{
	(void)ctx;
	if (off == ACPM_MB_INTGR0) {
		if ((v & 0xffffu) || !(v >> 16)) {
			mock->bad_doorbells++;
		}
		mock->doorbells |= v >> 16;
		mock->pending_polls = 0;
	} else if (off == ACPM_MB_INTCR1) {
		mock->intsr1 &= ~v;
	}
}

static uint8_t
bcd(unsigned v)
{
	return (uint8_t)((v / 10) << 4 | (v % 10));
}

static unsigned
unbcd(uint8_t v)
{
	return (v >> 4) * 10u + (v & 0xfu);
}

void
mock_rtc_latch(struct mock_apm *m)
{
	time_t t = (time_t)(m->rtc_counter + m->now_us / 1000000u);
	struct tm tm;
	gmtime_r(&t, &tm);
	bool b = m->rtc[0] & 1u;
	m->rtc[0x04] = (uint8_t)((m->now_us / 1000u) % 1000u / 4u);
	m->rtc[0x05] = b ? bcd((unsigned)tm.tm_sec) : (uint8_t)tm.tm_sec;
	m->rtc[0x06] = b ? bcd((unsigned)tm.tm_min) : (uint8_t)tm.tm_min;
	m->rtc[0x07] = (uint8_t)((b ? bcd((unsigned)tm.tm_hour) : (uint8_t)tm.tm_hour) | (tm.tm_hour >= 12 ? 0x40u : 0));
	m->rtc[0x08] = (uint8_t)(1u << tm.tm_wday);
	m->rtc[0x09] = b ? bcd((unsigned)tm.tm_mday) : (uint8_t)tm.tm_mday;
	m->rtc[0x0a] = b ? bcd((unsigned)tm.tm_mon + 1u) : (uint8_t)(tm.tm_mon + 1);
	m->rtc[0x0b] = b ? bcd((unsigned)tm.tm_year - 100u) : (uint8_t)(tm.tm_year - 100);
}

static void
rtc_commit(void)
{
	bool b = mock->rtc[0] & 1u;
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	tm.tm_sec = (int)(b ? unbcd(mock->rtc[5]) : mock->rtc[5]);
	tm.tm_min = (int)(b ? unbcd(mock->rtc[6]) : mock->rtc[6]);
	tm.tm_hour = (int)(b ? unbcd(mock->rtc[7] & 0x3fu) : (mock->rtc[7] & 0x1fu));
	tm.tm_mday = (int)(b ? unbcd(mock->rtc[9]) : mock->rtc[9]);
	tm.tm_mon = (int)(b ? unbcd(mock->rtc[0xa]) : mock->rtc[0xa]) - 1;
	tm.tm_year = (int)(b ? unbcd(mock->rtc[0xb]) : mock->rtc[0xb]) + 100;
	time_t t = timegm(&tm);
	mock->rtc_counter = (uint64_t)t - mock->now_us / 1000000u;
}

static void
rtc_tick(void)
{
	if (mock->rudr_pending > 0 && --mock->rudr_pending == 0) {
		mock_rtc_latch(mock);
		mock->rtc[2] &= (uint8_t)~0x01u;
	}
	if (mock->wudr_pending > 0 && --mock->wudr_pending == 0) {
		rtc_commit();
		mock->rtc[2] &= (uint8_t)~0x02u;
	}
}

static uint8_t *
block(uint32_t type)
{
	return type == 0 ? mock->top : type == 1 ? mock->pmic : type == 2 ? mock->rtc : NULL;
}

static void
pmic_do(uint32_t w[4])
{
	uint32_t type = (w[0] >> 8) & 0xfu, reg = w[0] & 0xffu, func = w[1] & 0xffu;
	uint8_t val = (uint8_t)(w[1] >> 8), *b = block(type);
	mock->last_w0 = w[0];
	mock->last_w1 = w[1];
	if (mock->speedy_fail) {
		mock->speedy_fail--;
		w[1] = 1u << 24;
		return;
	}
	if (b == NULL) {
		w[1] = 2u << 24;
		return;
	}
	if (func == 0) {
		mock->pmic_reads++;
		w[1] = (uint32_t)b[reg] << 8;
		if (type == 1 && reg <= 6) {
			b[reg] = 0;             /* clear on read */
		}
		return;
	}
	mock->pmic_writes++;
	b[reg] = val;
	if (type == 2) {
		mock->rtc_writes++;
		if (reg == 2 && (val & 1u)) {
			mock->rudr_pending = (int)mock->latch_delay_polls;
			if (mock->latch_delay_polls == 0) {
				mock_rtc_latch(mock);
				mock->rtc[2] &= (uint8_t)~0x01u;
			}
		}
		if (reg == 2 && (val & 2u)) {
			mock->wudr_pending = (int)mock->write_delay_polls;
			if (mock->write_delay_polls == 0) {
				rtc_commit();
				mock->rtc[2] &= (uint8_t)~0x02u;
			}
		}
	}
	w[1] = 0;
}

static void
answer(struct mch *c, uint32_t w[4])
{
	if (c->len <= 1) {
		for (unsigned i = 0; i < 4; i++) {
			W(c->out_base + 4 * i, w[i]);
		}
	} else {
		uint32_t f = S(c->out_front);
		for (unsigned i = 0; i < c->elem / 4; i++) {
			W(c->out_base + f * c->elem + 4 * i, i < 4 ? w[i] : 0);
		}
		W(c->out_front, (f + 1) % c->len);
	}
	mock->intsr1 |= 1u << c->id;
}

static void
firmware(void)
{
	for (unsigned k = 0; k < nch; k++) {
		struct mch *c = &chans[k];
		if (!(mock->doorbells & (1u << c->id))) {
			continue;
		}
		mock->doorbells &= ~(1u << c->id);
		if (mock->never_take) {
			continue;
		}
		uint32_t reqs[64][4];
		unsigned n = 0;
		if (c->len <= 1) {
			for (unsigned i = 0; i < 4; i++) {
				reqs[0][i] = S(c->in_base + 4 * i);
			}
			n = 1;
		} else {
			uint32_t r = S(c->in_rear), f = S(c->in_front);
			for (; r != f; r = (r + 1) % c->len) {
				for (unsigned i = 0; i < 4; i++) {
					reqs[n][i] = S(c->in_base + r * c->elem + 4 * i);
				}
				n++;
			}
			W(c->in_rear, r);
		}
		for (unsigned i = 0; i < n; i++) {
			uint32_t *w = reqs[i];
			if (mock->drop_answers) {
				mock->drop_answers--;
				continue;
			}
			if (c->id == 2) {
				pmic_do(w);
			} else if (c->id == 0) {
				if (w[2] == 0) {
					mock->dvfs_seen = true;
					mock->dvfs_domain = w[0] & 0xffffu;
					mock->dvfs_khz = w[1];
				} else {
					w[1] = mock->dvfs_khz;
				}
			} else {
				w[1] = 0xabcd0000u | (w[1] & 0xffffu);
			}
			answer(c, w);
		}
	}
}

static void
io_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	mock->now_us += us;
	rtc_tick();
	if (mock->doorbells && ++mock->pending_polls > mock->answer_delay_polls) {
		firmware();
	}
}

static uint64_t
io_now(void *ctx)
{
	(void)ctx;
	return mock->now_us;
}

static void
io_barrier(void *ctx)
{
	(void)ctx;
}

static void
io_log(void *ctx, const char *fmt, ...)
{
	(void)ctx;
	if (!mock_verbose) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

struct acpm_io mock_apm_io = {
	NULL, io_sram_rd, io_sram_wr, io_mbox_rd, io_mbox_wr, io_barrier, io_delay, io_now, io_log,
};

static void
add(uint32_t id, uint32_t len, uint32_t elem, uint32_t *area)
{
	struct mch *c = &chans[nch];
	c->id = id;
	c->len = len;
	c->elem = elem;
	c->desc = TABLE + nch * ACPM_CH_SIZE;
	c->in_rear = *area;
	c->in_front = *area + 4;
	c->out_rear = *area + 8;
	c->out_front = *area + 12;
	c->in_base = *area + 16;
	uint32_t q = (len ? len : 1) * elem;
	c->out_base = c->in_base + q;
	*area += 16 + 2 * q;
	W(c->desc + ACPM_CH_ID, 0xaa00u | id);  /* the high bytes are not ours */
	W(c->desc + ACPM_CH_OWNER, 0);
	W(c->desc + ACPM_CH_TYPE, len <= 1 ? 2 : 0);
	W(c->desc + ACPM_CH_IN_REAR, c->in_rear);
	W(c->desc + ACPM_CH_IN_FRONT, c->in_front);
	W(c->desc + ACPM_CH_IN_BASE, c->in_base);
	W(c->desc + ACPM_CH_OUT_REAR, c->out_rear);
	W(c->desc + ACPM_CH_OUT_FRONT, c->out_front);
	W(c->desc + ACPM_CH_OUT_BASE, c->out_base);
	W(c->desc + ACPM_CH_QLEN, len);
	W(c->desc + ACPM_CH_ELEM, elem);
	W(c->desc + ACPM_CH_AP_POLL, 1);
	nch++;
}

void
mock_apm_reset(struct mock_apm *m, uint32_t len, uint32_t elem, unsigned stale)
{
	memset(m, 0, sizeof(*m));
	mock = m;
	nch = 0;
	uint32_t area = QAREA;
	const char *ver = "MOCK-9810 ACPM 92dbdb1 x";
	for (unsigned i = 0; i < 24; i += 4) {
		uint32_t w = 0;
		for (unsigned b = 0; b < 4; b++) {
			w |= (uint32_t)(uint8_t)ver[i + b] << (8 * b);
		}
		W(INITDATA + ACPM_ID_STRINGS + i, w);
	}
	add(0, 4, 16, &area);           /* DVFS, table index 0 */
	add(2, len, elem, &area);       /* PMIC */
	add(5, 4, 16, &area);
	add(7, 1, 16, &area);           /* a single-slot buffer */
	W(INITDATA + ACPM_ID_CHANNELS, TABLE);
	W(INITDATA + ACPM_ID_NCHANNELS, 12);
	W(INITDATA + ACPM_ID_AP_MAX, nch);
	m->top[0] = 0x51;               /* PMICID (an example value) */
	m->pmic[0x6c] = 0x2c;           /* LDO35 off, 1.8 V programmed */
	m->pmic[0x74] = 0x30;           /* LDO43 off, 3.0 V */
	m->pmic[0x2b] = 0x50;           /* BUCK6 800 mV */
	m->pmic[0x68] = 0xe8;
	m->pmic[0x4a] = 0xfc;
	m->pmic[0x01] = 0x5a;           /* INT2: clear on read */
	m->rtc[0x00] = 0x02;            /* binary, 24 h */
	m->rtc[0x03] = 0xf3;            /* CAPSEL as Linux leaves it */
	m->rtc_counter = 1793609343u;   /* 2026-11-02 08:49:03 */
	mock_rtc_latch(m);
	for (unsigned s = 0; s < stale && s + 1 < len; s++) {
		struct mch *c = &chans[1];
		uint32_t f = S(c->out_front);
		W(c->out_base + f * c->elem, (uint32_t)(s + 1) << 16);     /* answers with seq 1, 2, ...: stale */
		W(c->out_base + f * c->elem + 4, 0x7700u);
		W(c->out_front, (f + 1) % c->len);
	}
}
