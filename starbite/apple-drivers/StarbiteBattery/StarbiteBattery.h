/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteBattery: IOPMPowerSource for the S9's MAX77705 (fuel gauge 0x36, charger 0x69) on HSI2C 0x14360000,
 * read only. Matching: IONameMatch "hsi2c-pmic" (the arm-io node name; tools/board/starlte.board) with its own
 * IOMatchCategory, so StarbiteHSI2C can sit on the same nub later. Address: the nub's reg 0.
 *
 * Boot-args:
 *   sbbattery=0       no bus access: SweetBattery's static 100 % on AC (as today)
 *   sbbatpoll=S       poll period in seconds (default 30; 2..300)
 *   sbbattest=1       log every sample (PHONE-TEST.md M11)
 *   sbbatload=S       test: after a 10 s baseline, busy every CPU for S s (max 60), then the M11 verdict line
 *   sbstop=battery    0423 self-stop after the verdict (or after the first sample without sbbatload)
 */
#ifndef STARBITE_BATTERY_H
#define STARBITE_BATTERY_H

#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "hw/exynos_hsi2c.h"
#include "hw/max77705.h"

class StarbiteBattery : public IOPMPowerSource {
	OSDeclareDefaultStructors(StarbiteBattery);

public:
	bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;

private:
	IOMemoryMap *fMap;
	vm_address_t fBase;
	IOWorkLoop *fLoop;
	IOTimerEventSource *fTimer;
	bool fTimerAdded, fRunning, fReal, fTestLog;
	struct hsi2c fBus;
	struct hsi2c_io fIo;
	struct max77705_bus fGauge;
	struct battery_state fState;
	uint32_t fLow, fPollMs, fSamples, fLastMeasured;
	/* M11 test */
	uint32_t fLoadS;
	uint64_t fT0;
	int fPhase;                     /* 0 baseline, 1 load, 2 done */
	int32_t fBaseSoc, fBaseMa, fMinSoc, fMaxSoc, fLoadMa;
	bool fAllMeasured;

	void putNumber(const char *key, int64_t value);
	void putBool(const char *key, bool value);
	void putString(const char *key, const char *value);
	void publish(const struct battery_state *st);
	void poll(void);
	void testStep(uint64_t now_s);
	static void timerFired(OSObject *owner, IOTimerEventSource *sender);
	static IOReturn stopPolling(OSObject *owner, void *, void *, void *, void *);

	static uint32_t ioRd(void *ctx, uint32_t off);
	static void ioWr(void *ctx, uint32_t off, uint32_t v);
	static void ioDelay(void *ctx, uint32_t us);
	static int gaugeRead(void *ctx, uint8_t addr7, uint8_t reg, uint8_t *buf, unsigned len);
};

#endif
