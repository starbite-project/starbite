/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteWatchdog (docs/APPLE-S9-DRIVERS.md Part 3 row 9): see StarbiteWatchdog.h, hw/sbwdt.c, NOTES.md.
 * The IOWatchdog front end answers watchdogd as SweetWatchdog (0040/0045) did: every selector accepted with zeroed
 * outputs, IOPMUPrimary = 1; selector 1 counts as a check-in [inferred, NOTES.md].
 */
#include "StarbiteWatchdog.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <kern/clock.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

#if defined(ARM64_BOARD_CONFIG_STARLTE)
extern "C" uint64_t sweet_fb_first_present;     /* SweetFramebuffer: first frame on the panel (0421) */
#endif
#if defined(STARBITE_WDT_VETO_HOOK)
extern "C" void starlte_wdt_veto(const char *why);      /* herolte_ufs.c, added by integration */
#endif

/* ---- IOWatchdog (Apple's name, for watchdogd) ---- */

OSDefineMetaClassAndStructors(IOWatchdog, IOService);

bool
IOWatchdog::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IOWatchdogUserClient");
	setProperty("IOPMUPrimary", 1ULL, 32);     /* watchdogd writes the boot stage to the IOPMUPrimary service */
	if (getMetaClass() == IOWatchdog::metaClass) {
		registerService();              /* the plain front end (IOResources fallback); subclasses register later */
		IOLog("StarbiteWatchdog: plain IOWatchdog (check-ins only, no hardware)\n");
	}
	return true;
}

IOReturn
IOWatchdog::setProperties(OSObject *)
{
	return kIOReturnSuccess;
}

void
IOWatchdog::userCheckIn(void)
{
}

/* ---- the user client ---- */

OSDefineMetaClassAndStructors(IOWatchdogUserClient, IOUserClient);

bool
IOWatchdogUserClient::start(IOService *provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fOwner = OSDynamicCast(IOWatchdog, provider);
	return fOwner != NULL;
}

IOReturn
IOWatchdogUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

static uint64_t gWdtLogged;

IOReturn
IOWatchdogUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
    OSObject *, void *)
{
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutputDescriptor != NULL) {
		static const uint64_t zero = 0;
		IOByteCount len = args->structureOutputDescriptor->getLength();
		for (IOByteCount off = 0; off < len; off += 8) {
			args->structureOutputDescriptor->writeBytes(off, &zero, len - off < 8 ? len - off : 8);
		}
	} else if (args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector == 1 && fOwner) {
		fOwner->userCheckIn();
	}
	if (selector < 64 && !(gWdtLogged & (1ULL << selector))) {
		gWdtLogged |= 1ULL << selector;
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("StarbiteWatchdog: %s selector %u (in %u/%u, out %u/%u) -> ok\n", name, selector,
		    args->scalarInputCount, args->structureInputSize, args->scalarOutputCount, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* ---- StarbiteWatchdog ---- */

OSDefineMetaClassAndStructors(StarbiteWatchdog, IOWatchdog);

static uint64_t
uptime_ms(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

bool
StarbiteWatchdog::start(IOService *provider)
{
	struct sbwdt_cfg c;
	char stop[16] = "";
	uint32_t own = 0;

	if (!IOWatchdog::start(provider)) {
		return false;
	}
	bzero(&c, sizeof(c));
	fLock = IOLockAlloc();
	fMap = provider->mapDeviceMemoryWithIndex(0);
	if (fLock == NULL || fMap == NULL || fMap->getLength() < 0x10) {
		IOLog("StarbiteWatchdog: no reg 0 on the nub: check-ins only\n");
		registerService();
		return true;
	}
	fBase = fMap->getVirtualAddress();
	c.wtcon = *(volatile uint32_t *)(fBase + SBWDT_WTCON);
	c.wtdat = *(volatile uint32_t *)(fBase + SBWDT_WTDAT);
	uint32_t cnt = *(volatile uint32_t *)(fBase + SBWDT_WTCNT);
	c.sbwdt_found = PE_parse_boot_argn("sbwdt", &c.sbwdt_s, sizeof(c.sbwdt_s));
	c.test_boot = PE_parse_boot_argn("sbstop", stop, sizeof(stop) - 1) && stop[0] != '\0';
	(void)PE_parse_boot_argn("sweetreboot", &c.sweetreboot_s, sizeof(c.sweetreboot_s));
	c.noui_s = 300;
	(void)PE_parse_boot_argn("sbnoui", &c.noui_s, sizeof(c.noui_s));
	(void)PE_parse_boot_argn("sbwdtuser", &c.user_timeout_s, sizeof(c.user_timeout_s));
	/* The PMU's mask words are not in this nub: treat the reset as masked, so without sbwdt= nothing is fed. */
	c.pmu_mask = 1u << SBWDT_PMU_BIT;
	sbwdt_plan(&c, &fW);
#if defined(STARBITE_WDT_OWNER)
	(void)PE_parse_boot_argn("sbwdtown", &own, sizeof(own));
#endif
	fOwner = own != 0 && fW.may_feed;
	IOLog("StarbiteWatchdog: cluster 1 WTCON %#x WTDAT %u WTCNT %u (%u ms); %s; mode %s, period %u ms, userspace "
	    "timeout %u s\n", c.wtcon, c.wtdat, cnt, fW.hw_ms, fW.why, fOwner ? "OWNER (feeds)" : "observer (never writes)",
	    fW.period_ms, c.user_timeout_s);
	setProperty("StarbiteWatchdogMode", fOwner ? "owner" : "observer");
	setProperty("StarbiteWatchdogPlan", fW.why);
	if (fW.may_feed) {
		fLoop = IOWorkLoop::workLoop();
		fTimer = IOTimerEventSource::timerEventSource(this, &StarbiteWatchdog::timerFired);
		if (fLoop && fTimer && fLoop->addEventSource(fTimer) == kIOReturnSuccess) {
			fTimerAdded = fRunning = true;
			fTimer->setTimeoutMS(fOwner ? fW.period_ms : 1000);
		}
	}
	registerService();
	return true;
}

void
StarbiteWatchdog::userCheckIn(void)
{
	if (fLock) {
		IOLockLock(fLock);
		sbwdt_checkin(&fW, uptime_ms());
		IOLockUnlock(fLock);
	}
}

void
StarbiteWatchdog::tick(void)
{
	bool frame = true;
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	frame = sweet_fb_first_present != 0;
#endif
	IOLockLock(fLock);
	int was = fW.stopped;
	bool feed = sbwdt_tick(&fW, uptime_ms(), panic_active() != 0, frame);
	int now = fW.stopped;
	IOLockUnlock(fLock);
	if (feed && fOwner) {
		__asm__ volatile ("dmb oshst" ::: "memory");
		*(volatile uint32_t *)(fBase + SBWDT_WTCNT) = fW.reload;
	}
	if (now != was) {
		IOLog("StarbiteWatchdog: %s after %llu feeds, %llu check-ins; the watchdog resets the phone\n",
		    sbwdt_stop_name(now), fW.feeds, fW.checkins);
		fRunning = false;
		if (!fOwner && !fVetoSent) {
			fVetoSent = true;
#if defined(STARBITE_WDT_VETO_HOOK)
			starlte_wdt_veto(sbwdt_stop_name(now));         /* today's feeder stops for good */
#else
			IOLog("StarbiteWatchdog: observer without the veto hook: today's feeder keeps feeding\n");
#endif
		}
	}
}

void
StarbiteWatchdog::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteWatchdog *me = OSDynamicCast(StarbiteWatchdog, owner);
	if (me == NULL || !me->fRunning) {
		return;
	}
	me->tick();
	if (me->fRunning) {
		sender->setTimeoutMS(me->fOwner ? me->fW.period_ms : 1000);
	}
}

IOReturn
StarbiteWatchdog::stopPolling(OSObject *owner, void *, void *, void *, void *)
{
	StarbiteWatchdog *me = OSDynamicCast(StarbiteWatchdog, owner);
	if (me == NULL) {
		return kIOReturnBadArgument;
	}
	me->fRunning = false;
	me->fTimer->cancelTimeout();
	me->fTimer->disable();
	return kIOReturnSuccess;
}

void
StarbiteWatchdog::stop(IOService *provider)
{
	/* Stopping the driver stops feeding: the watchdog then resets the phone (fail-safe). */
	if (fLoop && fTimerAdded) {
		(void)fLoop->runAction(&StarbiteWatchdog::stopPolling, this);
		fLoop->removeEventSource(fTimer);
		fTimerAdded = false;
	}
	IOWatchdog::stop(provider);
}

void
StarbiteWatchdog::free(void)
{
	OSSafeReleaseNULL(fTimer);
	OSSafeReleaseNULL(fLoop);
	OSSafeReleaseNULL(fMap);
	if (fLock) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOWatchdog::free();
}
