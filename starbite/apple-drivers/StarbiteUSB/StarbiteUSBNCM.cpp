/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSBNCM (StarbiteUSBNCM.h). Everything runs on StarbiteUSB's work loop: the output queue is a gated
 * queue on it, enable/disable come through IONetworkController's command gate on it, and StarbiteUSB calls
 * back from its timer or interrupt on it.
 *   out: outputPacket() copies the mbuf chain into the NTB StarbiteUSB is filling (hw/sbusb_func.c decides
 *        when it goes on the wire); when every NTB is in flight the queue stalls and usbTxReady restarts it.
 *   in:  usbRx() gets one Ethernet frame per datagram of a received NTB, copies it into a fresh mbuf and
 *        queues it on the interface; usbRxBatchDone() flushes the input queue once per poll.
 */
#include "StarbiteUSBNCM.h"
#include <IOKit/IOLib.h>
#include <sys/kpi_mbuf.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(StarbiteUSBNCM, IOEthernetController);

#define LOG(fmt, ...) IOLog("StarbiteUSBNCM: " fmt "\n", ##__VA_ARGS__)

IOWorkLoop *
StarbiteUSBNCM::getWorkLoop(void) const
{
	return fUSB != NULL ? fUSB->getWorkLoop() : NULL;
}

IOOutputQueue *
StarbiteUSBNCM::createOutputQueue(void)
{
	return IOGatedOutputQueue::withTarget(this, getWorkLoop());
}

const OSString *
StarbiteUSBNCM::newVendorString(void) const
{
	return OSString::withCString("Starbite");
}

const OSString *
StarbiteUSBNCM::newModelString(void) const
{
	return OSString::withCString("S9 USB network (CDC-NCM device side)");
}

IOReturn
StarbiteUSBNCM::getHardwareAddress(IOEthernetAddress *address)
{
	uint8_t host[6];
	fUSB->getAddresses(address->bytes, host);
	return kIOReturnSuccess;
}

IOReturn
StarbiteUSBNCM::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
	if (group == gIOEthernetWakeOnLANFilterGroup) {
		*filters = 0;
		return kIOReturnSuccess;
	}
	if (group == gIONetworkFilterGroup) {
		/* a point-to-point cable: every frame the host sends is handed up */
		*filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast | kIOPacketFilterMulticast |
		    kIOPacketFilterMulticastAll | kIOPacketFilterPromiscuous;
		return kIOReturnSuccess;
	}
	return super::getPacketFilters(group, filters);
}

IOReturn StarbiteUSBNCM::setPromiscuousMode(bool) { return kIOReturnSuccess; }
IOReturn StarbiteUSBNCM::setMulticastMode(bool) { return kIOReturnSuccess; }
IOReturn StarbiteUSBNCM::setMulticastList(IOEthernetAddress *, UInt32) { return kIOReturnSuccess; }

bool
StarbiteUSBNCM::configureInterface(IONetworkInterface *interface)
{
	IONetworkData *data;

	if (!super::configureInterface(interface)) {
		return false;
	}
	data = interface->getNetworkData(kIONetworkStatsKey);
	fNetStats = data != NULL ? (IONetworkStats *)data->getBuffer() : NULL;
	data = interface->getNetworkData(kIOEthernetStatsKey);
	fEtherStats = data != NULL ? (IOEthernetStats *)data->getBuffer() : NULL;
	return fNetStats != NULL;
}

void
StarbiteUSBNCM::publishLink(void)
{
	const IONetworkMedium *medium = fFullSpeed ? fMediumFull : fMediumHigh;

	if (fLinkUp) {
		setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, medium, fFullSpeed ? 12000000ULL : 480000000ULL);
	} else {
		setLinkStatus(kIONetworkLinkValid, medium, 0);
	}
}

/* ---- StarbiteUSB callbacks (work loop) ------------------------------------------------------ */

void
StarbiteUSBNCM::usbLink(void *owner, bool up, bool fullSpeed)
{
	StarbiteUSBNCM *me = (StarbiteUSBNCM *)owner;

	me->fLinkUp = up;
	me->fFullSpeed = fullSpeed;
	me->publishLink();
	if (me->fQueue != NULL && me->fEnabled) {
		if (up) {
			me->fQueue->start();
		} else {
			me->fQueue->stop();
			me->fQueue->flush();            /* nothing waits for a cable that is gone */
		}
	}
}

void
StarbiteUSBNCM::usbTxReady(void *owner)
{
	StarbiteUSBNCM *me = (StarbiteUSBNCM *)owner;
	if (me->fQueue != NULL && me->fEnabled) {
		me->fQueue->service(IOBasicOutputQueue::kServiceAsync);
	}
}

void
StarbiteUSBNCM::usbRx(void *owner, const uint8_t *frame, uint32_t length)
{
	StarbiteUSBNCM *me = (StarbiteUSBNCM *)owner;
	mbuf_t m;

	if (!me->fEnabled || me->fNetif == NULL) {
		return;
	}
	m = me->allocatePacket(length);
	if (m == NULL || mbuf_copyback(m, 0, length, frame, MBUF_DONTWAIT) != 0) {
		if (m != NULL) {
			me->freePacket(m);
		}
		if (me->fNetStats != NULL) {
			++me->fNetStats->inputErrors;
		}
		return;
	}
	(void)me->fNetif->inputPacket(m, length, IONetworkInterface::kInputOptionQueuePacket);
	if (me->fNetStats != NULL) {
		++me->fNetStats->inputPackets;
	}
}

void
StarbiteUSBNCM::usbRxBatchDone(void *owner)
{
	StarbiteUSBNCM *me = (StarbiteUSBNCM *)owner;
	if (me->fEnabled && me->fNetif != NULL) {
		(void)me->fNetif->flushInputQueue();
	}
}

/* ---- data out ------------------------------------------------------------------------------- */

UInt32
StarbiteUSBNCM::outputPacket(mbuf_t packet, void *)
{
	size_t length = mbuf_pkthdr_len(packet);
	bool stall = false;
	uint8_t *room;

	if (!fEnabled || !fLinkUp || length < 14 || length > SBUSB_DG_MAX) {
		freePacket(packet);
		if (fNetStats != NULL) {
			++fNetStats->outputErrors;
		}
		return kIOReturnOutputDropped;
	}
	room = fUSB->txBegin((uint32_t)length, &stall);
	if (room == NULL) {
		if (stall) {
			return kIOReturnOutputStall;    /* the queue keeps the packet; usbTxReady restarts it */
		}
		freePacket(packet);
		if (fNetStats != NULL) {
			++fNetStats->outputErrors;
		}
		return kIOReturnOutputDropped;
	}
	if (mbuf_copydata(packet, 0, length, room) != 0) {
		bzero(room, length);                    /* cannot happen for a well-formed chain; never send stale bytes */
	}
	fUSB->txCommit();
	freePacket(packet);
	if (fNetStats != NULL) {
		++fNetStats->outputPackets;
	}
	return kIOReturnOutputSuccess;
}

IOReturn
StarbiteUSBNCM::enable(IONetworkInterface *)
{
	fEnabled = true;
	publishLink();
	if (fQueue != NULL && fLinkUp) {
		fQueue->start();
	}
	return kIOReturnSuccess;
}

IOReturn
StarbiteUSBNCM::disable(IONetworkInterface *)
{
	fEnabled = false;
	if (fQueue != NULL) {
		fQueue->stop();
		fQueue->flush();
	}
	return kIOReturnSuccess;
}

/* ---- start / stop --------------------------------------------------------------------------- */

IOReturn
StarbiteUSBNCM::attachGated(OSObject *owner, void *, void *, void *, void *)
{
	StarbiteUSBNCM *me = (StarbiteUSBNCM *)owner;
	StarbiteUSBClient client = { me, usbRx, usbRxBatchDone, usbLink, usbTxReady };

	me->fUSB->setClient(&client);           /* calls usbLink with the state now */
	me->fClientSet = true;
	return kIOReturnSuccess;
}

IOReturn
StarbiteUSBNCM::detachGated(OSObject *owner, void *, void *, void *, void *)
{
	StarbiteUSBNCM *me = (StarbiteUSBNCM *)owner;

	if (me->fClientSet) {
		me->fUSB->setClient(NULL);
		me->fClientSet = false;
	}
	me->fEnabled = false;
	return kIOReturnSuccess;
}

bool
StarbiteUSBNCM::start(IOService *provider)
{
	IOWorkLoop *loop;

	fUSB = OSDynamicCast(StarbiteUSB, provider);
	if (fUSB == NULL || fUSB->getWorkLoop() == NULL) {
		return false;
	}
	fUSB->retain();
	if (!super::start(provider)) {
		return false;
	}
	loop = getWorkLoop();
	fQueue = getOutputQueue();
	if (fQueue == NULL) {
		LOG("no output queue");
		goto fail;
	}
	fQueue->retain();
	fQueue->setCapacity(256);

	fMedia = OSDictionary::withCapacity(2);
	fMediumHigh = IONetworkMedium::medium(kIOMediumEthernetAuto | kIOMediumOptionFullDuplex, 480000000ULL);
	fMediumFull = IONetworkMedium::medium(kIOMediumEthernetAuto, 12000000ULL);
	if (fMedia == NULL || fMediumHigh == NULL || fMediumFull == NULL ||
	    !IONetworkMedium::addMedium(fMedia, fMediumHigh) || !IONetworkMedium::addMedium(fMedia, fMediumFull) ||
	    !publishMediumDictionary(fMedia) || !setCurrentMedium(fMediumHigh)) {
		LOG("no medium dictionary");
		goto fail;
	}
	publishLink();
	if (!attachInterface((IONetworkInterface **)&fNetif, false) || fNetif == NULL) {
		LOG("no Ethernet interface");
		goto fail;
	}
	(void)loop->runAction(attachGated, this);
	fNetif->registerService();
	LOG("Ethernet interface attached (link %s)", fLinkUp ? "up" : "down");
	return true;

fail:
	stop(provider);
	return false;
}

void
StarbiteUSBNCM::stop(IOService *provider)
{
	IOWorkLoop *loop = getWorkLoop();

	if (loop != NULL) {
		(void)loop->runAction(detachGated, this);
	}
	if (fNetif != NULL) {
		detachInterface(fNetif);
	}
	OSSafeReleaseNULL(fNetif);
	OSSafeReleaseNULL(fQueue);
	super::stop(provider);
}

void
StarbiteUSBNCM::free(void)
{
	OSSafeReleaseNULL(fMedia);
	OSSafeReleaseNULL(fMediumHigh);
	OSSafeReleaseNULL(fMediumFull);
	OSSafeReleaseNULL(fUSB);
	super::free();
}
