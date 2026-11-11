/* SPDX-License-Identifier: APSL-2.0
 * StarbiteUSB (StarbiteUSB.h): the IOKit shell around hw/. Register logic, USB and NCM are all in hw/ and
 * host-tested; this file maps the nubs, allocates the DMA block, and runs hw/ from one work loop.
 *
 * Boot-args (all optional):
 *   sbusb=1          start the driver. Default 0: phase 1 is opt-in until M11 has passed on the phone, so no
 *                    other milestone's boot touches the USB hardware.
 *   sbusb_ring=1     ring transfers on the data endpoints (hw/sbusb_dwc3.h). Default: the single-TRB model.
 *   sbusb_irq=1      use the usb-drd nub's interrupt when it has one. Default: a 1 ms timer.
 *   sbusb_echo=1     answer ping from the driver itself: IPv6 on the link-local address of the device MAC,
 *                    IPv4 on 169.254.83.66 (hw/sbusb_echo.h, PHONE-TEST.md).
 *   sbusb_serial=S   USB serial number (the MAC addresses derive from it). Default: IOPlatformSerialNumber,
 *                    else "STARBITE".
 *   sbstop=usbncm    (kernel 0423) with sbusb_echo=1: after the third echo reply the test boot stops itself
 *                    and returns to TWRP.
 * start() never waits for a host and never blocks boot: every wait in hw/ is bounded (0.35 s at worst), and
 * a failure returns false with one log line.
 */
#include "StarbiteUSB.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>

extern "C" {
#include "hw/sbusb_echo.h"
}

/* The one use of a symbol outside the public KPIs: the milestone self-stop of our own kernel (0423,
 * bsd/dev/herolte_ufs.c). Build with -DSTARBITE_USB_SBSTOP=0 for a kernel collection. */
#ifndef STARBITE_USB_SBSTOP
#define STARBITE_USB_SBSTOP 1
#endif
#if STARBITE_USB_SBSTOP
extern "C" int starlte_sbstop_at(const char *stage);
extern "C" void starlte_sbstop_now(const char *stage);
#endif

#define super IOService
OSDefineMetaClassAndStructors(StarbiteUSB, IOService);

#define LOG(fmt, ...) IOLog("StarbiteUSB: " fmt "\n", ##__VA_ARGS__)
#define POLL_ACTIVE_MS  1
#define POLL_IDLE_MS    20

static const char *const kLineNames[4] = { "SE0", "J", "K", "SE1" };

/* ---- hw backend ----------------------------------------------------------------------------- */

int
StarbiteUSB::ioHas(void *ctx, unsigned blk)
{
	StarbiteUSB *me = (StarbiteUSB *)ctx;
	return blk < SBUSB_BLK_COUNT && me->fMap[blk] != NULL;
}

uint32_t
StarbiteUSB::ioRead(void *ctx, unsigned blk, uint32_t off)
{
	StarbiteUSB *me = (StarbiteUSB *)ctx;
	IOMemoryMap *map = blk < SBUSB_BLK_COUNT ? me->fMap[blk] : NULL;

	if (map == NULL || (uint64_t)off + 4 > map->getLength()) {
		return 0;
	}
	uint32_t value = *(volatile uint32_t *)(map->getVirtualAddress() + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return value;
}

void
StarbiteUSB::ioWrite(void *ctx, unsigned blk, uint32_t off, uint32_t value)
{
	StarbiteUSB *me = (StarbiteUSB *)ctx;
	IOMemoryMap *map = blk < SBUSB_BLK_COUNT ? me->fMap[blk] : NULL;

	if (map == NULL || (uint64_t)off + 4 > map->getLength()) {
		return;
	}
	__asm__ volatile ("dsb st" ::: "memory");       /* TRB and buffer stores reach memory before a doorbell */
	*(volatile uint32_t *)(map->getVirtualAddress() + off) = value;
}

void
StarbiteUSB::ioDelay(void *, unsigned us)
{
	if (us >= 1000) {
		IOSleep((us + 999) / 1000);
	} else {
		IODelay(us);
	}
}

uint64_t
StarbiteUSB::ioNow(void *)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

void
StarbiteUSB::ioSync(void *ctx, uint32_t off, uint32_t len, int toDevice)
{
	StarbiteUSB *me = (StarbiteUSB *)ctx;
	/* The block is asked for uncached; this keeps it right even if that request was not honoured. */
	(void)me->fDMA->performOperation(toDevice ? kIOMemoryIncoherentIOStore : kIOMemoryIncoherentIOFlush, off, len);
}

/* ---- function callbacks --------------------------------------------------------------------- */

void
StarbiteUSB::echo(const uint8_t *frame, uint32_t length)
{
	static const uint8_t ip[4] = SBUSB_ECHO_IP;
	uint8_t reply[SBUSB_DG_MAX];
	int kind = SBUSB_ECHO_NONE, stall = 0;
	uint32_t n = sbusb_echo(ip, fFunc.d.st.id.dev_mac, frame, length, reply, sizeof(reply), &kind);

	if (n == 0) {
		return;
	}
	uint8_t *out = sbusb_func_tx_begin(&fFunc, n, &stall);
	if (out == NULL) {
		return;
	}
	bcopy(reply, out, n);
	sbusb_func_tx_commit(&fFunc);
	if (kind == SBUSB_ECHO_ARP || kind == SBUSB_ECHO_ND) {
		if (fEchoReplies == 0) {
			LOG("echo: %s answered", kind == SBUSB_ECHO_ARP ? "ARP for 169.254.83.66" : "neighbour solicitation");
		}
	} else if (++fEchoReplies <= 3) {
		LOG("M11 PASS: echo reply %u sent over %s (ping over NCM)", fEchoReplies,
		    kind == SBUSB_ECHO_ICMP6 ? "IPv6" : "IPv4");
#if STARBITE_USB_SBSTOP
		if (fEchoReplies == 3 && starlte_sbstop_at("usbncm")) {
			fStopWanted = true;             /* after the reply has gone out: poll() */
			fStopAt = ioNow(this) + 300000;
		}
#endif
	}
}

void
StarbiteUSB::funcRx(void *ctx, const uint8_t *frame, uint32_t len)
{
	StarbiteUSB *me = (StarbiteUSB *)ctx;
	if (me->fEcho) {
		me->echo(frame, len);
	}
	if (me->fClient.rx != NULL) {
		me->fClient.rx(me->fClient.owner, frame, len);
	}
}

void
StarbiteUSB::funcLink(void *ctx, int up, int fullSpeed)
{
	StarbiteUSB *me = (StarbiteUSB *)ctx;
	LOG("NCM data interface %s (%s speed)", up ? "open: link up" : "closed: link down", fullSpeed ? "full" : "high");
	me->setProperty("SBUSBLinkUp", up ? kOSBooleanTrue : kOSBooleanFalse);
	if (me->fClient.link != NULL) {
		me->fClient.link(me->fClient.owner, up != 0, fullSpeed != 0);
	}
}

void
StarbiteUSB::funcTxReady(void *ctx)
{
	StarbiteUSB *me = (StarbiteUSB *)ctx;
	if (me->fClient.txReady != NULL) {
		me->fClient.txReady(me->fClient.owner);
	}
}

/* ---- client API ----------------------------------------------------------------------------- */

void
StarbiteUSB::setClient(const StarbiteUSBClient *client)
{
	if (client != NULL) {
		fClient = *client;
		if (fClient.link != NULL) {
			fClient.link(fClient.owner, fFunc.up != 0, fFunc.d.st.full_speed != 0);
		}
	} else {
		bzero(&fClient, sizeof(fClient));
	}
}

uint8_t *
StarbiteUSB::txBegin(uint32_t length, bool *stall)
{
	int s = 0;
	uint8_t *p = fRunning ? sbusb_func_tx_begin(&fFunc, length, &s) : NULL;
	*stall = s != 0;
	return p;
}

void
StarbiteUSB::txCommit(void)
{
	sbusb_func_tx_commit(&fFunc);
}

bool
StarbiteUSB::linkUp(void) const
{
	return fRunning && fFunc.up != 0;
}

bool
StarbiteUSB::fullSpeed(void) const
{
	return fFunc.d.st.full_speed != 0;
}

void
StarbiteUSB::getAddresses(uint8_t device[6], uint8_t host[6]) const
{
	bcopy(fFunc.d.st.id.dev_mac, device, 6);
	bcopy(fFunc.d.st.id.host_mac, host, 6);
}

IOWorkLoop *
StarbiteUSB::getWorkLoop(void) const
{
	return fLoop;
}

/* ---- polling -------------------------------------------------------------------------------- */

void
StarbiteUSB::report(void)
{
	const struct sbusb_stats *n = &fFunc.d.n;

	if (n->conndone != fSeenConn) {
		fSeenConn = n->conndone;
		LOG("host connected, %s speed (resets %u)", fFunc.d.st.full_speed ? "full" : "high", n->resets);
	}
	if (n->set_configs != fSeenCfg) {
		fSeenCfg = n->set_configs;
		LOG("configuration %u set by the host (setups %u, stalls %u)", fFunc.d.st.config, n->setups, n->stalls);
	}
	if (n->restarts != fSeenRestarts) {
		fSeenRestarts = n->restarts;
		LOG("link restarted from the PHY (%u): %s", n->restarts,
		    fFunc.d.started ? "running" : fFunc.d.why != NULL ? fFunc.d.why : "failed");
	}
	/* while no host has configured us: one line every 10 s, six times, so a photo or pmsg says how far it got */
	if (fFunc.d.st.config == 0 && fIdleLogs < 6) {
		uint64_t now = ioNow(this);
		if (now >= fNextIdleLog) {
			fNextIdleLog = now + 10000000;
			++fIdleLogs;
			LOG("no host yet: line %s, events %u resets %u setups %u stalls %u cmd failures %u (0x%08x)",
			    kLineNames[sbusb_phy_line(&fFunc.d.io) & 3], n->events, n->resets, n->setups, n->stalls, n->cmd_fail,
			    n->last_cmd_fail);
		}
	}
}

void
StarbiteUSB::poll(void)
{
	if (!fRunning) {
		return;
	}
	uint32_t before = fFunc.n.rx_frames;
	(void)sbusb_func_poll(&fFunc);
	if (fFunc.n.rx_frames != before && fClient.rxBatchDone != NULL) {
		fClient.rxBatchDone(fClient.owner);
	}
	report();
#if STARBITE_USB_SBSTOP
	if (fStopWanted && (fFunc.tx_inflight == 0 || ioNow(this) >= fStopAt)) {
		fStopWanted = false;
		LOG("sbstop=usbncm: rx %u frames in %u NTBs (%u bad), tx %u frames in %u NTBs; stopping",
		    fFunc.n.rx_frames, fFunc.n.rx_ntb, fFunc.n.rx_bad, fFunc.n.tx_frames, fFunc.n.tx_ntb);
		sbusb_func_stop(&fFunc);                /* off the bus cleanly: the Mac sees a disconnect */
		fRunning = false;
		starlte_sbstop_now("usbncm");
	}
#endif
}

void
StarbiteUSB::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteUSB *me = OSDynamicCast(StarbiteUSB, owner);
	if (me == NULL || !me->fRunning) {
		return;
	}
	me->poll();
	if (me->fRunning) {
		/* with the interrupt: only the timeouts and the link watch need the timer */
		sender->setTimeoutMS(me->fIRQ != NULL ? 100 : me->fFunc.d.connected ? POLL_ACTIVE_MS : POLL_IDLE_MS);
	}
}

void
StarbiteUSB::interruptFired(OSObject *owner, IOInterruptEventSource *, int)
{
	StarbiteUSB *me = OSDynamicCast(StarbiteUSB, owner);
	if (me != NULL) {
		me->poll();
	}
}

/* ---- start / stop --------------------------------------------------------------------------- */

bool
StarbiteUSB::mapBlocks(IOService *provider)
{
	/* usb-drd "reg" 0 = the DWC3. usb-phy "reg" 0 = USBCON, then (INTEGRATION.md) the two PMU isolation
	 * registers and the three clock gates, in SBUSB_BLK_* order. */
	fMap[SBUSB_BLK_DWC3] = provider->mapDeviceMemoryWithIndex(0);
	if (fMap[SBUSB_BLK_DWC3] == NULL || fMap[SBUSB_BLK_DWC3]->getLength() < 0xd000) {
		LOG("the usb-drd nub has no usable \"reg\" (need 0x10000 bytes of DWC3)");
		return false;
	}
	OSDictionary *match = IOService::nameMatching("usb-phy");
	if (match != NULL) {
		fPhyNub = IOService::waitForMatchingService(match, 1000000000ULL);     /* at most 1 s */
		match->release();
	}
	if (fPhyNub == NULL) {
		LOG("no usb-phy nub: the PHY cannot be brought up");
		return false;
	}
	for (unsigned i = 0; i + SBUSB_BLK_PHY < SBUSB_BLK_COUNT; ++i) {
		fMap[SBUSB_BLK_PHY + i] = fPhyNub->mapDeviceMemoryWithIndex(i);
	}
	if (fMap[SBUSB_BLK_PHY] == NULL || fMap[SBUSB_BLK_PHY]->getLength() < 0x120) {
		LOG("the usb-phy nub has no usable \"reg\" (need 0x200 bytes of USBCON)");
		return false;
	}
	return true;
}

void
StarbiteUSB::unmapBlocks(void)
{
	for (unsigned i = 0; i < SBUSB_BLK_COUNT; ++i) {
		OSSafeReleaseNULL(fMap[i]);
	}
	OSSafeReleaseNULL(fPhyNub);
}

bool
StarbiteUSB::start(IOService *provider)
{
	uint32_t on = 0, ring = 0, irq = 0, echoArg = 0;
	char serial[33] = "";
	struct sbusb_io io = { this, ioHas, ioRead, ioWrite, ioDelay, ioNow, ioSync };
	struct sbusb_func_ops ops = { this, funcRx, funcLink, funcTxReady };
	struct sbusb_dma dma;
	IOByteCount segment = 0;
	unsigned flags = 0;
	int r;

	(void)PE_parse_boot_argn("sbusb", &on, sizeof(on));
	if (on == 0) {
		return false;                           /* quietly: phase 1 is opt-in */
	}
	if (!super::start(provider)) {
		return false;
	}
	(void)PE_parse_boot_argn("sbusb_ring", &ring, sizeof(ring));
	(void)PE_parse_boot_argn("sbusb_irq", &irq, sizeof(irq));
	(void)PE_parse_boot_argn("sbusb_echo", &echoArg, sizeof(echoArg));
	fEcho = echoArg != 0;
	if (!PE_parse_boot_argn("sbusb_serial", serial, sizeof(serial)) || serial[0] == '\0') {
		IORegistryEntry *root = IORegistryEntry::fromPath("/", gIOServicePlane);
		OSString *s = root != NULL ? OSDynamicCast(OSString, root->getProperty("IOPlatformSerialNumber")) : NULL;
		if (s != NULL) {
			strlcpy(serial, s->getCStringNoCopy(), sizeof(serial));
		}
		OSSafeReleaseNULL(root);
	}

	if (!mapBlocks(provider)) {
		goto fail;
	}
	/* One block the controller reaches: below 4 GiB like the loader's, 16 KiB aligned, physically
	 * contiguous, asked for uncached (the loader's DMA window is non-cacheable too). */
	fDMA = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, SBUSB_DMA_BYTES, 0x00000000ffffc000ULL);
	if (fDMA == NULL || fDMA->prepare() != kIOReturnSuccess) {
		LOG("no DMA memory (%u bytes, contiguous, below 4 GiB)", SBUSB_DMA_BYTES);
		goto fail;
	}
	fDMAPrepared = true;
	dma.va = (uint8_t *)fDMA->getBytesNoCopy();
	dma.pa = fDMA->getPhysicalSegment(0, &segment);
	dma.size = SBUSB_DMA_BYTES;
	if (dma.va == NULL || dma.pa == 0 || segment < SBUSB_DMA_BYTES) {
		LOG("the DMA block is not one physical segment (%llu of %u bytes)", (uint64_t)segment, SBUSB_DMA_BYTES);
		goto fail;
	}

	fLoop = IOWorkLoop::workLoop();
	fTimer = fLoop != NULL ? IOTimerEventSource::timerEventSource(this, timerFired) : NULL;
	if (fTimer == NULL || fLoop->addEventSource(fTimer) != kIOReturnSuccess) {
		LOG("no work loop or timer");
		goto fail;
	}
	if (irq != 0) {
		fIRQ = IOInterruptEventSource::interruptEventSource(this, interruptFired, provider, 0);
		if (fIRQ == NULL || fLoop->addEventSource(fIRQ) != kIOReturnSuccess) {
			LOG("sbusb_irq=1 but the usb-drd nub has no interrupt (SPI 244): polling");
			OSSafeReleaseNULL(fIRQ);
		}
	}
	flags = (ring != 0 ? SBUSB_F_RING : 0) | (fIRQ != NULL ? SBUSB_F_IRQ : 0);
	sbusb_func_init(&fFunc, &io, &dma, serial, flags, &ops);
	LOG("start: transfers %s, events by %s, echo %s, serial %s, DMA 0x%llx", ring != 0 ? "ring" : "single",
	    fIRQ != NULL ? "interrupt" : "timer", fEcho ? "on" : "off", fFunc.d.st.id.serial, dma.pa);
	{
		const uint8_t *m = fFunc.d.st.id.dev_mac;
		LOG("device %02x:%02x:%02x:%02x:%02x:%02x = fe80::%x:%02xff:fe%02x:%02x%02x, host MAC ends %02x", m[0], m[1],
		    m[2], m[3], m[4], m[5], ((m[0] ^ 2) << 8) | m[1], m[2], m[3], m[4], m[5], fFunc.d.st.id.host_mac[5]);
	}

	r = sbusb_func_start(&fFunc);
	if (fFunc.d.phy.missing != 0) {
		LOG("usb-phy nub lacks reg entries (block mask 0x%x): PMU isolation / clock gates left as the loader set them",
		    fFunc.d.phy.missing);
	}
	if (r != 0) {
		LOG("start failed at stage %d: %s (GSNPSID 0x%08x, UTMI 0x%08x, HSP_TEST 0x%08x)", r,
		    fFunc.d.why != NULL ? fFunc.d.why : "?", fFunc.d.id, fFunc.d.phy.utmi, fFunc.d.phy.hsp_test);
		sbusb_func_stop(&fFunc);
		goto fail;
	}
	fRunning = true;
	LOG("PHY up (line %s), DWC3 0x%08x running as %04x:%04x, waiting for a host", kLineNames[fFunc.d.phy.line & 3],
	    fFunc.d.id, fFunc.d.st.id.vid, fFunc.d.st.id.pid);
	setProperty("SBUSBSerial", fFunc.d.st.id.serial);
	setProperty("SBUSBLinkUp", kOSBooleanFalse);
	if (fIRQ != NULL) {
		fIRQ->enable();
	}
	fTimer->setTimeoutMS(POLL_IDLE_MS);
	registerService();                              /* StarbiteUSBNCM matches here */
	return true;

fail:
	stop(provider);
	return false;
}

IOReturn
StarbiteUSB::stopGated(OSObject *owner, void *, void *, void *, void *)
{
	StarbiteUSB *me = (StarbiteUSB *)owner;
	if (me->fRunning) {
		me->fRunning = false;
		sbusb_func_stop(&me->fFunc);
	}
	return kIOReturnSuccess;
}

void
StarbiteUSB::stop(IOService *provider)
{
	if (fLoop != NULL) {
		(void)fLoop->runAction(stopGated, this);
		if (fTimer != NULL) {
			fTimer->cancelTimeout();
			fLoop->removeEventSource(fTimer);
		}
		if (fIRQ != NULL) {
			fIRQ->disable();
			fLoop->removeEventSource(fIRQ);
		}
	}
	OSSafeReleaseNULL(fTimer);
	OSSafeReleaseNULL(fIRQ);
	if (fDMA != NULL && fDMAPrepared) {
		fDMA->complete();
		fDMAPrepared = false;
	}
	OSSafeReleaseNULL(fDMA);
	unmapBlocks();
	super::stop(provider);
}

void
StarbiteUSB::free(void)
{
	OSSafeReleaseNULL(fLoop);
	super::free();
}
