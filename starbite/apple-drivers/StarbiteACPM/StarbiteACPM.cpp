/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteACPM (docs/APPLE-S9-DRIVERS.md Part 3 row 7): see StarbiteACPM.h, hw/acpm_ipc.c, NOTES.md.
 * Polled, one request at a time under fLock (as starlte_touch.c 0302); the IPC interrupt (SPI 0x44) is not used
 * and the mailbox's INTMR1 is left as found.
 */
#include "StarbiteACPM.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

#if defined(ARM64_BOARD_CONFIG_STARLTE)
extern "C" int starlte_sbstop_at(const char *stage);
extern "C" void starlte_sbstop_now(const char *stage);
#endif

#define super IOService
OSDefineMetaClassAndStructors(StarbiteACPM, IOService);

static StarbiteACPM *gStarbiteACPM;

StarbiteACPM *
StarbiteACPM::shared(void)
{
	return gStarbiteACPM;
}

/* ---- MMIO for hw/ ---- */

uint32_t
StarbiteACPM::ioSramRd(void *ctx, uint32_t off)
{
	StarbiteACPM *me = (StarbiteACPM *)ctx;
	uint32_t v = *(volatile uint32_t *)(me->fSram + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

void
StarbiteACPM::ioSramWr(void *ctx, uint32_t off, uint32_t v)
{
	StarbiteACPM *me = (StarbiteACPM *)ctx;
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(me->fSram + off) = v;
}

uint32_t
StarbiteACPM::ioMboxRd(void *ctx, uint32_t off)
{
	StarbiteACPM *me = (StarbiteACPM *)ctx;
	uint32_t v = *(volatile uint32_t *)(me->fMbox + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

void
StarbiteACPM::ioMboxWr(void *ctx, uint32_t off, uint32_t v)
{
	StarbiteACPM *me = (StarbiteACPM *)ctx;
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(me->fMbox + off) = v;
}

void
StarbiteACPM::ioBarrier(void *)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

void
StarbiteACPM::ioDelay(void *, uint32_t us)
{
	IODelay(us);
}

uint64_t
StarbiteACPM::ioNow(void *)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000u;
}

void
StarbiteACPM::ioLog(void *, const char *fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	IOLog("StarbiteACPM: %s", line);
}

/* ---- start ---- */

static bool
u64pair(OSData *d, uint64_t *a, uint64_t *b)
{
	if (d == NULL || d->getLength() < 16) {
		return false;
	}
	const uint8_t *p = (const uint8_t *)d->getBytesNoCopy();
	memcpy(a, p, 8);
	memcpy(b, p + 8, 8);
	return true;
}

bool
StarbiteACPM::mapRegions(IOService *provider, uint32_t *sramSize, uint32_t *initdata)
{
	fMboxMap = provider->mapDeviceMemoryWithIndex(0);
	if (fMboxMap == NULL || fMboxMap->getLength() < 0x100) {
		IOLog("StarbiteACPM: no mailbox (reg 0) on the nub\n");
		return false;
	}
	if (provider->getDeviceMemoryCount() >= 2) {
		fSramMap = provider->mapDeviceMemoryWithIndex(1);
	} else {
		uint64_t pa = 0, size = 0;
		OSData *d = OSDynamicCast(OSData, provider->getProperty("sram"));
		if (u64pair(d, &pa, &size) && size >= 0x1000 && size <= 0x100000) {
			fSramMem = IODeviceMemory::withRange((IOPhysicalAddress)pa, (IOPhysicalLength)size);
			if (fSramMem) {
				fSramMap = fSramMem->map(kIOMapInhibitCache);
			}
		}
	}
	if (fSramMap == NULL) {
		IOLog("StarbiteACPM: no APM SRAM (reg 1 or \"sram\") on the nub; see INTEGRATION.md\n");
		return false;
	}
	*sramSize = (uint32_t)fSramMap->getLength();
	*initdata = ACPM_S9_INITDATA;
	OSData *id = OSDynamicCast(OSData, provider->getProperty("initdata-base"));
	if (id && id->getLength() >= 4) {
		memcpy(initdata, id->getBytesNoCopy(), 4);
	}
	fMbox = fMboxMap->getVirtualAddress();
	fSram = fSramMap->getVirtualAddress();
	IOLog("StarbiteACPM: mailbox %#llx, SRAM %#llx + %#x, initdata-base %#x\n",
	    (unsigned long long)fMboxMap->getPhysicalAddress(), (unsigned long long)fSramMap->getPhysicalAddress(),
	    *sramSize, *initdata);
	return true;
}

/* One read-only look at the PMIC: the line PHONE-TEST.md's pass checks. No write. */
void
StarbiteACPM::report(void)
{
	static const struct {
		uint8_t type, reg;
		const char *name;
	} regs[] = {
		{ 0, 0x00, "PMICID" }, { 1, 0x6c, "LDO35" }, { 1, 0x74, "LDO43" }, { 1, 0x2b, "BUCK6" },
		{ 1, 0x68, "LDO31" }, { 1, 0x4a, "LDO2" }, { 2, 0x00, "RTC_CTRL" }, { 2, 0x03, "RTC_CAPSEL" },
	};
	char line[200];
	size_t o = 0;
	unsigned ok = 0;
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t v = 0;
		IOReturn r = pmicRead(regs[i].type, regs[i].reg, &v);
		ok += r == kIOReturnSuccess;
		o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%s=%s%02x", i ? " " : "", regs[i].name,
		    r == kIOReturnSuccess ? "0x" : "ERR", r == kIOReturnSuccess ? v : 0);
		if (o >= sizeof(line)) {
			o = sizeof(line) - 1;
		}
	}
	IOLog("StarbiteACPM: firmware \"%s\", PMIC channel %u: %s (%u/%u reads), %u timeouts, %u stale drained, "
	    "INTMR1 %#x INTSR1 %#x\n", fAcpm.version, ACPM_CH_PMIC, line, ok, (unsigned)(sizeof(regs) / sizeof(regs[0])),
	    fAcpm.st.timeouts, fAcpm.st.stale_drained, ioMboxRd(this, ACPM_MB_INTMR1), ioMboxRd(this, ACPM_MB_INTSR1));
}

void
StarbiteACPM::publishStats(void)
{
	setProperty("ACPMFirmware", fAcpm.version);
	setProperty("ACPMRequests", fAcpm.st.requests, 32);
	setProperty("ACPMTimeouts", fAcpm.st.timeouts, 32);
	setProperty("ACPMResends", fAcpm.st.resends, 32);
	setProperty("ACPMStatusErrors", fAcpm.st.status_errors, 32);
	setProperty("ACPMRefused", fAcpm.st.refused, 32);
}

bool
StarbiteACPM::start(IOService *provider)
{
	uint32_t sramSize = 0, initdata = 0;
	uint32_t off = 0;

	if (!super::start(provider)) {
		return false;
	}
	if (PE_parse_boot_argn("sbacpm", &off, sizeof(off)) && off == 0) {
		IOLog("StarbiteACPM: sbacpm=0: not started\n");
		return false;
	}
	fLock = IOLockAlloc();
	if (fLock == NULL || !mapRegions(provider, &sramSize, &initdata)) {
		return false;
	}
	fIo.ctx = this;
	fIo.sram_rd = ioSramRd;
	fIo.sram_wr = ioSramWr;
	fIo.mbox_rd = ioMboxRd;
	fIo.mbox_wr = ioMboxWr;
	fIo.barrier = ioBarrier;
	fIo.delay_us = ioDelay;
	fIo.now_us = ioNow;
	fIo.log = ioLog;
	IOLockLock(fLock);
	int e = acpm_setup(&fAcpm, &fIo, sramSize, ACPM_S9_SRAM_FIRST, initdata);
	IOLockUnlock(fLock);
	if (e != ACPM_OK) {
		IOLog("StarbiteACPM: setup failed: %s\n", acpm_strerror(e));
		return false;
	}
	fReady = true;
	gStarbiteACPM = this;
	report();
	publishStats();
#if defined(ARM64_BOARD_CONFIG_STARLTE)
	if (starlte_sbstop_at("acpm")) {
		starlte_sbstop_now("acpm");
	}
#endif
	registerService();
	return true;
}

void
StarbiteACPM::stop(IOService *provider)
{
	if (gStarbiteACPM == this) {
		gStarbiteACPM = NULL;
	}
	if (fLock) {
		IOLockLock(fLock);
		fReady = false;
		IOLockUnlock(fLock);
	}
	super::stop(provider);
}

void
StarbiteACPM::free(void)
{
	OSSafeReleaseNULL(fMboxMap);
	OSSafeReleaseNULL(fSramMap);
	OSSafeReleaseNULL(fSramMem);
	if (fLock) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	super::free();
}

/* ---- client API ---- */

static IOReturn
toIOReturn(int e)
{
	switch (e) {
	case ACPM_OK: return kIOReturnSuccess;
	case ACPM_E_TIMEOUT: return kIOReturnTimeout;
	case ACPM_E_STATUS: return kIOReturnIOError;
	case ACPM_E_REFUSED: return kIOReturnNotPermitted;
	case ACPM_E_ARG: return kIOReturnBadArgument;
	default: return kIOReturnNotReady;
	}
}

IOReturn
StarbiteACPM::pmicRead(uint32_t type, uint32_t reg, uint8_t *value)
{
	if (value == NULL) {
		return kIOReturnBadArgument;
	}
	if (!s2mps18_read_allowed(type, reg)) {
		return kIOReturnNotPermitted;
	}
	IOLockLock(fLock);
	int e = fReady ? acpm_pmic_read(&fAcpm, type, reg, value) : ACPM_E_NODEV;
	if (e == ACPM_OK) {
		s2mps18_note_read(&fPolicy, type, reg, *value);
	}
	IOLockUnlock(fLock);
	return toIOReturn(e);
}

IOReturn
StarbiteACPM::pmicWrite(uint32_t type, uint32_t reg, uint8_t value)
{
	const char *why = NULL;
	uint8_t cur = 0;

	IOLockLock(fLock);
	int e = fReady ? acpm_pmic_read(&fAcpm, type, reg, &cur) : ACPM_E_NODEV;
	if (e == ACPM_OK) {
		s2mps18_note_read(&fPolicy, type, reg, cur);
		if (s2mps18_write_allowed(&fPolicy, type, reg, value, cur, &why)) {
			e = acpm_pmic_write_raw(&fAcpm, type, reg, value);
		} else {
			fAcpm.st.refused++;
			e = ACPM_E_REFUSED;
		}
	}
	IOLockUnlock(fLock);
	if (e == ACPM_E_REFUSED) {
		IOLog("StarbiteACPM: write %u:%#x = %#x (now %#x) REFUSED: %s\n", type, reg, value, cur, why);
	} else if (e != ACPM_OK) {
		IOLog("StarbiteACPM: write %u:%#x = %#x failed: %s\n", type, reg, value, acpm_strerror(e));
	}
	return toIOReturn(e);
}

IOReturn
StarbiteACPM::dvfsSet(uint32_t domain, uint32_t khz)
{
	const char *why = NULL;
	IOLockLock(fLock);
	int e = ACPM_E_NODEV;
	if (fReady) {
		e = acpm_dvfs_allowed(domain, khz, fGovernor, &why) ? acpm_dvfs_request(&fAcpm, domain, khz, false, NULL) :
		    ACPM_E_REFUSED;
	}
	if (e == ACPM_E_REFUSED) {
		fAcpm.st.refused++;
	}
	IOLockUnlock(fLock);
	if (e == ACPM_E_REFUSED) {
		IOLog("StarbiteACPM: DVFS domain %u %u kHz REFUSED: %s\n", domain, khz, why);
	}
	return toIOReturn(e);
}

IOReturn
StarbiteACPM::dvfsGet(uint32_t domain, uint32_t *khz)
{
	IOLockLock(fLock);
	int e = fReady ? acpm_dvfs_request(&fAcpm, domain, 0, true, khz) : ACPM_E_NODEV;
	IOLockUnlock(fLock);
	return toIOReturn(e);
}

void
StarbiteACPM::setThermalGovernor(bool active)
{
	IOLockLock(fLock);
	fGovernor = active;
	IOLockUnlock(fLock);
	IOLog("StarbiteACPM: thermal governor %s: cpucl1 limit %u kHz\n", active ? "on" : "off",
	    active ? 2704000u : 1794000u);
}

/* callPlatformFunction("starbite-acpm-pmic-read", false, type, reg, uint8_t *val, NULL) and
 * "starbite-acpm-pmic-write" (type, reg, (void *)(uintptr_t)val), "starbite-acpm-dvfs" (domain, khz):
 * for clients that only hold an IOService *. */
IOReturn
StarbiteACPM::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *p1, void *p2, void *p3,
    void *p4)
{
	if (functionName->isEqualTo("starbite-acpm-pmic-read")) {
		return pmicRead((uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2, (uint8_t *)p3);
	}
	if (functionName->isEqualTo("starbite-acpm-pmic-write")) {
		return pmicWrite((uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2, (uint8_t)(uintptr_t)p3);
	}
	if (functionName->isEqualTo("starbite-acpm-dvfs")) {
		return dvfsSet((uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2);
	}
	return super::callPlatformFunction(functionName, waitForFunction, p1, p2, p3, p4);
}

extern "C" bool
starbite_acpm_pmic_read(uint32_t type, uint32_t reg, uint8_t *val)
{
	StarbiteACPM *a = gStarbiteACPM;
	return a != NULL && a->pmicRead(type, reg, val) == kIOReturnSuccess;
}

extern "C" bool
starbite_acpm_pmic_write(uint32_t type, uint32_t reg, uint8_t val)
{
	StarbiteACPM *a = gStarbiteACPM;
	return a != NULL && a->pmicWrite(type, reg, val) == kIOReturnSuccess;
}

extern "C" bool
starbite_acpm_dvfs(uint32_t domain, uint32_t khz)
{
	StarbiteACPM *a = gStarbiteACPM;
	return a != NULL && a->dvfsSet(domain, khz) == kIOReturnSuccess;
}
