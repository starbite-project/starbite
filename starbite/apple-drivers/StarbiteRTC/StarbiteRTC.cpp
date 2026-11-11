/* SPDX-License-Identifier: APSL-2.0 */
/* StarbiteRTC (docs/APPLE-S9-DRIVERS.md Part 3 row 8): see StarbiteRTC.h, hw/s2mps18_rtc.c, NOTES.md. */
#include "StarbiteRTC.h"
#include "../StarbiteACPM/StarbiteACPM.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#if defined(ARM64_BOARD_CONFIG_STARLTE)
extern "C" int starlte_sbstop_at(const char *stage);
extern "C" void starlte_sbstop_now(const char *stage);
#endif

#define super IORTC
OSDefineMetaClassAndStructors(StarbiteRTC, IORTC);

static StarbiteRTC *gStarbiteRTC;

StarbiteRTC *
StarbiteRTC::shared(void)
{
	return gStarbiteRTC;
}

int
StarbiteRTC::opRd(void *ctx, uint8_t reg, uint8_t *v)
{
	StarbiteRTC *me = (StarbiteRTC *)ctx;
	return me->fACPM->pmicRead(ACPM_PMIC_RTC, reg, v) == kIOReturnSuccess ? 0 : -1;
}

/* Policy-gated in StarbiteACPM: only UPDATE (+RUDR / +WUDR) and SEC..YEAR are writable. */
int
StarbiteRTC::opWr(void *ctx, uint8_t reg, uint8_t v)
{
	StarbiteRTC *me = (StarbiteRTC *)ctx;
	return me->fACPM->pmicWrite(ACPM_PMIC_RTC, reg, v) == kIOReturnSuccess ? 0 : -1;
}

void
StarbiteRTC::opDelay(void *, uint32_t us)
{
	IODelay(us);
}

static void
fmt_date(uint64_t t, char *out, size_t n)
{
	struct s2m_tm tm;
	s2m_unix_to_tm(t, &tm);
	snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u UTC", tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec);
}

static uint64_t
uptime_s(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_SEC;
}

int
StarbiteRTC::readNow(uint64_t *secs, const char **why)
{
	uint8_t r[S2M_RTC_NREGS];
	int e = s2m_rtc_read(&fOps, secs, r, why);
	if (e == S2M_E_IO) {
		fReadFails++;
	}
	return e;
}

bool
StarbiteRTC::start(IOService *provider)
{
	uint32_t on = 1, wr = 0, setTo = 0;
	char date[40];
	uint8_t r[S2M_RTC_NREGS] = { 0 };
	uint64_t t = 0;
	const char *why = "?";

	if (PE_parse_boot_argn("sbrtc", &on, sizeof(on)) && on == 0) {
		IOLog("StarbiteRTC: sbrtc=0: not started\n");
		return false;
	}
	fACPM = OSDynamicCast(StarbiteACPM, provider);
	if (fACPM == NULL || !super::start(provider)) {
		return false;
	}
	fOps.ctx = this;
	fOps.rd = opRd;
	fOps.wr = opWr;
	fOps.delay_us = opDelay;
	(void)PE_parse_boot_argn("sbrtcwrite", &wr, sizeof(wr));
	fWrite = wr != 0;

	int e = s2m_rtc_read(&fOps, &t, r, &why);
	IOLog("StarbiteRTC: S2MPS18 RTC CTRL %#x UPDATE %#x CAPSEL %#x MSEC %#x SEC %#x MIN %#x HOUR %#x WEEK %#x "
	    "DAY %#x MONTH %#x YEAR %#x\n", r[0], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
	fValid = e == S2M_OK;
	if (fValid) {
		fmt_date(t, date, sizeof(date));
		IOLog("StarbiteRTC: clock %s (%llu), %s; writes %s\n", date, t, why, fWrite ? "on" : "off (sbrtcwrite=1)");
	} else {
		IOLog("StarbiteRTC: clock not used: %s (%s); writes %s\n", s2m_strerror(e), why,
		    fWrite ? "on" : "off (sbrtcwrite=1)");
	}
	setProperty("RTCValid", fValid);
	setProperty("RTCWrites", fWrite);

	if (PE_parse_boot_argn("sbrtcset", &setTo, sizeof(setTo)) && setTo != 0) {
		uint64_t want = (uint64_t)setTo + uptime_s();
		if (!fWrite) {
			IOLog("StarbiteRTC: sbrtcset=%u ignored: needs sbrtcwrite=1\n", setTo);
		} else {
			e = s2m_rtc_write(&fOps, want, &why);
			uint64_t back = 0;
			const char *rwhy = "?";
			int re = s2m_rtc_read(&fOps, &back, r, &rwhy);
			fmt_date(want, date, sizeof(date));
			IOLog("StarbiteRTC: sbrtcset: wrote %s (%llu): %s (%s); reads back %llu (%s)\n", date, want,
			    s2m_strerror(e), why, re == S2M_OK ? back : 0ULL, rwhy);
			fValid = re == S2M_OK;
		}
	}
	gStarbiteRTC = this;
	registerService();
	publishResource("IORTC", this);
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (starlte_sbstop_at("rtc")) {
		starlte_sbstop_now("rtc");
	}
#endif
	return true;
}

void
StarbiteRTC::stop(IOService *provider)
{
	if (gStarbiteRTC == this) {
		gStarbiteRTC = NULL;
	}
	super::stop(provider);
}

/* 0 when the clock cannot be read or was never set: the calendar then starts as it does today (card clock). */
long
StarbiteRTC::getGMTTimeOfDay(void)
{
	uint64_t t = 0;
	const char *why = "?";
	int e = readNow(&t, &why);
	if (e != S2M_OK) {
		static uint32_t said;
		if (said++ < 4) {
			IOLog("StarbiteRTC: getGMTTimeOfDay: %s (%s): 0\n", s2m_strerror(e), why);
		}
		return 0;
	}
	return (long)t;
}

void
StarbiteRTC::setGMTTimeOfDay(long secs)
{
	const char *why = "?";
	char date[40];
	if (!fWrite || secs <= 0) {
		return;
	}
	int e = s2m_rtc_write(&fOps, (uint64_t)secs, &why);
	fmt_date((uint64_t)secs, date, sizeof(date));
	if (e == S2M_OK) {
		if (fWrites++ < 4) {
			IOLog("StarbiteRTC: set to %s (%ld)\n", date, secs);
		}
		fValid = true;
	} else if (fWriteFails++ < 8) {
		IOLog("StarbiteRTC: set to %s FAILED: %s (%s)\n", date, s2m_strerror(e), why);
	}
}

void
StarbiteRTC::setAlarmEnable(IOOptionBits)
{
	/* no alarms: the alarm registers (0x0c..) are never written */
}
