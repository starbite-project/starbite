/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSBNCM: the phone's end of the USB network link as an Ethernet controller (IONetworkingFamily's
 * public KPI), so macOS / iOS on the phone gets an "en" interface. A client of StarbiteUSB (StarbiteUSB.h).
 */
#ifndef STARBITE_USB_NCM_H
#define STARBITE_USB_NCM_H

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/network/IONetworkMedium.h>
#include "StarbiteUSB.h"

class StarbiteUSBNCM : public IOEthernetController {
	OSDeclareDefaultStructors(StarbiteUSBNCM);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOWorkLoop *getWorkLoop(void) const APPLE_KEXT_OVERRIDE;

	IOReturn enable(IONetworkInterface *interface) APPLE_KEXT_OVERRIDE;
	IOReturn disable(IONetworkInterface *interface) APPLE_KEXT_OVERRIDE;
	UInt32 outputPacket(mbuf_t packet, void *param) APPLE_KEXT_OVERRIDE;
	IOOutputQueue *createOutputQueue(void) APPLE_KEXT_OVERRIDE;
	bool configureInterface(IONetworkInterface *interface) APPLE_KEXT_OVERRIDE;
	IOReturn getHardwareAddress(IOEthernetAddress *address) APPLE_KEXT_OVERRIDE;
	IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const APPLE_KEXT_OVERRIDE;
	IOReturn setPromiscuousMode(bool active) APPLE_KEXT_OVERRIDE;
	IOReturn setMulticastMode(bool active) APPLE_KEXT_OVERRIDE;
	IOReturn setMulticastList(IOEthernetAddress *addresses, UInt32 count) APPLE_KEXT_OVERRIDE;
	const OSString *newVendorString(void) const APPLE_KEXT_OVERRIDE;
	const OSString *newModelString(void) const APPLE_KEXT_OVERRIDE;

private:
	StarbiteUSB *fUSB;
	IOEthernetInterface *fNetif;
	IOOutputQueue *fQueue;
	IONetworkStats *fNetStats;
	IOEthernetStats *fEtherStats;
	OSDictionary *fMedia;
	const IONetworkMedium *fMediumHigh, *fMediumFull;
	bool fEnabled, fLinkUp, fFullSpeed, fClientSet;

	void publishLink(void);
	static IOReturn attachGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn detachGated(OSObject *owner, void *, void *, void *, void *);
	static void usbRx(void *owner, const uint8_t *frame, uint32_t length);
	static void usbRxBatchDone(void *owner);
	static void usbLink(void *owner, bool up, bool fullSpeed);
	static void usbTxReady(void *owner);
};

#endif
