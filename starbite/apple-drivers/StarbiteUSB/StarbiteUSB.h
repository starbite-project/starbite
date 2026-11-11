/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB: the S9's USB port for Apple's OSes, phase 1 (docs/APPLE-S9-DRIVERS.md Part 3 row 23, M11).
 *
 *   StarbiteUSB      IOService on the usb-drd nub ("starbite,dwc3"). Owns the DWC3 in device mode, the PHY
 *                    (the usb-phy nub's registers) and the CDC-NCM function (hw/). Needs only XNU's own IOKit.
 *                    With sbusb_echo=1 it answers ping by itself (PHONE-TEST.md), no network stack involved.
 *   StarbiteUSBNCM   IOEthernetController on StarbiteUSB (StarbiteUSBNCM.cpp). Needs IONetworkingFamily.
 *
 * All addresses come from the two nubs. Public KPIs only, with one marked exception (STARBITE_USB_SBSTOP).
 */
#ifndef STARBITE_USB_H
#define STARBITE_USB_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" {
#include "hw/sbusb_func.h"
}

/* what a network client (StarbiteUSBNCM) gives; every call comes on StarbiteUSB's work loop */
struct StarbiteUSBClient {
	void *owner;
	void (*rx)(void *owner, const uint8_t *frame, uint32_t length);
	void (*rxBatchDone)(void *owner);
	void (*link)(void *owner, bool up, bool fullSpeed);
	void (*txReady)(void *owner);
};

class StarbiteUSB : public IOService {
	OSDeclareDefaultStructors(StarbiteUSB);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOWorkLoop *getWorkLoop(void) const APPLE_KEXT_OVERRIDE;

	/* for the client; work loop only */
	void setClient(const StarbiteUSBClient *client);
	uint8_t *txBegin(uint32_t length, bool *stall);
	void txCommit(void);
	bool linkUp(void) const;
	bool fullSpeed(void) const;
	void getAddresses(uint8_t device[6], uint8_t host[6]) const;

private:
	IOWorkLoop *fLoop;
	IOTimerEventSource *fTimer;
	IOInterruptEventSource *fIRQ;
	IOService *fPhyNub;
	IOMemoryMap *fMap[SBUSB_BLK_COUNT];
	IOBufferMemoryDescriptor *fDMA;
	bool fDMAPrepared, fRunning, fEcho, fStopWanted;
	StarbiteUSBClient fClient;
	struct sbusb_func fFunc;
	uint32_t fSeenConn, fSeenCfg, fSeenRestarts, fEchoReplies, fIdleLogs;
	uint64_t fNextIdleLog, fStopAt;

	bool mapBlocks(IOService *provider);
	void unmapBlocks(void);
	void poll(void);
	void report(void);
	void echo(const uint8_t *frame, uint32_t length);
	static void timerFired(OSObject *owner, IOTimerEventSource *sender);
	static void interruptFired(OSObject *owner, IOInterruptEventSource *sender, int count);
	static IOReturn stopGated(OSObject *owner, void *, void *, void *, void *);

	/* hw/sbusb_io.h backend */
	static int ioHas(void *ctx, unsigned blk);
	static uint32_t ioRead(void *ctx, unsigned blk, uint32_t off);
	static void ioWrite(void *ctx, unsigned blk, uint32_t off, uint32_t value);
	static void ioDelay(void *ctx, unsigned us);
	static uint64_t ioNow(void *ctx);
	static void ioSync(void *ctx, uint32_t off, uint32_t len, int toDevice);
	/* hw/sbusb_func.h callbacks */
	static void funcRx(void *ctx, const uint8_t *frame, uint32_t len);
	static void funcLink(void *ctx, int up, int fullSpeed);
	static void funcTxReady(void *ctx);
};

#endif
