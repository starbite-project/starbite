/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Host tests of StarbiteBattery's hw/: a register-level mock of the USI HSI2C controller in AUTO mode with a
 * MAX77705 fuel gauge (0x36) and charger (0x69) behind it, the fixed-point conversions, the published state.
 */
#include <stdio.h>
#include <string.h>
#include "../hw/exynos_hsi2c.h"
#include "../hw/max77705.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the mock controller + devices ---- */

static struct {
	uint32_t reg[0x100 / 4];
	uint8_t rx[64];
	unsigned rx_n, rx_r;
	/* the transfer in flight */
	bool active, receive, stop;
	unsigned count, got;
	uint8_t addr, txbuf[64];
	bool open;                      /* last message ended without STOP: the next is a repeated START */
	/* devices */
	uint16_t fg[256];
	uint8_t chg[256];
	uint8_t ptr_fg, ptr_chg;
	bool fg_present, chg_present;
	/* behaviour */
	bool rs_broken;                 /* a message without STOP fails (NACK) */
	bool no_done_without_stop;      /* no TRANSFER_DONE for a no-STOP write */
	unsigned nack_next;
	/* checks */
	unsigned long_writes, clear_on_read, msgs, rs_reads, resets;
	unsigned writes, bad_writes;    /* 2-byte register writes; any not 0x69:0xbd with WDTCLR = 01 is bad */
	uint8_t last_write;
	uint64_t now;
	/* the charger's watchdog (S-Boot enables it: CNFG_00 WDTEN) */
	uint64_t wdt_last;
	bool wdt_expired;
} b;

#define MOCK_CHG_WDT_US 80000000ull     /* a mock timeout; the real one is unknown (NOTES.md) */

static void
chg_wdt_update(void)
{
	if ((b.chg[CHG_CNFG_00] & CHG_CNFG_00_WDTEN) && !b.wdt_expired && b.now - b.wdt_last > MOCK_CHG_WDT_US) {
		b.wdt_expired = true;           /* sticky, stricter than the chip: the clear must come in time */
	}
}

#define REG(o) b.reg[(o) / 4]

static void
complete(uint32_t irq)
{
	REG(HSI2C_INT_STAT) |= irq;
	b.active = false;
}

static bool
present(uint8_t a)
{
	return (a == MAX77705_FG_ADDR && b.fg_present) || (a == MAX77705_CHG_ADDR && b.chg_present);
}

static void
start(void)
{
	uint32_t ac = REG(HSI2C_AUTO_CONF);
	b.msgs++;
	b.receive = (ac >> 16) & 1;
	b.stop = (ac >> 17) & 1;
	b.count = ac & 0xffff;
	b.addr = (uint8_t)((REG(HSI2C_ADDR) >> 10) & 0x7f);
	b.got = 0;
	b.active = true;
	bool repeated = b.open;
	b.open = !b.stop;
	if (b.nack_next) {
		b.nack_next--;
		complete(0x200);
		b.open = false;
		return;
	}
	if (!present(b.addr) || (b.rs_broken && !b.stop)) {
		complete(0x200);
		b.open = false;
		return;
	}
	if (b.receive) {
		if (repeated) {
			b.rs_reads++;
		}
		for (unsigned i = 0; i < b.count; i++) {
			uint8_t v;
			if (b.addr == MAX77705_FG_ADDR) {
				uint16_t w = b.fg[(uint8_t)(b.ptr_fg + i / 2)];
				v = (uint8_t)(i & 1 ? w >> 8 : w);
			} else {
				uint8_t r = (uint8_t)(b.ptr_chg + i);
				if (r == CHG_INT) {
					b.clear_on_read++;
				}
				chg_wdt_update();
				v = b.chg[r];
				if (r == CHG_DETAILS_01 && b.wdt_expired) {
					v = (uint8_t)((v & 0xf0u) | 0x0bu);        /* CHG_DTLS: watchdog expired */
				}
			}
			b.rx[b.rx_n++ % 64] = v;
		}
		complete(0x80);
	}
}

static void
tx_byte(uint8_t v)
{
	if (!b.active || b.receive) {
		return;
	}
	b.txbuf[b.got++ % 64] = v;
	if (b.got == b.count) {
		if (b.count == 2) {             /* a register write */
			b.writes++;
			b.last_write = b.txbuf[1];
			if (b.addr != MAX77705_CHG_ADDR || b.txbuf[0] != CHG_CNFG_06 || (b.txbuf[1] & 3u) != 1u ||
			    (b.txbuf[1] & 0xf0u)) {
				b.bad_writes++;
			} else {
				chg_wdt_update();
				if (!b.wdt_expired) {
					b.wdt_last = b.now;
				}
				b.chg[CHG_CNFG_06] = b.txbuf[1] & 0x0cu;      /* WDTCLR reads back 0 */
			}
			complete(0x80);
			return;
		}
		if (b.count != 1) {
			b.long_writes++;
		}
		if (b.addr == MAX77705_FG_ADDR) {
			b.ptr_fg = b.txbuf[0];
		} else {
			b.ptr_chg = b.txbuf[0];
		}
		if (b.stop || !b.no_done_without_stop) {
			complete(0x80);
		} else {
			b.active = false;
		}
	}
}

static uint32_t
io_rd(void *ctx, uint32_t off)
{
	(void)ctx;
	if (off == HSI2C_FIFO_STAT) {
		uint32_t v = HSI2C_FIFO_TX_EMPTY;
		if (b.rx_r == b.rx_n) {
			v |= HSI2C_FIFO_RX_EMPTY;
		} else {
			v |= ((b.rx_n - b.rx_r) & 0x7f) << 16;
		}
		return v;
	}
	if (off == HSI2C_RX_DATA) {
		return b.rx_r < b.rx_n ? b.rx[b.rx_r++ % 64] : 0;
	}
	if (off == HSI2C_TRANS_STATUS) {
		return 0;
	}
	return REG(off);
}

static void
io_wr(void *ctx, uint32_t off, uint32_t v)
{
	(void)ctx;
	if (off == HSI2C_INT_STAT) {
		REG(off) &= ~v;
		return;
	}
	if (off == HSI2C_TX_DATA) {
		tx_byte((uint8_t)v);
		return;
	}
	if (off == HSI2C_CTL && (v & 0x80000000u)) {
		b.resets++;
		b.rx_r = b.rx_n = 0;
		b.active = false;
		b.open = false;
		REG(HSI2C_TIMING_FS1) = REG(HSI2C_TIMING_FS2) = REG(HSI2C_TIMING_FS3) = 0;      /* reset clears timing */
	}
	REG(off) = v;
	if (off == HSI2C_AUTO_CONF && (v & 0x80000000u)) {
		start();
	}
}

static void
io_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	b.now += us;
}

static const struct hsi2c_io io = { NULL, io_rd, io_wr, io_delay };
static struct hsi2c h;

static int
bus_read(void *ctx, uint8_t addr7, uint8_t reg, uint8_t *buf, unsigned len)
{
	(void)ctx;
	return hsi2c_read_reg(&h, addr7, reg, buf, len);
}

static int
bus_write(void *ctx, uint8_t addr7, uint8_t reg, uint8_t value)
{
	(void)ctx;
	return hsi2c_write_reg(&h, addr7, reg, value);
}

static const struct max77705_bus bus = { NULL, bus_read, bus_write };
static const struct max77705_bus bus_ro = { NULL, bus_read, NULL };

static void
model(void)
{
	memset(&b, 0, sizeof(b));
	REG(HSI2C_TIMING_FS1) = 0x00f00000u;   /* what S-Boot left (example) */
	REG(HSI2C_TIMING_FS2) = 0x000000e0u;
	REG(HSI2C_TIMING_FS3) = 0x00210000u;
	b.fg_present = b.chg_present = true;
	b.fg[FG_REPSOC] = 0x3280;               /* 50.5 % */
	b.fg[FG_VCELL] = 49280;                 /* 3850 mV */
	b.fg[FG_AVGVCELL] = 49280;
	b.fg[FG_CURRENT] = (uint16_t)-1600;     /* -500 mA */
	b.fg[FG_AVGCURRENT] = (uint16_t)-1600;
	b.fg[FG_TEMP] = 0x1a00;                 /* 26 C */
	b.fg[FG_REPCAP] = 1500;
	b.fg[FG_FULLCAPREP] = 2900;
	b.fg[FG_DESIGNCAP] = 0x0b97;
	b.fg[FG_CYCLES] = 12345;
	b.fg[FG_STATUS] = 0x0002;
	b.fg[FG_INIT_RESULT] = 0x0000;
	b.chg[CHG_INT_OK] = 0x0f;               /* no input */
	b.chg[CHG_DETAILS_00] = 0x00;
	b.chg[CHG_DETAILS_01] = 0x38;           /* BAT_DTLS 3 (ok), CHG_DTLS 8 (off) */
	b.chg[CHG_INT] = 0x55;
	b.chg[CHG_CNFG_00] = 0x15;              /* as S-Boot leaves it: charge mode + WDTEN */
	b.chg[CHG_CNFG_06] = 0x0c;              /* CHGPROT unlocked (example) */
	memset(&h, 0, sizeof(h));
	hsi2c_init(&h, &io);
}

/* ---- tests ---- */

static void
t_conversions(void)
{
	CHECK(max77705_mv(53760) == 4200);
	CHECK(max77705_mv(32000) == 2500);
	CHECK(max77705_mv(60160) == 4700);
	CHECK(max77705_ma((uint16_t)-64) == -20);
	CHECK(max77705_ma(3200) == 1000);
	CHECK(max77705_ma(0x7fff) == 10239);
	CHECK(max77705_soc_x10(0x3280) == 505);
	CHECK(max77705_soc_x10(0x6400) == 1000);
	CHECK(max77705_soc_x10(0x00ff) == 9);
	CHECK(max77705_mah(0x0b97) == 2967);
	CHECK(max77705_temp_c10(0x1a00) == 260);
	CHECK(max77705_temp_c10((uint16_t)0xfb00) == -50);
	CHECK(max77705_cycles(12345) == 123);
}

static void
t_read_allowed(void)
{
	CHECK(!max77705_read_allowed(MAX77705_CHG_ADDR, CHG_INT));
	CHECK(max77705_read_allowed(MAX77705_CHG_ADDR, CHG_INT_OK));
	CHECK(!max77705_read_allowed(MAX77705_PMIC_ADDR, 0x22) && !max77705_read_allowed(MAX77705_PMIC_ADDR, 0x24));
	CHECK(max77705_read_allowed(MAX77705_PMIC_ADDR, 0x00));
	CHECK(!max77705_read_allowed(0x25, 0x03) && max77705_read_allowed(0x25, 0x08));
	CHECK(!max77705_read_allowed(0x62, 0x00));
	CHECK(!max77705_read_allowed(0x50, 0x00));
}

static void
t_timing(void)
{
	model();
	CHECK(REG(HSI2C_TIMING_FS3) == 0x00210000u && REG(HSI2C_TIMING_FS1) == 0x00f00000u);    /* restored */
	CHECK(REG(HSI2C_CONF) & 0x80000000u);
	/* S-Boot left none: the 400 kHz defaults */
	memset(&b, 0, sizeof(b));
	memset(&h, 0, sizeof(h));
	hsi2c_init(&h, &io);
	CHECK(((REG(HSI2C_TIMING_FS3) >> 16) & 0xff) == 0x21 && (REG(HSI2C_TIMING_FS2) & 0xff) == 0xe0 &&
	    ((REG(HSI2C_TIMING_FS1) >> 16) & 0xff) == 0xf0);
}

static void
t_discharging(void)
{
	struct max77705_raw raw;
	struct battery_state st;
	uint32_t low = 0;
	model();
	CHECK(max77705_sample(&bus, &raw) == 0);
	max77705_state(&raw, &st, &low);
	CHECK(st.measured);
	CHECK(st.percent == 51 && st.soc_x10 == 505);
	CHECK(st.mv == 3850 && st.ma == -500 && st.avg_ma == -500);
	CHECK(st.temp_c10 == 260 && st.cur_mah == 1500 && st.max_mah == 2900 && st.design_mah == 2967);
	CHECK(st.cycles == 123);
	CHECK(!st.external && !st.charging && !st.full && !st.wireless);
	CHECK(st.minutes_to_empty == 180 && st.minutes_to_full == -1);
	CHECK(!st.critical && !st.warn);
	CHECK(b.rs_reads > 0);                  /* repeated starts used */
	CHECK(b.long_writes == 0 && b.clear_on_read == 0 && b.writes == 0);
	CHECK(h.errors == 0);
}

static void
t_charging(void)
{
	struct max77705_raw raw;
	struct battery_state st;
	uint32_t low = 0;
	model();
	b.chg[CHG_INT_OK] = 0x5f;               /* CHGIN_OK */
	b.chg[CHG_DETAILS_00] = 0x60;           /* CHGIN valid */
	b.chg[CHG_DETAILS_01] = 0x31;           /* fast charge CC */
	b.fg[FG_CURRENT] = b.fg[FG_AVGCURRENT] = 3200;      /* +1000 mA */
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(st.external && st.charge_capable && st.charging && !st.full);
	CHECK(st.ma == 1000 && st.minutes_to_full == 84);   /* (2900 - 1500) * 60 / 1000 */
	CHECK(st.chg_dtls == 1 && st.bat_dtls == 3 && st.chgin_dtls == 3);
	b.chg[CHG_DETAILS_01] = 0x34;           /* done */
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(st.full && !st.charging);
	b.chg[CHG_DETAILS_01] = 0x3b;           /* charger watchdog expired */
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(st.charger_wdt_expired && !st.charging && st.external);
	b.chg[CHG_INT_OK] = 0x2f;               /* wireless */
	b.chg[CHG_DETAILS_01] = 0x32;
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(st.wireless && st.external && st.charge_capable && st.charging);
	/* charger absent: the current's sign */
	b.chg_present = false;
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(st.measured && st.charging && st.external && st.chg_dtls == -1);
	CHECK(b.clear_on_read == 0 && b.long_writes == 0 && b.writes == 0);
}

static void
t_fallbacks(void)
{
	struct max77705_raw raw;
	struct battery_state st;
	uint32_t low = 0;
	model();
	b.fg_present = false;
	CHECK(max77705_sample(&bus, &raw) != 0);
	max77705_state(&raw, &st, &low);
	CHECK(!st.measured && st.percent == 100 && st.external && st.mv == 4200);
	model();
	b.fg[FG_VCELL] = 0;                     /* a torn / reset read: never 0 % */
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(!st.measured && st.percent == 100);
	model();
	b.fg[FG_REPSOC] = 0xff00;
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(!st.measured);
	model();
	b.fg[FG_DESIGNCAP] = 0x0b96;            /* jig reset: model not loaded */
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(!st.measured && strstr(st.why, "model") != NULL);
	model();
	b.fg[FG_INIT_RESULT] = 0xff00;
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(!st.measured);
	model();
	b.fg[FG_STATUS] = 0x0008;               /* battery absent */
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(!st.measured);
	/* a NACK now and then is retried */
	model();
	b.nack_next = 2;
	CHECK(max77705_sample(&bus, &raw) == 0);
	max77705_state(&raw, &st, &low);
	CHECK(st.measured && st.percent == 51);
}

static void
t_critical(void)
{
	struct max77705_raw raw;
	struct battery_state st;
	uint32_t low = 0;
	model();
	b.fg[FG_REPSOC] = 0x0100;               /* 1 % */
	b.fg[FG_VCELL] = 43520;                 /* 3400 mV */
	for (unsigned i = 0; i < 2; i++) {
		max77705_sample(&bus, &raw);
		max77705_state(&raw, &st, &low);
		CHECK(!st.critical && st.warn);
	}
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(st.critical);
	b.fg[FG_VCELL] = 49280;                 /* one good sample resets the count */
	max77705_sample(&bus, &raw);
	max77705_state(&raw, &st, &low);
	CHECK(!st.critical && low == 0);
	/* on a charger never critical */
	b.fg[FG_VCELL] = 43520;
	b.chg[CHG_INT_OK] = 0x40;
	for (unsigned i = 0; i < 5; i++) {
		max77705_sample(&bus, &raw);
		max77705_state(&raw, &st, &low);
	}
	CHECK(!st.critical);
}

static void
t_percent_moves(void)
{
	/* the M11 question: does the published value follow the gauge under load? */
	struct max77705_raw raw;
	struct battery_state st;
	uint32_t low = 0;
	model();
	int32_t prev = -1;
	unsigned moved = 0;
	for (uint16_t s = 0x3280; s > 0x3200; s -= 0x0a) {
		b.fg[FG_REPSOC] = s;
		max77705_sample(&bus, &raw);
		max77705_state(&raw, &st, &low);
		if (prev >= 0 && st.soc_x10 != prev) {
			moved++;
		}
		prev = st.soc_x10;
	}
	CHECK(moved >= 3);
}

static void
t_bus_quirks(void)
{
	struct max77705_raw raw;
	struct battery_state st;
	uint32_t low = 0;
	/* the controller fails repeated starts: the STOP fallback reads the same values */
	model();
	b.rs_broken = true;
	CHECK(max77705_sample(&bus, &raw) == 0);
	max77705_state(&raw, &st, &low);
	CHECK(st.measured && st.percent == 51 && st.mv == 3850);
	CHECK(h.fallbacks > 0);
	/* no TRANSFER_DONE for a no-STOP write: the TX-empty settle path */
	model();
	b.no_done_without_stop = true;
	CHECK(max77705_sample(&bus, &raw) == 0);
	CHECK(h.fallbacks == 0 && b.rs_reads > 0);
	CHECK(b.long_writes == 0 && b.clear_on_read == 0);
	/* bad arguments */
	uint8_t x;
	CHECK(hsi2c_msg(&h, 0x80, true, &x, 1, true) != 0);
	CHECK(hsi2c_msg(&h, 0x36, true, &x, 0, true) != 0);
}

static void
charging_model(void)
{
	model();
	b.chg[CHG_INT_OK] = 0x5f;
	b.chg[CHG_DETAILS_00] = 0x60;
	b.chg[CHG_DETAILS_01] = 0x31;
	b.fg[FG_CURRENT] = b.fg[FG_AVGCURRENT] = 3200;
}

/* minutes on the cable, a sample every 30 s, the clear every kick_s (0 = never); returns the first expired sample
 * (or 0), counts the samples that were charging */
static unsigned
run_cable(unsigned minutes, unsigned kick_s, unsigned *charging)
{
	struct max77705_raw raw;
	struct battery_state st;
	uint32_t low = 0;
	uint64_t next_kick = 0;
	unsigned first_bad = 0;
	*charging = 0;
	for (unsigned i = 1; i <= minutes * 2; i++) {
		if (kick_s && b.now >= next_kick) {
			CHECK(max77705_wdt_kick(&bus, NULL) == MAX77705_KICK_OK);
			next_kick = b.now + (uint64_t)kick_s * 1000000u;
		}
		b.now += 30000000u;
		max77705_sample(&bus, &raw);
		max77705_state(&raw, &st, &low);
		*charging += st.charging;
		if (st.charger_wdt_expired && !first_bad) {
			first_bad = i;
		}
	}
	return first_bad;
}

static void
t_charger_watchdog(void)
{
	unsigned charging = 0, bad;
	/* without the clear: the watchdog expires (the old cut-off) and charging stops */
	charging_model();
	bad = run_cable(10, 0, &charging);
	CHECK(bad == 3);                        /* expired at the third 30 s sample (> 80 s) */
	CHECK(charging == 2);
	CHECK(b.writes == 0);
	/* with the clear every 30 s: 20 minutes on the cable, never expired, charging throughout */
	charging_model();
	bad = run_cable(20, MAX77705_WDT_KICK_S, &charging);
	CHECK(bad == 0 && charging == 40);
	CHECK(b.writes == 40 && b.bad_writes == 0 && b.long_writes == 0 && b.clear_on_read == 0);
	CHECK(b.last_write == 0x0d);            /* CHGPROT (0x0c) kept, WDTCLR = 01 */
	/* the mock is meaningful: a clear every 90 s is too slow for its 80 s */
	charging_model();
	bad = run_cable(10, 90, &charging);
	CHECK(bad != 0);
	/* CHGPROT kept as read, never more than [3:0] */
	charging_model();
	uint8_t w = 0;
	b.chg[CHG_CNFG_06] = 0x00;
	CHECK(max77705_wdt_kick(&bus, &w) == MAX77705_KICK_OK && w == 0x01 && b.last_write == 0x01);
	b.chg[CHG_CNFG_06] = 0xf7;
	CHECK(max77705_wdt_kick(&bus, &w) == MAX77705_KICK_OK && w == 0x05);
	CHECK(b.bad_writes == 0);
	/* WDTEN clear: nothing written */
	charging_model();
	b.chg[CHG_CNFG_00] = 0x05;
	CHECK(max77705_wdt_kick(&bus, &w) == MAX77705_KICK_OFF && b.writes == 0);
	/* read-only bus: refused */
	charging_model();
	CHECK(max77705_wdt_kick(&bus_ro, &w) == MAX77705_KICK_EREFUSED && b.writes == 0);
	/* charger absent: read error, nothing written */
	charging_model();
	b.chg_present = false;
	CHECK(max77705_wdt_kick(&bus, &w) == MAX77705_KICK_EREAD && b.writes == 0);
	/* the HSI2C layer refuses every other register / address without bus traffic */
	charging_model();
	unsigned m0 = b.msgs;
	CHECK(hsi2c_write_reg(&h, MAX77705_CHG_ADDR, CHG_CNFG_00, 0x05) != 0);
	CHECK(hsi2c_write_reg(&h, MAX77705_CHG_ADDR, CHG_INT, 0x00) != 0);
	CHECK(hsi2c_write_reg(&h, MAX77705_FG_ADDR, CHG_CNFG_06, 0x01) != 0);
	CHECK(hsi2c_write_reg(&h, MAX77705_PMIC_ADDR, 0x50, 0x00) != 0);
	CHECK(b.msgs == m0 && b.writes == 0);
	/* the policy, exhaustively: one register, one value per current content */
	static const uint8_t addrs[] = { 0x25, 0x36, 0x62, 0x66, 0x69 };
	unsigned allowed = 0, wrong = 0;
	for (unsigned a = 0; a < sizeof(addrs); a++) {
		for (unsigned r = 0; r < 256; r++) {
			for (unsigned v = 0; v < 256; v++) {
				for (unsigned c = 0; c < 256; c += 5) {
					if (max77705_write_allowed(addrs[a], (uint8_t)r, (uint8_t)v, (uint8_t)c)) {
						allowed++;
						wrong += !(addrs[a] == 0x69 && r == 0xbd && v == ((c & 0x0cu) | 1u));
					}
				}
			}
		}
	}
	CHECK(allowed == 52 && wrong == 0);     /* one value for each of the 52 sampled current contents */
}

int
main(void)
{
	t_conversions();
	t_read_allowed();
	t_timing();
	t_discharging();
	t_charging();
	t_fallbacks();
	t_critical();
	t_percent_moves();
	t_bus_quirks();
	t_charger_watchdog();
	printf("StarbiteBattery host tests: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
