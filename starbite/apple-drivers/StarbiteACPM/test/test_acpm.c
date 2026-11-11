/* SPDX-License-Identifier: APSL-2.0 */
/* Host tests of StarbiteACPM's hw/ layer against the mock APM. */
#include <stdio.h>
#include <string.h>
#include "mock_apm.h"
#include "../hw/acpm_ipc.h"
#include "../hw/s2mps18_policy.h"

extern int mock_verbose;
static int fails, checks;

#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct mock_apm m;
static struct acpm a;

static void
t_setup_layout(void)
{
	mock_apm_reset(&m, 8, 16, 0);
	CHECK(acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA) == ACPM_OK);
	CHECK(a.ready);
	CHECK(a.ap_max == 4);
	CHECK(a.index0_id == 0);
	CHECK(a.ch[2].usable && !a.ch[2].buffer && a.ch[2].len == 8 && a.ch[2].elem == 16);
	CHECK(a.ch[7].usable && a.ch[7].buffer);
	CHECK(!a.ch[1].usable);
	CHECK(strncmp(a.version, "MOCK-9810", 9) == 0);
	CHECK(m.faults == 0);
}

static void
t_bad_tables(void)
{
	mock_apm_reset(&m, 8, 16, 0);
	/* initdata below the faulting pages: refused without touching them */
	CHECK(acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, 0x1000) == ACPM_E_TABLE);
	CHECK(m.faults == 0);
	/* channel table pointing into the faulting pages */
	m.sram[(ACPM_S9_INITDATA + ACPM_ID_CHANNELS) / 4] = 0x100;
	CHECK(acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA) == ACPM_E_TABLE);
	CHECK(m.faults == 0);
	/* ap_max absurd */
	mock_apm_reset(&m, 8, 16, 0);
	m.sram[(ACPM_S9_INITDATA + ACPM_ID_AP_MAX) / 4] = 200;
	CHECK(acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA) == ACPM_E_TABLE);
	/* channel 2's queue base outside the SRAM: no PMIC channel */
	mock_apm_reset(&m, 8, 16, 0);
	m.sram[(0x8000 + ACPM_CH_SIZE + ACPM_CH_IN_BASE) / 4] = 0x2fff0;
	CHECK(acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA) == ACPM_E_NODEV);
	CHECK(m.faults == 0);
	/* odd element size */
	mock_apm_reset(&m, 8, 18, 0);
	CHECK(acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA) == ACPM_E_NODEV);
}

static void
t_framing(void)
{
	CHECK(acpm_pmic_w0(1, 0x6c) == 0x16c);
	CHECK(acpm_pmic_w0(2, 0x0b) == 0x20b);
	CHECK(acpm_pmic_w1(ACPM_FUNC_WRITE, 0xec, 0) == 0xec01);
	CHECK(acpm_pmic_w1(ACPM_FUNC_UPDATE, 0x01, 0x01) == 0x010102);
	mock_apm_reset(&m, 8, 16, 0);
	acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA);
	uint8_t v = 0;
	CHECK(acpm_pmic_read(&a, 0, 0, &v) == ACPM_OK && v == 0x51);
	CHECK((m.last_w0 & 0xfffu) == 0x000 && ((m.last_w0 >> 16) & 0x3f) != 0);
	CHECK(acpm_pmic_read(&a, 1, 0x6c, &v) == ACPM_OK && v == 0x2c);
	CHECK((m.last_w0 & 0xfffu) == 0x16c && (m.last_w1 & 0xff) == 0);
	CHECK(acpm_pmic_write_raw(&a, 1, 0x6c, 0xec) == ACPM_OK && m.pmic[0x6c] == 0xec);
	CHECK((m.last_w1 & 0xffff) == 0xec01);
	CHECK(m.bad_doorbells == 0 && m.faults == 0);
	CHECK(acpm_pmic_read(&a, 16, 0, &v) == ACPM_E_ARG);
}

static void
t_sequence_wrap(void)
{
	mock_apm_reset(&m, 4, 16, 0);
	acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA);
	uint8_t v;
	unsigned bad = 0;
	uint32_t seen = 0;
	for (unsigned i = 0; i < 200; i++) {
		m.pmic[0x50] = (uint8_t)i;
		if (acpm_pmic_read(&a, 1, 0x50, &v) != ACPM_OK || v != (uint8_t)i) {
			bad++;
		}
		uint32_t s = (m.last_w0 >> 16) & 0x3f;
		if (s == 0) {
			bad++;
		}
		seen |= s == 63 ? 1u : 0;
	}
	CHECK(bad == 0);
	CHECK(seen == 1);               /* 1..63 used, never 0 */
	CHECK(m.faults == 0);
}

static void
t_stale_answers(void)
{
	/* an earlier kernel left answers with seq 1, 2, 3 in the queue: our first seq is 1 too */
	mock_apm_reset(&m, 8, 16, 3);
	acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA);
	CHECK(a.st.stale_drained >= 1);
	uint8_t v = 0;
	CHECK(acpm_pmic_read(&a, 1, 0x74, &v) == ACPM_OK && v == 0x30);   /* not the stale 0x77 */
	CHECK(acpm_pmic_read(&a, 1, 0x2b, &v) == ACPM_OK && v == 0x50);
}

static void
t_timeouts_and_resend(void)
{
	mock_apm_reset(&m, 8, 16, 0);
	acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA);
	a.answer_us = 5000;
	a.room_us = 1000;
	uint8_t v = 0;
	m.drop_answers = 1;             /* the first send is lost: the second gets through */
	CHECK(acpm_pmic_read(&a, 1, 0x6c, &v) == ACPM_OK && v == 0x2c);
	CHECK(a.st.resends == 1);
	m.drop_answers = 10;            /* all three lost */
	CHECK(acpm_pmic_read(&a, 1, 0x6c, &v) == ACPM_E_TIMEOUT);
	CHECK(a.st.timeouts == 1);
	m.drop_answers = 0;
	m.answer_delay_polls = 50;      /* slow but within the deadline */
	CHECK(acpm_pmic_read(&a, 1, 0x6c, &v) == ACPM_OK);
	m.answer_delay_polls = 0;
	/* a stuck APM: the request queue fills, then no room */
	mock_apm_reset(&m, 4, 16, 0);
	acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA);
	a.answer_us = 2000;
	a.room_us = 500;
	m.never_take = true;
	CHECK(acpm_pmic_read(&a, 1, 0x6c, &v) == ACPM_E_TIMEOUT);
	CHECK(acpm_pmic_read(&a, 1, 0x6c, &v) == ACPM_E_TIMEOUT);
	m.never_take = false;
	CHECK(m.faults == 0);
}

static void
t_status_error(void)
{
	mock_apm_reset(&m, 8, 16, 0);
	acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA);
	uint8_t v = 0x99;
	m.speedy_fail = 1;
	CHECK(acpm_pmic_read(&a, 1, 0x6c, &v) == ACPM_E_STATUS && v == 0x99);
	m.speedy_fail = 1;
	CHECK(acpm_pmic_write_raw(&a, 1, 0x6c, 0xec) == ACPM_E_STATUS);
	CHECK(a.st.status_errors == 2);
}

static void
t_buffer_and_dvfs(void)
{
	mock_apm_reset(&m, 8, 16, 0);
	acpm_setup(&a, &mock_apm_io, ACPM_S9_SRAM_SIZE, ACPM_S9_SRAM_FIRST, ACPM_S9_INITDATA);
	uint32_t cmd[4] = { 1, 0x1234, 0, 0 };
	CHECK(acpm_request(&a, 7, cmd) == ACPM_OK && cmd[1] == 0xabcd1234u);
	CHECK((m.intsr1 & (1u << 7)) == 0);    /* cleared after the read */
	uint32_t k = 0;
	CHECK(acpm_dvfs_request(&a, ACPM_DVFS_CPUCL1, 1794000, false, NULL) == ACPM_OK);
	CHECK(m.dvfs_seen && m.dvfs_domain == 3 && m.dvfs_khz == 1794000);
	CHECK(acpm_dvfs_request(&a, ACPM_DVFS_CPUCL1, 0, true, &k) == ACPM_OK && k == 1794000);
	CHECK(acpm_request(&a, 9, cmd) == ACPM_E_NODEV);
}

static void
t_policy(void)
{
	struct s2mps18_policy p;
	const char *why;
	memset(&p, 0, sizeof(p));
	/* reads */
	CHECK(!s2mps18_read_allowed(1, 0x00) && !s2mps18_read_allowed(1, 0x06));
	CHECK(s2mps18_read_allowed(1, 0x07) && s2mps18_read_allowed(0, 0x00) && s2mps18_read_allowed(2, 0x0b));
	CHECK(!s2mps18_read_allowed(3, 0));
	/* today's writes pass */
	CHECK(s2mps18_write_allowed(&p, 1, 0x6c, 0xec, 0x2c, &why));
	CHECK(s2mps18_write_allowed(&p, 1, 0x6c, 0xec, 0x00, &why));
	CHECK(s2mps18_write_allowed(&p, 1, 0x74, 0xf0, 0x3c, &why));
	CHECK(s2mps18_write_allowed(&p, 1, 0x68, 0x28, 0xe8, &why));
	CHECK(s2mps18_write_allowed(&p, 1, 0x68, 0xe8, 0x28, &why));
	CHECK(s2mps18_write_allowed(&p, 1, 0x4a, 0xc0, 0x3c, &why));
	CHECK(s2mps18_write_allowed(&p, 1, 0x4a, 0xfc, 0xc0, &why));
	CHECK(s2mps18_write_allowed(&p, 1, 0x4a, 0x3c, 0xfc, &why));
	CHECK(s2mps18_write_allowed(&p, 1, 0x4a, 0x00, 0xc0, &why));      /* off at its vsel 0 */
	CHECK(s2mps18_write_allowed(&p, 1, 0x2b, 0x68, 0x50, &why));
	/* anything else is refused */
	CHECK(!s2mps18_write_allowed(&p, 1, 0x6c, 0xed, 0x2c, &why));     /* a higher voltage */
	CHECK(!s2mps18_write_allowed(&p, 1, 0x6c, 0xec, 0x3f, &why));     /* unexpected programmed vsel */
	CHECK(!s2mps18_write_allowed(&p, 1, 0x74, 0xfc, 0x30, &why));     /* 3.3 V */
	CHECK(!s2mps18_write_allowed(&p, 1, 0x74, 0xf0, 0x10, &why));
	CHECK(!s2mps18_write_allowed(&p, 1, 0x68, 0xfc, 0x28, &why));
	CHECK(!s2mps18_write_allowed(&p, 1, 0x68, 0xe8, 0x10, &why));
	CHECK(!s2mps18_write_allowed(&p, 1, 0x4a, 0xd0, 0xc0, &why));
	CHECK(!s2mps18_write_allowed(&p, 1, 0x2b, 0x70, 0x50, &why));     /* 1000 mV: no */
	CHECK(!s2mps18_write_allowed(&p, 1, 0x2b, 0x50, 0x68, &why));     /* restore before any read: no */
	s2mps18_note_read(&p, 1, 0x2b, 0x50);
	s2mps18_note_read(&p, 1, 0x2b, 0x68);                             /* only the first is kept */
	CHECK(s2mps18_write_allowed(&p, 1, 0x2b, 0x50, 0x68, &why));      /* restore the boot value */
	CHECK(!s2mps18_write_allowed(&p, 1, 0x2b, 0x51, 0x68, &why));
	CHECK(!s2mps18_write_allowed(&p, 1, 0x1d, 0x00, 0x00, &why));     /* BUCK1 */
	CHECK(!s2mps18_write_allowed(&p, 1, 0x00, 0x00, 0x00, &why));
	CHECK(!s2mps18_write_allowed(&p, 0, 0x02, 0x00, 0x00, &why));     /* TOP */
	CHECK(!s2mps18_write_allowed(&p, 3, 0x02, 0x00, 0x00, &why));
	/* RTC */
	CHECK(s2mps18_write_allowed(&p, 2, 0x02, 0x01, 0x00, &why));
	CHECK(s2mps18_write_allowed(&p, 2, 0x02, 0x12, 0x10, &why));
	CHECK(!s2mps18_write_allowed(&p, 2, 0x02, 0x04, 0x00, &why));     /* FREEZE */
	CHECK(!s2mps18_write_allowed(&p, 2, 0x02, 0x10, 0x00, &why));     /* AUDR */
	CHECK(!s2mps18_write_allowed(&p, 2, 0x02, 0x03, 0x00, &why));     /* both at once */
	CHECK(!s2mps18_write_allowed(&p, 2, 0x00, 0x02, 0x02, &why));     /* CTRL */
	CHECK(!s2mps18_write_allowed(&p, 2, 0x01, 0x00, 0x00, &why));     /* WTSR/SMPL */
	CHECK(!s2mps18_write_allowed(&p, 2, 0x03, 0xf0, 0xf3, &why));     /* CAPSEL */
	CHECK(!s2mps18_write_allowed(&p, 2, 0x04, 0x00, 0x00, &why));     /* MSEC */
	CHECK(!s2mps18_write_allowed(&p, 2, 0x0c, 0x00, 0x00, &why));     /* alarm 0 */
	CHECK(!s2mps18_write_allowed(&p, 2, 0x1a, 0x00, 0x00, &why));     /* OSCCTRL */
	CHECK(s2mps18_write_allowed(&p, 2, 0x05, 59, 0, &why));
	CHECK(!s2mps18_write_allowed(&p, 2, 0x05, 0x60, 0, &why));
	CHECK(s2mps18_write_allowed(&p, 2, 0x07, 23 | 0x40, 0, &why));
	CHECK(!s2mps18_write_allowed(&p, 2, 0x07, 0x80, 0, &why));
	CHECK(s2mps18_write_allowed(&p, 2, 0x08, 0x40, 0, &why));
	CHECK(!s2mps18_write_allowed(&p, 2, 0x08, 0x03, 0, &why));
	CHECK(!s2mps18_write_allowed(&p, 2, 0x09, 0, 0, &why));
	CHECK(!s2mps18_write_allowed(&p, 2, 0x0a, 13 + 6, 0, &why));      /* 0x13 */
	CHECK(s2mps18_write_allowed(&p, 2, 0x0b, 26, 0, &why));
}

/* Exhaustive: no PMIC-block register outside the five rails accepts any value. */
static void
t_policy_exhaustive(void)
{
	struct s2mps18_policy p;
	memset(&p, 0, sizeof(p));
	unsigned allowed = 0;
	for (unsigned reg = 0; reg < 256; reg++) {
		for (unsigned v = 0; v < 256; v++) {
			for (unsigned cur = 0; cur < 256; cur += 17) {
				if (s2mps18_write_allowed(&p, 1, reg, (uint8_t)v, (uint8_t)cur, NULL)) {
					allowed++;
					CHECK(reg == 0x6c || reg == 0x74 || reg == 0x2b || reg == 0x68 || reg == 0x4a);
				}
				if (s2mps18_write_allowed(&p, 0, reg, (uint8_t)v, (uint8_t)cur, NULL)) {
					CHECK(0);
				}
			}
		}
	}
	CHECK(allowed > 0);
	/* every allowed LDO write keeps a voltage today's code uses */
	for (unsigned v = 0; v < 256; v++) {
		for (unsigned cur = 0; cur < 256; cur++) {
			if (s2mps18_write_allowed(&p, 1, 0x6c, (uint8_t)v, (uint8_t)cur, NULL)) {
				CHECK((v & 0x3f) == 0x2c);
			}
			if (s2mps18_write_allowed(&p, 1, 0x74, (uint8_t)v, (uint8_t)cur, NULL)) {
				CHECK((v & 0x3f) == 0x30);
			}
			if (s2mps18_write_allowed(&p, 1, 0x68, (uint8_t)v, (uint8_t)cur, NULL)) {
				CHECK((v & 0x3f) == 0x28);
			}
			if (s2mps18_write_allowed(&p, 1, 0x4a, (uint8_t)v, (uint8_t)cur, NULL)) {
				CHECK((v & 0x3f) == 0x00 || (v & 0x3f) == 0x3c || (v & 0xc0) == 0);
			}
			if (s2mps18_write_allowed(&p, 1, 0x2b, (uint8_t)v, (uint8_t)cur, NULL)) {
				CHECK(v == 0x68);
			}
		}
	}
}

static void
t_dvfs_limits(void)
{
	const char *why;
	CHECK(acpm_dvfs_allowed(2, 1690000, false, &why));
	CHECK(acpm_dvfs_allowed(3, 1794000, false, &why));
	CHECK(!acpm_dvfs_allowed(3, 1794001, false, &why));
	CHECK(!acpm_dvfs_allowed(2, 2002000, true, &why));
	CHECK(acpm_dvfs_allowed(3, 2314000, true, &why));
	CHECK(acpm_dvfs_allowed(3, 2704000, true, &why));
	CHECK(!acpm_dvfs_allowed(3, 2704001, true, &why));
	CHECK(acpm_dvfs_allowed(4, 572000, false, &why));
	CHECK(!acpm_dvfs_allowed(4, 600000, false, &why));
	CHECK(!acpm_dvfs_allowed(0, 2093000, true, &why));   /* MIF */
	CHECK(!acpm_dvfs_allowed(1, 667000, true, &why));    /* INT */
	CHECK(!acpm_dvfs_allowed(2, 0, false, &why));
}

int
main(int argc, char **argv)
{
	mock_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
	t_setup_layout();
	t_bad_tables();
	t_framing();
	t_sequence_wrap();
	t_stale_answers();
	t_timeouts_and_resend();
	t_status_error();
	t_buffer_and_dvfs();
	t_policy();
	t_policy_exhaustive();
	t_dvfs_limits();
	printf("StarbiteACPM host tests: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
