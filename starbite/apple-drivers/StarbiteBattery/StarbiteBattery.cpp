/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteBattery (docs/APPLE-S9-DRIVERS.md Part 3 row 22): see StarbiteBattery.h, hw/, NOTES.md.
 * The published key set follows what SweetBattery (xnu/series-12377 0220/0221) publishes for powerd, written
 * anew here; capacities are in percent (MaxCapacity 100) as Apple's own batteries publish them, mAh in the
 * AppleRaw* keys. Every bus access happens on the work loop (timers), never in start's caller beyond the first.
 * One register write only: the MAX77705 charger watchdog clear (CNFG_06 WDTCLR = 01, allowed 2026-11-11).
 */
#include "StarbiteBattery.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

#if defined(ARM64_BOARD_CONFIG_STARLTE)
extern "C" int starlte_sbstop_at(const char *stage);
extern "C" void starlte_sbstop_now(const char *stage);
#endif

#define super IOPMPowerSource
OSDefineMetaClassAndStructors(StarbiteBattery, IOPMPowerSource);

/* ---- MMIO for hw/ ---- */

uint32_t
StarbiteBattery::ioRd(void *ctx, uint32_t off)
{
	StarbiteBattery *me = (StarbiteBattery *)ctx;
	uint32_t v = *(volatile uint32_t *)(me->fBase + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

void
StarbiteBattery::ioWr(void *ctx, uint32_t off, uint32_t v)
{
	StarbiteBattery *me = (StarbiteBattery *)ctx;
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(me->fBase + off) = v;
}

void
StarbiteBattery::ioDelay(void *, uint32_t us)
{
	IODelay(us);
}

int
StarbiteBattery::gaugeRead(void *ctx, uint8_t addr7, uint8_t reg, uint8_t *buf, unsigned len)
{
	StarbiteBattery *me = (StarbiteBattery *)ctx;
	return hsi2c_read_reg(&me->fBus, addr7, reg, buf, len);
}

/* The one write (0x69:0xbd, checked again in hsi2c_write_reg and max77705_wdt_kick). */
int
StarbiteBattery::gaugeWrite(void *ctx, uint8_t addr7, uint8_t reg, uint8_t value)
{
	StarbiteBattery *me = (StarbiteBattery *)ctx;
	return hsi2c_write_reg(&me->fBus, addr7, reg, value);
}

static uint64_t
uptime_s(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_SEC;
}

/* ---- the M11 load: one busy kernel thread per CPU until the deadline ---- */

static uint64_t gLoadUntil;     /* absolute time */
static volatile uint64_t gLoadSink;

static void
load_thread(void *, wait_result_t)
{
	uint64_t x = 0;
	while (mach_absolute_time() < gLoadUntil) {
		for (uint32_t i = 0; i < 100000; i++) {
			x = x * 6364136223846793005ULL + i;
		}
		gLoadSink = x;
	}
	thread_terminate(current_thread());
}

static void
load_start(uint32_t secs)
{
	unsigned n = ml_wait_max_cpus();
	uint64_t interval = 0;
	nanoseconds_to_absolutetime((uint64_t)secs * NSEC_PER_SEC, &interval);
	gLoadUntil = mach_absolute_time() + interval;
	for (unsigned i = 0; i < n; i++) {
		thread_t th;
		if (kernel_thread_start(load_thread, NULL, &th) == KERN_SUCCESS) {
			thread_deallocate(th);
		}
	}
	IOLog("StarbiteBattery: M11 load: %u busy threads for %u s\n", n, secs);
}

/* ---- init / properties ---- */

bool
StarbiteBattery::init(OSDictionary *dictionary)
{
	/* This XNU's IOPMPowerSource has only init(void): run it, then keep the personality's keys. */
	if (!super::init()) {
		return false;
	}
	if (dictionary) {
		OSCollectionIterator *it = OSCollectionIterator::withCollection(dictionary);
		if (it == NULL) {
			return false;
		}
		const OSSymbol *key;
		while ((key = OSDynamicCast(OSSymbol, it->getNextObject())) != NULL) {
			setProperty(key, dictionary->getObject(key));
		}
		it->release();
	}
	return true;
}

void
StarbiteBattery::putNumber(const char *key, int64_t value)
{
	const OSSymbol *k = OSSymbol::withCString(key);
	OSNumber *n = OSNumber::withNumber((unsigned long long)value, 64);
	if (k && n) {
		setPSProperty(k, n);
	}
	OSSafeReleaseNULL(n);
	OSSafeReleaseNULL(k);
}

void
StarbiteBattery::putBool(const char *key, bool value)
{
	const OSSymbol *k = OSSymbol::withCString(key);
	if (k) {
		setPSProperty(k, value ? kOSBooleanTrue : kOSBooleanFalse);
		k->release();
	}
}

void
StarbiteBattery::putString(const char *key, const char *value)
{
	const OSSymbol *k = OSSymbol::withCString(key);
	OSString *s = OSString::withCString(value ? value : "");
	if (k && s) {
		setPSProperty(k, s);
	}
	OSSafeReleaseNULL(s);
	OSSafeReleaseNULL(k);
}

void
StarbiteBattery::publish(const struct battery_state *st)
{
	setBatteryInstalled(st->present);
	setMaxCapacity(100);
	setCurrentCapacity(st->percent);
	setExternalConnected(st->external);
	setExternalChargeCapable(st->charge_capable);
	setIsCharging(st->charging);
	setAtWarnLevel(st->warn);
	setAtCriticalLevel(st->critical);
	setVoltage(st->mv);
	setAmperage(st->ma);
	setTimeRemaining(st->charging ? st->minutes_to_full : st->minutes_to_empty);
	setCycleCount(st->cycles);
	putBool("FullyCharged", st->full);
	putNumber("DesignCapacity", st->design_mah);
	putNumber("NominalChargeCapacity", st->max_mah);
	putNumber("AppleRawCurrentCapacity", st->cur_mah);
	putNumber("AppleRawMaxCapacity", st->max_mah);
	putBool("AppleRawExternalConnected", st->external);
	putNumber("AppleRawBatteryVoltage", st->mv);
	putNumber("InstantAmperage", st->ma);
	putNumber("AvgTimeToEmpty", st->minutes_to_empty >= 0 ? st->minutes_to_empty : 65535);
	putNumber("AvgTimeToFull", st->minutes_to_full >= 0 ? st->minutes_to_full : 65535);
	putNumber("Temperature", st->temp_c10 * 10);   /* centi-degrees C */
	putBool("StarbiteBatteryMeasured", st->measured);
	putString("StarbiteBatteryNote", st->why);
	putNumber("StarbiteBatterySOCx10", st->soc_x10);
	putBool("StarbiteBatteryTemperatureEstimated", true);
	putBool("ChargerWatchdogExpired", st->charger_wdt_expired);
	putNumber("ChargerDetails", st->chg_dtls);

	OSDictionary *legacy = OSDictionary::withCapacity(6);
	if (legacy) {
		const char *keys[] = { kIOBatteryCurrentChargeKey, kIOBatteryCapacityKey, kIOBatteryVoltageKey,
			               kIOBatteryAmperageKey, kIOBatteryCycleCountKey, kIOBatteryFlagsKey };
		UInt32 values[] = { st->cur_mah, st->max_mah, st->mv, (UInt32)st->ma, st->cycles,
			            (UInt32)(kIOBatteryInstalled | (st->external ? kIOBatteryChargerConnect : 0) |
			            (st->charging ? kIOBatteryCharge : 0)) };
		for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
			OSNumber *n = OSNumber::withNumber(values[i], 32);
			if (n) {
				legacy->setObject(keys[i], n);
				n->release();
			}
		}
		setLegacyIOBatteryInfo(legacy);
		legacy->release();
	}
	updateStatus();
}

/* ---- sampling ---- */

void
StarbiteBattery::poll(void)
{
	struct max77705_raw raw;
	struct battery_state st;

	bzero(&raw, sizeof(raw));
	if (fReal) {
		(void)max77705_sample(&fGauge, &raw);
		max77705_state(&raw, &st, &fLow);
	} else {
		battery_fallback(&st, "sbbattery=0: static");
	}
	fSamples++;
	bool modeChanged = fSamples == 1 || st.measured != (fLastMeasured != 0);
	fLastMeasured = st.measured;
	if (modeChanged || fTestLog || fSamples % 20 == 0) {
		IOLog("StarbiteBattery: t=%llu s %s: %d.%d %% (%u%%) %u mV %d mA (avg %d) %d.%d C %u/%u mAh cycles %u "
		    "ext %d chg %d full %d dtls %d/%d/%d%s; bus errors %u fallbacks %u; %s\n", uptime_s(),
		    st.measured ? "MAX77705" : "fallback", st.soc_x10 / 10, st.soc_x10 % 10, st.percent, st.mv, st.ma,
		    st.avg_ma, st.temp_c10 / 10, (st.temp_c10 < 0 ? -st.temp_c10 : st.temp_c10) % 10, st.cur_mah,
		    st.max_mah, st.cycles, st.external, st.charging, st.full, st.chg_dtls, st.bat_dtls, st.chgin_dtls,
		    st.charger_wdt_expired ? " CHARGER WATCHDOG EXPIRED" : "", fBus.errors, fBus.fallbacks, st.why);
	}
	fState = st;
	publish(&st);
}

/* PHONE-TEST.md M11: baseline 10 s, load fLoadS s, then the verdict. */
void
StarbiteBattery::testStep(uint64_t now_s)
{
	uint64_t t = now_s - fT0;
	if (fPhase == 2) {
		return;
	}
	fAllMeasured = fAllMeasured && fState.measured;
	if (fState.soc_x10 < fMinSoc) {
		fMinSoc = fState.soc_x10;
	}
	if (fState.soc_x10 > fMaxSoc) {
		fMaxSoc = fState.soc_x10;
	}
	if (fPhase == 0) {
		fBaseSoc = fState.soc_x10;
		fBaseMa = fState.ma;
		if (t >= 10) {
			fPhase = 1;
			fMinSoc = fMaxSoc = fState.soc_x10;
			fLoadMa = 0;
			load_start(fLoadS);
		}
		return;
	}
	if (fState.ma < fLoadMa) {
		fLoadMa = fState.ma;            /* the most negative: the heaviest discharge */
	}
	if (t < 10 + (uint64_t)fLoadS + 2) {
		return;
	}
	fPhase = 2;
	bool socMoved = fMinSoc != fMaxSoc || fState.soc_x10 != fBaseSoc;
	bool currentRose = fLoadMa <= fBaseMa - 200;
	bool pass = fAllMeasured && socMoved && currentRose;
	IOLog("StarbiteBattery: M11 %s: measured %s, SOC %d.%d %% -> %d.%d %% (range %d..%d x0.1 %%), current %d mA "
	    "idle -> %d mA under load (%s), percent shown %u%%\n", pass ? "PASS" : "FAIL", fAllMeasured ? "yes" : "NO",
	    fBaseSoc / 10, fBaseSoc % 10, fState.soc_x10 / 10, fState.soc_x10 % 10, fMinSoc, fMaxSoc, fBaseMa,
	    fLoadMa, currentRose ? "rose" : "did not rise 200 mA", fState.percent);
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (starlte_sbstop_at("battery")) {
		starlte_sbstop_now("battery");
	}
#endif
}

void
StarbiteBattery::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteBattery *me = OSDynamicCast(StarbiteBattery, owner);
	if (me == NULL || !me->fRunning) {
		return;
	}
	me->poll();
	if (me->fChgTestS) {
		me->chgTestStep(uptime_s());
	}
	if (me->fLoadS) {
		me->testStep(uptime_s());
	}
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	else if (me->fSamples == 1 && starlte_sbstop_at("battery")) {
		starlte_sbstop_now("battery");
	}
#endif
	sender->setTimeoutMS(me->fPollMs);
}

/* The charger watchdog clear, every MAX77705_WDT_KICK_S s on the work loop (serialised with the sampling). */
void
StarbiteBattery::kickFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteBattery *me = OSDynamicCast(StarbiteBattery, owner);
	if (me == NULL || !me->fRunning || !me->fKick) {
		return;
	}
	uint8_t w = 0;
	int r = max77705_wdt_kick(&me->fGauge, &w);
	if (r == MAX77705_KICK_OK) {
		me->fKicks++;
		if (!me->fKickLogged) {
			me->fKickLogged = true;
			IOLog("StarbiteBattery: charger watchdog: cleared every %u s (CNFG_06 0xbd = %#x: WDTCLR 01, CHGPROT kept)\n",
			    MAX77705_WDT_KICK_S, w);
		}
	} else if (r == MAX77705_KICK_OFF) {
		if (!me->fKickLogged) {
			me->fKickLogged = true;
			IOLog("StarbiteBattery: charger watchdog: not enabled (CNFG_00 WDTEN 0): nothing written\n");
		}
	} else if (me->fKickFails++ < 4) {
		IOLog("StarbiteBattery: charger watchdog clear failed (%d)\n", r);
	}
	me->setProperty("ChargerWatchdogClears", me->fKicks, 32);
	sender->setTimeoutMS(MAX77705_WDT_KICK_S * 1000u);
}

/* PHONE-TEST.md M11b: on the cable for fChgTestS s, did charging hold (no CHG_DTLS 0x0b, charging or full)? */
void
StarbiteBattery::chgTestStep(uint64_t now_s)
{
	uint64_t t = now_s - fT0;
	if (fChgDone) {
		return;
	}
	bool ok = fState.measured && fState.external && !fState.charger_wdt_expired && (fState.charging || fState.full);
	if (!ok && fChgHeld) {
		fChgHeld = false;
		fChgBadT = t;
		fChgBadDtls = fState.chg_dtls;
	}
	if (t < fChgTestS) {
		return;
	}
	fChgDone = true;
	if (fChgHeld) {
		IOLog("StarbiteBattery: M11b PASS: on the cable %llu s, charging throughout, %u watchdog clears, never expired\n",
		    t, fKicks);
	} else {
		IOLog("StarbiteBattery: M11b FAIL: charging lost at %llu s (CHG_DTLS %d), %u watchdog clears (clear %s)\n",
		    fChgBadT, fChgBadDtls, fKicks, fKick ? "on" : "off: sbchgwdt=0");
	}
}

/* ---- start / stop ---- */

bool
StarbiteBattery::start(IOService *provider)
{
	uint32_t on = 1, poll_s = 30, test = 0, load = 0, kick = 1, chgtest = 0;

	if (!super::start(provider)) {
		return false;
	}
	(void)PE_parse_boot_argn("sbbattery", &on, sizeof(on));
	(void)PE_parse_boot_argn("sbbatpoll", &poll_s, sizeof(poll_s));
	(void)PE_parse_boot_argn("sbbattest", &test, sizeof(test));
	(void)PE_parse_boot_argn("sbbatload", &load, sizeof(load));
	(void)PE_parse_boot_argn("sbchgwdt", &kick, sizeof(kick));
	(void)PE_parse_boot_argn("sbchgtest", &chgtest, sizeof(chgtest));
	fChgTestS = chgtest;
	fChgHeld = true;
	fTestLog = fTestLog || chgtest != 0;
	poll_s = poll_s < 2 ? 2 : poll_s > 300 ? 300 : poll_s;
	fPollMs = poll_s * 1000u;
	fTestLog = test != 0 || load != 0;
	fLoadS = load > 60 ? 60 : load;
	fT0 = uptime_s();
	fAllMeasured = true;
	fMinSoc = 1000;
	fMaxSoc = 0;

	fReal = false;
	if (on) {
		fMap = provider->mapDeviceMemoryWithIndex(0);
		if (fMap && fMap->getLength() >= 0x100) {
			fBase = fMap->getVirtualAddress();
			fIo.ctx = this;
			fIo.rd = ioRd;
			fIo.wr = ioWr;
			fIo.delay_us = ioDelay;
			hsi2c_init(&fBus, &fIo);
			fGauge.ctx = this;
			fGauge.read = gaugeRead;
			fGauge.write = kick ? gaugeWrite : NULL;
			fKick = kick != 0;
			uint8_t id = 0;
			bool pmic = hsi2c_read_reg(&fBus, MAX77705_PMIC_ADDR, 0x00, &id, 1) == 0;
			IOLog("StarbiteBattery: HSI2C %#llx, timing FS1 %#x FS2 %#x FS3 %#x (S-Boot's), MAX77705 PMICID %s%#x\n",
			    (unsigned long long)fMap->getPhysicalAddress(), fBus.fs1, fBus.fs2, fBus.fs3,
			    pmic ? "" : "unreadable ", id);
			fReal = true;
		} else {
			IOLog("StarbiteBattery: no reg 0 on the nub: static fallback\n");
		}
	} else {
		IOLog("StarbiteBattery: sbbattery=0: static fallback, no bus access\n");
	}
	/* A complete source before registerService wakes powerd. */
	poll();
	fLoop = IOWorkLoop::workLoop();
	fTimer = IOTimerEventSource::timerEventSource(this, &StarbiteBattery::timerFired);
	if (fLoop && fTimer && fLoop->addEventSource(fTimer) == kIOReturnSuccess) {
		fTimerAdded = true;
		fRunning = true;
		fTimer->setTimeoutMS(fPollMs);
		if (fKick) {
			fKickTimer = IOTimerEventSource::timerEventSource(this, &StarbiteBattery::kickFired);
			if (fKickTimer && fLoop->addEventSource(fKickTimer) == kIOReturnSuccess) {
				fKickAdded = true;
				fKickTimer->setTimeoutMS(1);    /* the first clear at once, then every 30 s */
			} else {
				IOLog("StarbiteBattery: charger watchdog: no timer, not cleared\n");
			}
		} else if (fReal) {
			IOLog("StarbiteBattery: charger watchdog: sbchgwdt=0, not cleared (read only)\n");
		}
	} else {
		IOLog("StarbiteBattery: no timer: the first sample stays\n");
	}
	registerService();
	return true;
}

IOReturn
StarbiteBattery::stopPolling(OSObject *owner, void *, void *, void *, void *)
{
	StarbiteBattery *me = OSDynamicCast(StarbiteBattery, owner);
	if (me == NULL) {
		return kIOReturnBadArgument;
	}
	me->fRunning = false;
	me->fTimer->cancelTimeout();
	me->fTimer->disable();
	if (me->fKickTimer) {
		me->fKickTimer->cancelTimeout();
		me->fKickTimer->disable();
	}
	return kIOReturnSuccess;
}

void
StarbiteBattery::stop(IOService *provider)
{
	if (fLoop && fTimerAdded) {
		(void)fLoop->runAction(&StarbiteBattery::stopPolling, this);
		fLoop->removeEventSource(fTimer);
		fTimerAdded = false;
		if (fKickAdded) {
			fLoop->removeEventSource(fKickTimer);
			fKickAdded = false;
		}
	}
	super::stop(provider);
}

void
StarbiteBattery::free(void)
{
	OSSafeReleaseNULL(fTimer);
	OSSafeReleaseNULL(fKickTimer);
	OSSafeReleaseNULL(fLoop);
	OSSafeReleaseNULL(fMap);
	super::free();
}
