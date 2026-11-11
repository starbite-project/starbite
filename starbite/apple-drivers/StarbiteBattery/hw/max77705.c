/* SPDX-License-Identifier: APSL-2.0 */
/* MAX77705 fuel gauge and charger, read only (see max77705.h and ../NOTES.md). */
#include "max77705.h"

bool
max77705_read_allowed(uint8_t addr7, uint8_t reg)
{
	switch (addr7) {
	case MAX77705_FG_ADDR:
		return true;
	case MAX77705_CHG_ADDR:
		return reg != CHG_INT;                  /* CHG_INT clears on read */
	case MAX77705_PMIC_ADDR:
		return reg != 0x22u && reg != 0x24u;    /* INTSRC, SYSTEM_INT clear on read */
	case 0x25u:                                     /* USB-C: UIC/CC/PD/VDM_INT clear on read */
		return reg < 0x02u || reg > 0x05u;
	default:
		return false;                           /* 0x62 debug and anything else */
	}
}

bool
max77705_write_allowed(uint8_t addr7, uint8_t reg, uint8_t value, uint8_t cur)
{
	return addr7 == MAX77705_CHG_ADDR && reg == CHG_CNFG_06 &&
	    value == (uint8_t)((cur & CHG_CNFG_06_CHGPROT) | CHG_CNFG_06_WDTCLR_KICK);
}

static int
rd(const struct max77705_bus *bus, uint8_t addr7, uint8_t reg, uint8_t *buf, unsigned len)
{
	if (!max77705_read_allowed(addr7, reg)) {
		return -1;
	}
	return bus->read(bus->ctx, addr7, reg, buf, len);
}

/* A gauge word, up to three reads; with lo < hi a value outside [lo, hi] is read again, then rejected. */
static bool
fg_word(const struct max77705_bus *bus, uint8_t reg, uint16_t *v, uint32_t lo, uint32_t hi, uint32_t *retries)
{
	for (unsigned a = 0; a < 3; a++) {
		uint8_t b[2];
		if (a) {
			(*retries)++;
		}
		if (rd(bus, MAX77705_FG_ADDR, reg, b, 2) != 0) {
			continue;
		}
		uint16_t w = (uint16_t)(b[0] | (uint32_t)b[1] << 8);
		if (lo < hi && (w < lo || w > hi)) {
			continue;
		}
		*v = w;
		return true;
	}
	return false;
}

int
max77705_sample(const struct max77705_bus *bus, struct max77705_raw *raw)
{
	uint32_t *rt = &raw->fg_retries;
	raw->fg_retries = 0;
	/* RepSOC <= 101 %, VCELL 2.5..4.7 V: a failed or impossible read never becomes 0 % / 0 mV */
	raw->fg_ok = fg_word(bus, FG_REPSOC, &raw->repsoc, 0, 0x6500, rt) &&
	    fg_word(bus, FG_VCELL, &raw->vcell, 32000, 60160, rt);
	if (raw->fg_ok) {
		if (!fg_word(bus, FG_AVGVCELL, &raw->avgvcell, 32000, 60160, rt)) {
			raw->avgvcell = raw->vcell;
		}
		if (!fg_word(bus, FG_CURRENT, &raw->current, 0, 0, rt)) {
			raw->current = 0;
		}
		if (!fg_word(bus, FG_AVGCURRENT, &raw->avgcurrent, 0, 0, rt)) {
			raw->avgcurrent = raw->current;
		}
		raw->fg_ok = fg_word(bus, FG_DESIGNCAP, &raw->designcap, 0, 0, rt) &&
		    fg_word(bus, FG_STATUS, &raw->status, 0, 0, rt) &&
		    fg_word(bus, FG_INIT_RESULT, &raw->init_result, 0, 0, rt) &&
		    fg_word(bus, FG_REPCAP, &raw->repcap, 0, 6000, rt) &&
		    fg_word(bus, FG_FULLCAPREP, &raw->fullcaprep, 0, 6000, rt);
		if (!fg_word(bus, FG_TEMP, &raw->temp, 0, 0, rt)) {
			raw->temp = 25u << 8;
		}
		if (!fg_word(bus, FG_CYCLES, &raw->cycles, 0, 0, rt)) {
			raw->cycles = 0;
		}
	}
	raw->chg_ok = rd(bus, MAX77705_CHG_ADDR, CHG_INT_OK, &raw->int_ok, 1) == 0 &&
	    rd(bus, MAX77705_CHG_ADDR, CHG_DETAILS_00, &raw->dtls00, 1) == 0 &&
	    rd(bus, MAX77705_CHG_ADDR, CHG_DETAILS_01, &raw->dtls01, 1) == 0;
	if (raw->chg_ok) {
		(void)rd(bus, MAX77705_CHG_ADDR, CHG_DETAILS_02, &raw->dtls02, 1);
		(void)rd(bus, MAX77705_CHG_ADDR, CHG_CNFG_00, &raw->cnfg00, 1);
	}
	return raw->fg_ok ? 0 : -1;
}

void
battery_fallback(struct battery_state *st, const char *why)
{
	struct battery_state z = { 0 };
	*st = z;
	st->measured = false;
	st->why = why;
	st->percent = 100;
	st->soc_x10 = 1000;
	st->mv = st->avg_mv = 4200;
	st->temp_c10 = 250;
	st->cur_mah = st->max_mah = st->design_mah = S9_FULL_CAPACITY_MAH;
	st->external = st->charge_capable = true;
	st->full = true;
	st->present = true;
	st->chg_dtls = st->bat_dtls = st->chgin_dtls = -1;
	st->minutes_to_empty = st->minutes_to_full = -1;
}

void
max77705_state(const struct max77705_raw *raw, struct battery_state *st, uint32_t *low_count)
{
	if (!raw->fg_ok) {
		battery_fallback(st, "fuel gauge not answering (or an implausible RepSOC / VCELL)");
		*low_count = 0;
		return;
	}
	if (raw->designcap != S9_DESIGN_RAW || (raw->init_result >> 8) == 0xffu) {
		battery_fallback(st, "fuel gauge model not loaded (DesignCap != 0x0b97 or init result 0xff)");
		*low_count = 0;
		return;
	}
	if (raw->status & 0x0008u) {
		battery_fallback(st, "fuel gauge says no battery (Status.Bst)");
		*low_count = 0;
		return;
	}
	struct battery_state z = { 0 };
	*st = z;
	st->measured = true;
	st->why = raw->chg_ok ? "measured; temperature is S-Boot's last write (not live)" :
	    "measured; charger not answering: cable from the current's sign";
	st->present = true;
	st->soc_x10 = max77705_soc_x10(raw->repsoc) * 1000 / (int32_t)S9_CAPACITY_MAX;
	if (st->soc_x10 > 1000) {
		st->soc_x10 = 1000;
	}
	st->percent = (uint32_t)(st->soc_x10 + 5) / 10u;
	st->mv = (uint32_t)max77705_mv(raw->vcell);
	st->avg_mv = (uint32_t)max77705_mv(raw->avgvcell);
	st->ma = max77705_ma(raw->current);
	st->avg_ma = max77705_ma(raw->avgcurrent);
	st->temp_c10 = max77705_temp_c10(raw->temp);
	st->cur_mah = (uint32_t)max77705_mah(raw->repcap);
	st->max_mah = (uint32_t)max77705_mah(raw->fullcaprep);
	st->design_mah = (uint32_t)max77705_mah(raw->designcap);
	st->cycles = max77705_cycles(raw->cycles);
	if (raw->chg_ok) {
		st->wireless = (raw->int_ok & CHG_INT_OK_WCIN) != 0;
		st->external = st->wireless || (raw->int_ok & CHG_INT_OK_CHGIN) != 0;
		st->chgin_dtls = (raw->dtls00 >> 5) & 3;
		st->chg_dtls = raw->dtls01 & 0xf;
		st->bat_dtls = (raw->dtls01 >> 4) & 7;
		st->charge_capable = st->external && (st->wireless || st->chgin_dtls == 3);
		st->charging = st->external && st->chg_dtls <= 2;
		st->full = st->external && (st->chg_dtls == 3 || st->chg_dtls == 4);
		st->charger_wdt_expired = st->chg_dtls == 0x0b;
	} else {
		st->chg_dtls = st->bat_dtls = st->chgin_dtls = -1;
		st->charging = st->external = st->charge_capable = st->avg_ma > 20;
	}
	st->minutes_to_empty = st->minutes_to_full = -1;
	if (st->avg_ma < -20) {
		st->minutes_to_empty = (int32_t)(st->cur_mah * 60u / (uint32_t)(-st->avg_ma));
	} else if (st->charging && st->avg_ma > 20 && st->max_mah > st->cur_mah) {
		st->minutes_to_full = (int32_t)((st->max_mah - st->cur_mah) * 60u / (uint32_t)st->avg_ma);
	}
	/* low: three samples in a row at <= 2 % and <= 3.45 V on battery before AtCriticalLevel */
	bool low = !st->external && st->soc_x10 <= 20 && st->mv <= 3450;
	*low_count = low ? *low_count + 1 : 0;
	st->critical = *low_count >= 3;
	st->warn = !st->external && st->soc_x10 <= 100;
}

int
max77705_wdt_kick(const struct max77705_bus *bus, uint8_t *written)
{
	uint8_t cnfg00 = 0, cnfg06 = 0;
	if (bus->write == 0) {
		return MAX77705_KICK_EREFUSED;
	}
	if (rd(bus, MAX77705_CHG_ADDR, CHG_CNFG_00, &cnfg00, 1) != 0 ||
	    rd(bus, MAX77705_CHG_ADDR, CHG_CNFG_06, &cnfg06, 1) != 0) {
		return MAX77705_KICK_EREAD;
	}
	if (!(cnfg00 & CHG_CNFG_00_WDTEN)) {
		return MAX77705_KICK_OFF;
	}
	uint8_t v = (uint8_t)((cnfg06 & CHG_CNFG_06_CHGPROT) | CHG_CNFG_06_WDTCLR_KICK);
	if (!max77705_write_allowed(MAX77705_CHG_ADDR, CHG_CNFG_06, v, cnfg06)) {
		return MAX77705_KICK_EREFUSED;
	}
	if (bus->write(bus->ctx, MAX77705_CHG_ADDR, CHG_CNFG_06, v) != 0) {
		return MAX77705_KICK_EWRITE;
	}
	if (written) {
		*written = v;
	}
	return MAX77705_KICK_OK;
}
