/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteTouch: the Galaxy S9's touch screen (Samsung S6SY761 at I2C 0x48 on the USI03 HSI2C, INT on GPA1-0)
 * as an IOHIDEventService on Apple's open-source IOHIDFamily (docs/APPLE-S9-DRIVERS.md Part 3 row 14).
 *
 *   macOS (MacBookAir10,1 identity): a pointer: GenericDesktop Mouse + Pointer usage pairs, relative pointer
 *     events steered to the finger (direct touch), tap = click, hold-and-move = drag, two fingers = scroll and
 *     secondary click (hw/sb_touch_gesture.h). Boot-arg sbtouchabs=1 gives the absolute pointer instead.
 *   iOS (iPhone12,1 identity): a digitizer (Digitizer / TouchScreen): collection events with a finger child
 *     per contact, display-integrated, edge contacts marked.
 *
 * Addresses come only from the ADT: the touch node names its bus, the bus's clock / USI / pin blocks and the
 * interrupt pin's block; their nubs' "reg" are mapped (INTEGRATION.md has the node).
 */
#ifndef _STARBITE_TOUCH_H
#define _STARBITE_TOUCH_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/hidevent/IOHIDEventService.h>

#include "hw/sb_hsi2c.h"
#include "hw/sb_s6sy761.h"
#include "hw/sb_touch_gesture.h"
#include "hw/sb_touch_script.h"

class StarbiteTouch : public IOHIDEventService
{
	OSDeclareDefaultStructors(StarbiteTouch);

public:
	virtual bool            start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void            free(void) APPLE_KEXT_OVERRIDE;
	virtual IOWorkLoop *    getWorkLoop(void) const APPLE_KEXT_OVERRIDE;
	/* a contact from outside the work loop (the legacy sweet_hid_touch entry): runs it on the work loop */
	void                    externalContact(int finger, bool down, int x, int y);

protected:
	virtual bool            handleStart(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void            handleStop(IOService *provider) APPLE_KEXT_OVERRIDE;

	virtual OSString *      getTransport(void) APPLE_KEXT_OVERRIDE;
	virtual OSString *      getManufacturer(void) APPLE_KEXT_OVERRIDE;
	virtual OSString *      getProduct(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getVendorID(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getProductID(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getLocationID(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getReportInterval(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getPrimaryUsagePage(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getPrimaryUsage(void) APPLE_KEXT_OVERRIDE;
	virtual OSArray *       getDeviceUsagePairs(void) APPLE_KEXT_OVERRIDE;

private:
	/* bring-up */
	bool                    mapBlocks(IOService *provider);
	IOMemoryMap *           mapNode(const char *name, IOByteCount need);
	bool                    supplyOn(void);
	void                    probe(void);
	void                    configureGeometry(void);
	void                    publishModeProperties(void);
	bool                    intLow(void);

	/* event paths */
	static void             contactThunk(void *ctx, int finger, bool down, int x, int y);
	void                    contact(int finger, bool down, int x, int y);
	void                    dispatchActions(const struct sb_tg_action *a, int n);
	void                    dispatchFrame(const struct sb_dig_frame *f);
	void                    kickSteer(void);

	/* work loop event sources */
	static void             probeFired(OSObject *owner, IOTimerEventSource *sender);
	static void             pollFired(OSObject *owner, IOTimerEventSource *sender);
	static void             steerFired(OSObject *owner, IOTimerEventSource *sender);
	static void             scriptFired(OSObject *owner, IOTimerEventSource *sender);
	static void             stopFired(OSObject *owner, IOTimerEventSource *sender);
	static void             interruptFired(OSObject *owner, IOInterruptEventSource *sender, int count);
	void                    service(void);
	void                    runSelfTest(void);
	void                    report(bool final);

	IOWorkLoop *            fWorkLoop;
	IOTimerEventSource *    fProbeTimer;
	IOTimerEventSource *    fPollTimer;
	IOTimerEventSource *    fSteerTimer;
	IOTimerEventSource *    fScriptTimer;
	IOTimerEventSource *    fStopTimer;
	IOInterruptEventSource *fIntSource;
	IOCommandGate *         fGate;

	IOMemoryMap *           fBusMap;
	IOMemoryMap *           fCmuMap;
	IOMemoryMap *           fSysregMap;
	IOMemoryMap *           fPinsMap;
	IOMemoryMap *           fIrqMap;
	uint32_t                fIrqBank, fIrqPin;
	bool                    fIrqActiveLow;

	struct sb_hsi2c         fBus;
	struct sb_usi_i2c_platform fPlat;
	struct sb_s6            fChip;
	struct sb_tg            fGesture;
	struct sb_dig           fDigitizer;
	struct sb_script        fScript;
	struct sb_selftest_result fSelfTest;

	OSString *              fTransport;
	OSString *              fManufacturer;
	OSString *              fProduct;
	OSArray *               fUsagePairs;

	bool                    fPointer;               /* macOS pointer; false = iOS digitizer */
	bool                    fChipUp;
	bool                    fSteerActive;
	bool                    fStopArmed;
	bool                    fScriptOnFrame;
	uint32_t                fAbsButtons, fRelButtons;
	unsigned                fNotes;
	uint32_t                fPollMS;
	uint32_t                fWindowS;
	uint64_t                fScriptBase;            /* mach_absolute_time() the script counts from, 0 = not yet */
	uint64_t                fLastReport;

	/* counts for the log and the M8 result line */
	unsigned                fLiveContacts, fScriptContacts, fPointerMoves, fClicks, fScrolls, fFrames;
	unsigned                fPolls, fInterrupts;
	bool                    fInScript;
};

#endif /* _STARBITE_TOUCH_H */
