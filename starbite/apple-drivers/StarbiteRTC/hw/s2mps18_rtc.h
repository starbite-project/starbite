/* SPDX-License-Identifier: APSL-2.0 */
/*
 * The S2MPS18 RTC block (ACPM PMIC type 2), plain C. Register facts: ../NOTES.md.
 *   0x00 CTRL       bit 0 BCD_EN (0 = binary), bit 1 MODEL24 (1 = 24 h)
 *   0x01 WTSR_SMPL  never touched
 *   0x02 UPDATE     bit 0 RUDR (latch the counters into 0x04..0x0b), bit 1 WUDR (move 0x05..0x0b into the
 *                   counters), bit 2 FREEZE, bit 4 AUDR (alarms): only RUDR and WUDR are ever set here
 *   0x03 CAPSEL     [7:4] = 0xF once the clock has been set up (Linux's test); never written
 *   0x04 MSEC       0x05 SEC  0x06 MIN  0x07 HOUR (bit 6 PM)  0x08 WEEK (one bit a day)
 *   0x09 DAY        0x0a MONTH (1-12)   0x0b YEAR (since 2000)
 * All access goes through struct s2m_rtc_ops (StarbiteACPM on the phone, a model in the tests).
 */
#ifndef STARBITE_S2MPS18_RTC_H
#define STARBITE_S2MPS18_RTC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2M_RTC_CTRL            0x00u
#define S2M_RTC_UPDATE          0x02u
#define S2M_RTC_CAPSEL          0x03u
#define S2M_RTC_MSEC            0x04u
#define S2M_RTC_SEC             0x05u
#define S2M_RTC_MIN             0x06u
#define S2M_RTC_HOUR            0x07u
#define S2M_RTC_WEEK            0x08u
#define S2M_RTC_DAY             0x09u
#define S2M_RTC_MONTH           0x0au
#define S2M_RTC_YEAR            0x0bu
#define S2M_RTC_NREGS           12u

#define S2M_CTRL_BCD            0x01u
#define S2M_CTRL_24H            0x02u
#define S2M_UPDATE_RUDR         0x01u
#define S2M_UPDATE_WUDR         0x02u
#define S2M_HOUR_PM             0x40u

/* trusted years; outside: "never set" (the phone read 2018-01-01 when nothing had set it) */
#define S2M_YEAR_MIN            2024u
#define S2M_YEAR_MAX            2099u

/* latch / write-update waits */
#define S2M_UPDATE_POLLS        50u
#define S2M_UPDATE_POLL_US      1000u

struct s2m_rtc_ops {
	void *ctx;
	int (*rd)(void *ctx, uint8_t reg, uint8_t *v);  /* 0 = ok */
	int (*wr)(void *ctx, uint8_t reg, uint8_t v);
	void (*delay_us)(void *ctx, uint32_t us);
};

struct s2m_tm {
	uint32_t year;          /* 2000.. */
	uint32_t month;         /* 1..12 */
	uint32_t day;           /* 1..31 */
	uint32_t hour, min, sec;
	uint32_t wday;          /* 0 = Sunday */
};

/* results */
#define S2M_OK                  0
#define S2M_E_IO                (-1)    /* an ACPM read/write failed */
#define S2M_E_NOTSET            (-2)    /* CTRL / CAPSEL say the clock was reset, or the year is not plausible */
#define S2M_E_RANGE             (-3)    /* a field out of range */
#define S2M_E_VERIFY            (-4)    /* the read-back after a write disagrees */
#define S2M_E_MODE              (-5)    /* not 24-hour mode: we never write CTRL, so no writes */

/* Days-from-civil (proleptic Gregorian), Unix seconds of a date from 1970 on; the inverse. */
uint64_t s2m_tm_to_unix(const struct s2m_tm *tm);
void s2m_unix_to_tm(uint64_t t, struct s2m_tm *tm);

/* Decode registers 0x00..0x0b (r[0] = CTRL). Fields decoded per CTRL's BCD and 24 h bits. */
int s2m_rtc_decode(const uint8_t r[S2M_RTC_NREGS], struct s2m_tm *tm, const char **why);

/* Encode SEC..YEAR (7 bytes for 0x05..0x0b) in the coding CTRL says. 24-hour mode only. */
int s2m_rtc_encode(const struct s2m_tm *tm, uint8_t ctrl, uint8_t out[7]);

/* Latch (RUDR, wait for it to clear) and read 0x00..0x0b. latched: whether RUDR cleared in time. */
int s2m_rtc_read_raw(const struct s2m_rtc_ops *ops, uint8_t r[S2M_RTC_NREGS], bool *latched);

/* Read and decode: Unix seconds, or an error (why says which check). */
int s2m_rtc_read(const struct s2m_rtc_ops *ops, uint64_t *unix_secs, uint8_t r[S2M_RTC_NREGS], const char **why);

/* Set the clock: SEC..YEAR written, then WUDR, its clearing waited for, then read back (within 2 s). */
int s2m_rtc_write(const struct s2m_rtc_ops *ops, uint64_t unix_secs, const char **why);

const char *s2m_strerror(int e);

#ifdef __cplusplus
}
#endif
#endif
