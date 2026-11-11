/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteACPM: the provider service for the Exynos 9810 APM mailbox (ACPM IPC). Clients (StarbiteRTC, and later
 * StarbiteTouch / StarbiteSD / StarbiteMali / StarbiteTMU) reach the S2MPS18 PMIC and the DVFS domains through it.
 * Every PMIC write passes hw/s2mps18_policy.c; every DVFS request passes the clock limits below.
 *
 * Matching: IONameMatch "starbite,acpm-ipc" on the arm-io nub (tools/board/starlte.board block "acpm-ipc").
 * Addresses: the nub's reg 0 (mailbox), reg 1 or the property "sram" <u64 pa, u64 size> (the APM SRAM),
 * "initdata-base" (u32, default 0x7f00 as device/dtb/starlte.dts). Nothing is hard-coded.
 */
#ifndef STARBITE_ACPM_H
#define STARBITE_ACPM_H

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "hw/acpm_ipc.h"
#include "hw/s2mps18_policy.h"

class StarbiteACPM : public IOService {
	OSDeclareDefaultStructors(StarbiteACPM);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
	    void *param3, void *param4) APPLE_KEXT_OVERRIDE;

	/* S2MPS18 access. type: 0 TOP, 1 PMIC, 2 RTC. Reads of clear-on-read interrupt registers are refused. */
	virtual IOReturn pmicRead(uint32_t type, uint32_t reg, uint8_t *value);
	/* Read-check-write: the current value is read, the policy decides, then the write. */
	virtual IOReturn pmicWrite(uint32_t type, uint32_t reg, uint8_t value);
	/* DVFS (channel at table index 0). Limits: cpucl0/cpucl1 <= 1794000 kHz; cpucl1 up to 2704000 only while a
	 * thermal governor has registered (setThermalGovernor); g3d only the six levels of starlte_gpu.c; no other
	 * domain (MIF/INT froze the phone, boot69). */
	virtual IOReturn dvfsSet(uint32_t domain, uint32_t khz);
	virtual IOReturn dvfsGet(uint32_t domain, uint32_t *khz);
	virtual void setThermalGovernor(bool active);

	static StarbiteACPM *shared(void);      /* the started instance, or NULL */

private:
	IOMemoryMap *fMboxMap;
	IOMemoryMap *fSramMap;
	IODeviceMemory *fSramMem;               /* when it came from the "sram" property */
	IOLock *fLock;
	struct acpm fAcpm;
	struct acpm_io fIo;
	struct s2mps18_policy fPolicy;
	vm_address_t fMbox;
	vm_address_t fSram;
	bool fGovernor;
	bool fReady;

	bool mapRegions(IOService *provider, uint32_t *sramSize, uint32_t *initdata);
	void report(void);
	void publishStats(void);

	static uint32_t ioSramRd(void *ctx, uint32_t off);
	static void ioSramWr(void *ctx, uint32_t off, uint32_t v);
	static uint32_t ioMboxRd(void *ctx, uint32_t off);
	static void ioMboxWr(void *ctx, uint32_t off, uint32_t v);
	static void ioBarrier(void *ctx);
	static void ioDelay(void *ctx, uint32_t us);
	static uint64_t ioNow(void *ctx);
	static void ioLog(void *ctx, const char *fmt, ...) __printflike(2, 3);
};

/* C entry points for today's bsd/dev drivers (starlte_touch.c, starlte_gpu.c, starlte_sd.c) once integration
 * routes starlte_pmic_read / starlte_pmic_write / starlte_acpm_dvfs here (INTEGRATION.md). false until
 * StarbiteACPM has started. Same policy and limits as the methods. */
extern "C" {
bool starbite_acpm_pmic_read(uint32_t type, uint32_t reg, uint8_t *val);
bool starbite_acpm_pmic_write(uint32_t type, uint32_t reg, uint8_t val);
bool starbite_acpm_dvfs(uint32_t domain, uint32_t khz);
}

#endif
