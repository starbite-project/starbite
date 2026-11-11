/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteWatchdog: Apple's IOWatchdog interface for watchdogd (IOServiceMatching("IOWatchdog"), a user client,
 * IOPMUPrimary) on cluster 1's Exynos watchdog (0x10060000), keeping the loader's dead-man contract (hw/sbwdt.h).
 *
 * Two modes:
 *   observer (default)  the watchdog is never written. Today's feeder in herolte_ufs.c (0399/0421/0423) keeps the
 *                       contract. With sbwdtuser=T and the integration hook (STARBITE_WDT_VETO_HOOK), a watchdogd
 *                       that stops checking in for T s stops that feeder for good (starlte_wdt_veto), so a userspace
 *                       hang resets the phone where INFORM3 says.
 *   owner (sbwdtown=1, only when built with STARBITE_WDT_OWNER, i.e. after integration removed the herolte_ufs.c
 *                       feeder): this driver writes WTCNT = WTDAT itself, under the same contract.
 * Never arms or re-programs the watchdog (WTCON / WTDAT are only read).
 *
 * IOWatchdog / IOWatchdogUserClient are defined here; SweetWatchdog.cpp (0040/0045), which defines the same
 * classes, must not be built alongside (INTEGRATION.md).
 */
#ifndef STARBITE_WATCHDOG_H
#define STARBITE_WATCHDOG_H

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "hw/sbwdt.h"

class IOWatchdog : public IOService {
	OSDeclareDefaultStructors(IOWatchdog);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
	virtual void userCheckIn(void);
};

class StarbiteWatchdog : public IOWatchdog {
	OSDeclareDefaultStructors(StarbiteWatchdog);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	void userCheckIn(void) APPLE_KEXT_OVERRIDE;

private:
	IOMemoryMap *fMap;
	vm_address_t fBase;
	IOWorkLoop *fLoop;
	IOTimerEventSource *fTimer;
	bool fTimerAdded, fRunning, fOwner, fVetoSent;
	IOLock *fLock;
	struct sbwdt fW;

	void tick(void);
	static void timerFired(OSObject *owner, IOTimerEventSource *sender);
	static IOReturn stopPolling(OSObject *owner, void *, void *, void *, void *);
};

class IOWatchdogUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOWatchdogUserClient);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;

private:
	IOWatchdog *fOwner;
};

#endif
