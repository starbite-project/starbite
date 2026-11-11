/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteButtons: the Galaxy S9's power, volume up, volume down and Bixby keys (ALIVE GPIO, active low) as an
 * IOHIDEventService on Apple's open-source IOHIDFamily (docs/APPLE-S9-DRIVERS.md Part 3 row 15): Consumer
 * Control usages (Power 0x30, Volume Increment 0xE9, Volume Decrement 0xEA, Menu 0x40 for Bixby, which stands in
 * for Home as today). The same service on macOS and iOS; dispatchKeyboardEvent gives Apple's own chord handling.
 *
 * The pin controller's address comes from the ADT (the keys node names it); the key table is the S9's unless the
 * node carries one (INTEGRATION.md).
 */
#ifndef _STARBITE_BUTTONS_H
#define _STARBITE_BUTTONS_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/hidevent/IOHIDEventService.h>

#include "hw/sb_keys.h"

class StarbiteButtons : public IOHIDEventService
{
	OSDeclareDefaultStructors(StarbiteButtons);

public:
	virtual bool            start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void            free(void) APPLE_KEXT_OVERRIDE;
	virtual IOWorkLoop *    getWorkLoop(void) const APPLE_KEXT_OVERRIDE;
	/* a press from outside the work loop (the legacy sweet_hid_key entry), by usage */
	void                    externalKey(uint16_t usage, bool down);

protected:
	virtual bool            handleStart(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void            handleStop(IOService *provider) APPLE_KEXT_OVERRIDE;

	virtual OSString *      getTransport(void) APPLE_KEXT_OVERRIDE;
	virtual OSString *      getManufacturer(void) APPLE_KEXT_OVERRIDE;
	virtual OSString *      getProduct(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getVendorID(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getProductID(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getReportInterval(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getPrimaryUsagePage(void) APPLE_KEXT_OVERRIDE;
	virtual UInt32          getPrimaryUsage(void) APPLE_KEXT_OVERRIDE;

private:
	bool                    loadTable(IOService *provider);
	uint32_t                readPressed(void);
	void                    key(unsigned index, bool down, bool scripted);
	static void             pollFired(OSObject *owner, IOTimerEventSource *sender);
	static void             scriptFired(OSObject *owner, IOTimerEventSource *sender);

	IOWorkLoop *            fWorkLoop;
	IOTimerEventSource *    fPollTimer;
	IOTimerEventSource *    fScriptTimer;
	IOCommandGate *         fGate;
	IOMemoryMap *           fGpioMap;
	uintptr_t               fGpio;

	struct sb_key_def       fDefs[SB_KEYS_MAX];
	char                    fNames[SB_KEYS_MAX][12];
	unsigned                fCount;
	uint32_t                fBanks[SB_KEYS_BANKS];
	unsigned                fBankCount;
	struct sb_keys          fKeys;
	struct sb_keyscript     fScript;
	uint64_t                fScriptBase;

	OSString *              fTransport;
	OSString *              fManufacturer;
	OSString *              fProduct;
	unsigned                fEvents;
};

#endif /* _STARBITE_BUTTONS_H */
