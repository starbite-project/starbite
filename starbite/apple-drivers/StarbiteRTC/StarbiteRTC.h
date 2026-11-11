/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteRTC: an IORTC (public iokit/Kernel/IORTC.cpp) on the S2MPS18's RTC block, reached through StarbiteACPM
 * (ACPM PMIC type 2). Publishes the "IORTC" resource that IOKitInitializeTime waits for; the platform expert
 * forwards getGMTTimeOfDay / setGMTTimeOfDay here (INTEGRATION.md), as Apple's AppleARMPE does with its RTC.
 *
 * Boot-args:
 *   sbrtc=0          not started (the card clock, 0393, stays the only source)
 *   sbrtcwrite=1     setGMTTimeOfDay writes the RTC (off by default until PHONE-TEST.md M5b passed)
 *   sbrtcset=SECS    test: write SECS + uptime once at start (needs sbrtcwrite=1), read back, log
 *   sbstop=rtc       0423 self-stop after the first read (and after sbrtcset)
 */
#ifndef STARBITE_RTC_H
#define STARBITE_RTC_H

#include <IOKit/rtc/IORTCController.h>
#include "hw/s2mps18_rtc.h"

class StarbiteACPM;

class StarbiteRTC : public IORTC {
	OSDeclareDefaultStructors(StarbiteRTC);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

	long getGMTTimeOfDay(void) APPLE_KEXT_OVERRIDE;
	void setGMTTimeOfDay(long secs) APPLE_KEXT_OVERRIDE;
	void setAlarmEnable(IOOptionBits message) APPLE_KEXT_OVERRIDE;

	static StarbiteRTC *shared(void);       /* the started instance, or NULL (for the platform expert) */

private:
	StarbiteACPM *fACPM;
	struct s2m_rtc_ops fOps;
	bool fWrite;
	bool fValid;
	uint32_t fWrites, fWriteFails, fReadFails;

	int readNow(uint64_t *secs, const char **why);
	static int opRd(void *ctx, uint8_t reg, uint8_t *v);
	static int opWr(void *ctx, uint8_t reg, uint8_t v);
	static void opDelay(void *ctx, uint32_t us);
};

#endif
