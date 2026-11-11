/* SPDX-License-Identifier: APSL-2.0 */
/*
 * MAX77705 fuel gauge (I2C 0x36) and charger (0x69) on the S9, plain C. Read only, except the one allowed write:
 * the charger watchdog clear (CNFG_06 WDTCLR = 01, max77705_wdt_kick). Register facts with sources:
 * ../NOTES.md (build/em-research-battery.md, the vendor DTB). Fixed-point conversions for the S9's
 * fg_resistor = 2 (5 mOhm sense):
 *   VCELL / AvgVCELL      mV  = raw * 78.125 uV          = raw * 5 / 64
 *   Current / AvgCurrent  mA  = s16 * 0.3125 (+ = into the battery)  = s * 5 / 16
 *   RepSOC                %   = raw / 256 (0.1 %: hi * 10 + lo * 10 / 256)
 *   RepCap / FullCapRep / DesignCap  mAh = raw (fg_resistor / 2 = 1)
 *   Temperature           C   = s16 / 256 (host-written on the S9: S-Boot's ADC reading, stale)
 *   Cycles                cycles = raw / 100 [inferred: ModelGauge m5 1 % LSB; not confirmed for the MAX77705]
 */
#ifndef STARBITE_MAX77705_H
#define STARBITE_MAX77705_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX77705_PMIC_ADDR      0x66u
#define MAX77705_FG_ADDR        0x36u
#define MAX77705_CHG_ADDR       0x69u

/* fuel gauge (16 bit, little endian) */
#define FG_STATUS               0x00u
#define FG_REPCAP               0x05u
#define FG_REPSOC               0x06u
#define FG_TEMP                 0x08u
#define FG_VCELL                0x09u
#define FG_CURRENT              0x0au
#define FG_AVGCURRENT           0x0bu
#define FG_FULLCAP              0x10u
#define FG_CYCLES               0x17u
#define FG_DESIGNCAP            0x18u
#define FG_AVGVCELL             0x19u
#define FG_FULLCAPREP           0x35u
#define FG_INIT_RESULT          0xb2u   /* hi byte 0xff: S-Boot's model load failed */

/* charger (8 bit) */
#define CHG_INT                 0xb0u   /* clear on read: NEVER read */
#define CHG_INT_OK              0xb2u
#define CHG_DETAILS_00          0xb3u
#define CHG_DETAILS_01          0xb4u
#define CHG_DETAILS_02          0xb5u
#define CHG_CNFG_00             0xb7u

#define CHG_CNFG_06             0xbdu   /* [3:2] CHGPROT, [1:0] WDTCLR: the ONE register ever written */
#define CHG_CNFG_00_WDTEN       0x10u
#define CHG_CNFG_06_CHGPROT     0x0cu
#define CHG_CNFG_06_WDTCLR_KICK 0x01u
#define MAX77705_WDT_KICK_S     30u     /* clear interval (s): Engineer OS's feed; Linux clears on every health poll */

#define CHG_INT_OK_WCIN         0x20u
#define CHG_INT_OK_CHGIN        0x40u

/* S9 calibration (device/dtb/starlte.dts battery / fuelgauge nodes) */
#define S9_FG_RESISTOR          2u      /* fuelgauge,fg_resistor */
#define S9_DESIGN_RAW           0x0b97u /* fuelgauge,capacity: S-Boot's "initialized" marker, 2967 mAh */
#define S9_CAPACITY_MAX         1000u   /* fuelgauge,capacity_max (0.1 %): no rescaling */
#define S9_FULL_CAPACITY_MAH    3000u   /* battery,battery_full_capacity */
#define S9_FLOAT_MV             4350u   /* battery,chg_float_voltage */

/* Is this (addr, reg) safe to read? (no clear-on-read interrupt registers; no debug slave) */
bool max77705_read_allowed(uint8_t addr7, uint8_t reg);

/*
 * The only write the driver may make (your decision, 2026-11-11 19:12): the charger watchdog clear, CNFG_06
 * WDTCLR = 01 with CHGPROT kept as read, i.e. value == (cur & 0x0c) | 0x01 at 0x69:0xbd. Everything else: false.
 */
bool max77705_write_allowed(uint8_t addr7, uint8_t reg, uint8_t value, uint8_t cur);

/* fixed-point conversions */
static inline int32_t max77705_mv(uint16_t raw) { return (int32_t)((uint32_t)raw * 5u / 64u); }
static inline int32_t max77705_ma(uint16_t raw) { return (int32_t)(int16_t)raw * 5 / 16; }
static inline int32_t max77705_soc_x10(uint16_t raw) { return (int32_t)(raw >> 8) * 10 + (int32_t)((raw & 0xffu) * 10u / 256u); }
static inline int32_t max77705_mah(uint16_t raw) { return (int32_t)((uint32_t)raw * S9_FG_RESISTOR / 2u); }
static inline int32_t max77705_temp_c10(uint16_t raw) { return (int32_t)(int16_t)raw * 10 / 256; }
static inline uint32_t max77705_cycles(uint16_t raw) { return raw / 100u; }

/* A register reader: 0 = ok. On the phone hsi2c_read_reg, in the tests a model. */
struct max77705_bus {
	void *ctx;
	int (*read)(void *ctx, uint8_t addr7, uint8_t reg, uint8_t *buf, unsigned len);
	int (*write)(void *ctx, uint8_t addr7, uint8_t reg, uint8_t value);     /* may be NULL: read only */
};

/* The charger watchdog clear. Reads CNFG_00 (WDTEN) and CNFG_06; writes (CNFG_06 & 0x0c) | 0x01 when the watchdog
 * is enabled. Results: */
#define MAX77705_KICK_OK        0
#define MAX77705_KICK_OFF       1       /* WDTEN clear: nothing to do, nothing written */
#define MAX77705_KICK_EREAD     (-1)
#define MAX77705_KICK_EWRITE    (-2)
#define MAX77705_KICK_EREFUSED  (-3)
int max77705_wdt_kick(const struct max77705_bus *bus, uint8_t *written);

struct max77705_raw {
	uint16_t repsoc, vcell, avgvcell, current, avgcurrent, temp, repcap, fullcaprep, designcap, cycles, status,
	    init_result;
	uint8_t int_ok, dtls00, dtls01, dtls02, cnfg00;
	bool fg_ok, chg_ok;
	uint32_t fg_retries;
};

/* What the IOPMPowerSource publishes. */
struct battery_state {
	bool measured;                  /* false: the safe static fallback (100 %, on AC), never 0 % */
	const char *why;                /* why not measured / what is estimated */
	uint32_t percent;               /* 0..100 */
	int32_t soc_x10;
	uint32_t mv, avg_mv;
	int32_t ma, avg_ma;             /* + = charging */
	int32_t temp_c10;
	uint32_t cur_mah, max_mah, design_mah;
	uint32_t cycles;
	bool external, wireless, charge_capable, charging, full, present;
	bool charger_wdt_expired;       /* CHG_DTLS 0x0b: S-Boot's charger watchdog ran out (nobody kicks it) */
	int32_t chg_dtls, bat_dtls, chgin_dtls;
	int32_t minutes_to_empty, minutes_to_full;      /* -1 unknown */
	bool critical, warn;
};

/* Read one sample (fuel gauge words checked against plausible ranges, retried; charger bytes). 0 = gauge ok. */
int max77705_sample(const struct max77705_bus *bus, struct max77705_raw *raw);

/* Turn a sample into the published state. low_count: consecutive low samples so far (in/out), for the critical
 * flag, which needs three in a row (one bad sample must never shut the phone down). */
void max77705_state(const struct max77705_raw *raw, struct battery_state *st, uint32_t *low_count);

/* The fallback SweetBattery has always published: 100 %, 4200 mV, on AC, not charging. */
void battery_fallback(struct battery_state *st, const char *why);

#ifdef __cplusplus
}
#endif
#endif
