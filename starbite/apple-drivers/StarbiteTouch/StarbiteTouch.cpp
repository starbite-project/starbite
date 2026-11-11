/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteTouch (see StarbiteTouch.h). Everything that is not IOKit lives in hw/ and is host-tested
 * (test/run.sh); this file maps the ADT blocks, runs the bring-up and the polling on its own work loop, and
 * turns the hw/ actions into IOHIDFamily events.
 *
 * Boot-args:
 *   sbtouch=0              do not start
 *   sbtouchmode=1 / =2     pointer (macOS) / digitizer (iOS); default: pointer when the ADT root model is a Mac
 *   sbtouchabs=1           macOS: absolute pointer (0373) instead of direct touch (0397)
 *   sbtouchscroll=N        panel pixels per scroll line (default 24; negative flips the direction)
 *   sbtouchpwr=0           leave the touch supplies alone (S-Boot's state)
 *   sbtouchpoll=MS         poll period while no interrupt is wired (default 5)
 *   sbtouchselftest=0      skip the packet self-test at bring-up
 *   sbtouchscript=...      scripted contacts (hw/sb_touch_script.h); sbtouchscriptframe=1 counts its seconds
 *                          from the first frame on the panel instead of from the driver's start
 *   sbstop=hid             M8 test boot: the result line SBH_HIDWIN seconds after bring-up, then the 0423 stop
 *   sbhidwin=S             that window (default 25 s)
 */
#include "StarbiteTouch.h"
#include "StarbiteKernelHooks.h"

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/hid/IOHIDEvent.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>

extern "C" uint64_t mach_absolute_time(void);
extern "C" void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);

#define super IOHIDEventService
OSDefineMetaClassAndStructors(StarbiteTouch, IOHIDEventService);

static StarbiteTouch *gStarbiteTouch;   /* for the legacy entry point, set once registered */

#define LOG(fmt, ...) IOLog("StarbiteTouch: " fmt "\n", ##__VA_ARGS__)
#define SB_TOUCH_NOTES_MAX 200          /* gesture notes logged per boot */

/* hw/ masks are Apple's digitizer event mask bits */
static_assert(SB_DIG_RANGE == kIOHIDDigitizerEventRange, "digitizer mask");
static_assert(SB_DIG_TOUCH == kIOHIDDigitizerEventTouch, "digitizer mask");
static_assert(SB_DIG_POSITION == kIOHIDDigitizerEventPosition, "digitizer mask");
static_assert(SB_DIG_IDENTITY == kIOHIDDigitizerEventIdentity, "digitizer mask");
static_assert(SB_DIG_FROM_EDGE_TIP == kIOHIDDigitizerEventFromEdgeTip, "digitizer mask");

/* ---- device memory for hw/ ---- */

static uint32_t
mmio_rd(void *ctx __unused, uintptr_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
mmio_wr(void *ctx __unused, uintptr_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
}

static void
mmio_udelay(void *ctx __unused, unsigned us)
{
	IODelay(us);
}

static const struct sb_mmio_ops gTouchMMIO = { mmio_rd, mmio_wr, mmio_udelay, NULL };

static uint64_t
now_ns(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

/* ---- ADT helpers ---- */

/* The index-th NUL-separated string of an ADT property, or NULL. */
static const char *
prop_string(IORegistryEntry *e, const char *key, unsigned index, char *buf, size_t size)
{
	OSData *d = OSDynamicCast(OSData, e->getProperty(key));
	if (d == NULL || d->getLength() == 0) {
		return NULL;
	}
	const char *p = (const char *)d->getBytesNoCopy();
	unsigned len = d->getLength(), at = 0;
	for (unsigned i = 0; i < index; i++) {
		while (at < len && p[at] != '\0') {
			at++;
		}
		at++;
		if (at >= len) {
			return NULL;
		}
	}
	size_t n = 0;
	while (at + n < len && p[at + n] != '\0' && n + 1 < size) {
		buf[n] = p[at + n];
		n++;
	}
	buf[n] = '\0';
	return n ? buf : NULL;
}

static bool
prop_u32(IORegistryEntry *e, const char *key, unsigned index, uint32_t *out)
{
	OSData *d = OSDynamicCast(OSData, e->getProperty(key));
	if (d == NULL || d->getLength() < (index + 1) * 4) {
		return false;
	}
	const uint32_t *v = (const uint32_t *)d->getBytesNoCopy();
	*out = v[index];
	return true;
}

IOMemoryMap *
StarbiteTouch::mapNode(const char *name, IOByteCount need)
{
	char path[64];
	snprintf(path, sizeof(path), "/arm-io/%s", name);
	IORegistryEntry *e = IORegistryEntry::fromPath(path, gIODTPlane);
	if (e == NULL) {
		LOG("ADT has no %s", path);
		return NULL;
	}
	IOMemoryMap *map = NULL;
	IOService *nub = OSDynamicCast(IOService, e);
	if (nub != NULL && nub->getDeviceMemoryCount() > 0) {
		map = nub->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
	}
	if (map == NULL) {
		/* no nub (yet): the node's own reg (u64 offset, u64 size) through arm-io's ranges (u64 child,
		 * parent, size), as mkadt-board.py writes them */
		OSData *reg = OSDynamicCast(OSData, e->getProperty("reg"));
		IORegistryEntry *parent = e->getParentEntry(gIODTPlane);
		OSData *ranges = parent ? OSDynamicCast(OSData, parent->getProperty("ranges")) : NULL;
		if (reg != NULL && reg->getLength() >= 16) {
			const uint64_t *r = (const uint64_t *)reg->getBytesNoCopy();
			uint64_t pa = r[0], size = r[1];
			if (ranges != NULL && ranges->getLength() >= 24) {
				const uint64_t *rg = (const uint64_t *)ranges->getBytesNoCopy();
				pa = pa - rg[0] + rg[1];
			}
			IODeviceMemory *mem = IODeviceMemory::withRange((IOPhysicalAddress)pa, (IOPhysicalLength)size);
			if (mem != NULL) {
				map = mem->map(kIOMapInhibitCache);
				mem->release();
			}
		}
	}
	e->release();
	if (map == NULL) {
		LOG("%s: cannot map its reg", path);
		return NULL;
	}
	if (map->getLength() < need) {
		LOG("%s: reg is %#llx bytes, the driver needs %#llx (fix tools/board/starlte.board)", path,
		    (unsigned long long)map->getLength(), (unsigned long long)need);
		map->release();
		return NULL;
	}
	return map;
}

/*
 * The touch node (INTEGRATION.md):
 *   starbite,i2c-bus      "hsi2c-touch"                     the HSI2C block
 *   starbite,i2c-address  <0x48>
 *   starbite,bus-deps     "cmu-peric0" "sysreg-peric0" "pinctrl-peric0"   clock, USI mode, SCL/SDA pins
 *   starbite,irq-gpio     "pinctrl-alive"                   the INT pin's block
 *   starbite,irq-pin      <0x40 0 1>                        bank CON offset (GPA1), pin, active low
 */
bool
StarbiteTouch::mapBlocks(IOService *provider)
{
	char name[48];
	uint32_t addr = SB_S6_I2C_ADDR;

	if (!prop_string(provider, "starbite,i2c-bus", 0, name, sizeof(name))) {
		LOG("touch node has no starbite,i2c-bus");
		return false;
	}
	fBusMap = mapNode(name, 0x100);
	(void)prop_u32(provider, "starbite,i2c-address", 0, &addr);
	fPlat = sb_usi03_touch_bus;
	uint32_t maxGate = fPlat.qch;
	for (unsigned i = 0; i < fPlat.ngates; i++) {
		maxGate = fPlat.gates[i] > maxGate ? fPlat.gates[i] : maxGate;
	}
	if (prop_string(provider, "starbite,bus-deps", 0, name, sizeof(name))) {
		fCmuMap = mapNode(name, maxGate + 4);
	}
	if (prop_string(provider, "starbite,bus-deps", 1, name, sizeof(name))) {
		fSysregMap = mapNode(name, fPlat.usi_off + 4);
	}
	if (prop_string(provider, "starbite,bus-deps", 2, name, sizeof(name))) {
		fPinsMap = mapNode(name, fPlat.pin_bank + 0x10);
	}
	fIrqBank = 0x40;
	fIrqPin = 0;
	fIrqActiveLow = true;
	uint32_t v;
	if (prop_u32(provider, "starbite,irq-pin", 0, &v)) {
		fIrqBank = v;
	}
	if (prop_u32(provider, "starbite,irq-pin", 1, &v)) {
		fIrqPin = v & 31u;
	}
	if (prop_u32(provider, "starbite,irq-pin", 2, &v)) {
		fIrqActiveLow = v != 0;
	}
	if (prop_string(provider, "starbite,irq-gpio", 0, name, sizeof(name))) {
		fIrqMap = mapNode(name, fIrqBank + 8);
	}
	if (fBusMap == NULL || fIrqMap == NULL) {
		LOG("missing bus or INT block: not starting");
		return false;
	}
	if (fCmuMap && fSysregMap && fPinsMap) {
		fPlat.cmu = (uintptr_t)fCmuMap->getVirtualAddress();
		fPlat.sysreg = (uintptr_t)fSysregMap->getVirtualAddress();
		fPlat.gpio = (uintptr_t)fPinsMap->getVirtualAddress();
	} else {
		LOG("bus clock / USI / pin blocks not all mapped: trusting S-Boot's bus setup");
	}
	sb_hsi2c_setup(&fBus, &gTouchMMIO, (uintptr_t)fBusMap->getVirtualAddress(), NULL);
	struct sb_i2c i2c;
	sb_hsi2c_as_i2c(&fBus, &i2c);
	sb_s6_setup(&fChip, &i2c, 1440, 2960, &StarbiteTouch::contactThunk, this);
	fChip.addr = (uint8_t)addr;
	uint32_t panel[2];
	if (prop_u32(provider, "starbite,panel", 0, &panel[0]) && prop_u32(provider, "starbite,panel", 1, &panel[1]) &&
	    panel[0] && panel[1]) {
		fChip.panel_w = panel[0];
		fChip.panel_h = panel[1];
	}
	return true;
}

/* ---- identity, geometry, properties ---- */

static bool
adt_model_is_mac(void)
{
	IORegistryEntry *root = IORegistryEntry::fromPath("/", gIODTPlane);
	bool mac = false;
	if (root != NULL) {
		OSData *model = OSDynamicCast(OSData, root->getProperty("model"));
		mac = model != NULL && model->getLength() >= 3 && memcmp(model->getBytesNoCopy(), "Mac", 3) == 0;
		root->release();
	}
	return mac;
}

/*
 * The shown canvas, from the display's boot-args as today (SweetFramebuffer.cpp): sweetfb_native, and
 * sweetfb_scale with sweetfb_lw x sweetfb_lh drawn full height and centred between black bars.
 */
void
StarbiteTouch::configureGeometry(void)
{
	int pw = (int)fChip.panel_w, ph = (int)fChip.panel_h;
	int native = 0, scale = 0, mode = 0, step = 0;
	uint32_t lw = 0, lh = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	PE_parse_boot_argn("sweetfb_scale", &scale, sizeof(scale));
	PE_parse_boot_argn("sweetfb_lw", &lw, sizeof(lw));
	PE_parse_boot_argn("sweetfb_lh", &lh, sizeof(lh));

	int logicalW = native ? pw : 1125, logicalH = native ? ph : 2436;
	if (scale && lw && lh) {
		logicalH = ph;
		logicalW = (int)((uint64_t)lw * (uint64_t)ph / lh);
		if (logicalW > pw) {
			logicalW = pw;
			logicalH = (int)((uint64_t)lh * (uint64_t)pw / lw);
		}
	}
	struct sb_dig_config dc = { pw, ph, logicalW, logicalH };
	sb_dig_init(&fDigitizer, &dc);

	struct sb_tg_config tc;
	sb_tg_default_config(&tc);
	tc.panel_w = pw;
	tc.panel_h = ph;
	tc.canvas_w = lw ? (int)lw : 828;
	tc.canvas_h = lh ? (int)lh : 1792;
	tc.strip_w = (int)((uint64_t)tc.canvas_w * (uint64_t)ph / (uint64_t)tc.canvas_h);
	if (PE_parse_boot_argn("sbtouchabs", &mode, sizeof(mode)) && mode) {
		tc.mode = SB_TG_ABSOLUTE;
	}
	if (PE_parse_boot_argn("sbtouchscroll", &step, sizeof(step)) && step) {
		tc.scroll_step = step;
	}
	sb_tg_init(&fGesture, &tc);
	if (fPointer) {
		LOG("macOS pointer (%s): panel %dx%d, canvas %dx%d over a %d px strip, scroll %d px/line",
		    fGesture.cfg.mode == SB_TG_ABSOLUTE ? "absolute" : "direct touch", pw, ph, fGesture.cfg.canvas_w,
		    fGesture.cfg.canvas_h, fGesture.cfg.strip_w, fGesture.cfg.scroll_step);
	} else {
		LOG("iOS digitizer: panel %dx%d, shown display %dx%d", pw, ph, logicalW, logicalH);
	}
}

static OSDictionary *
usage_pair(UInt32 page, UInt32 usage)
{
	OSDictionary *pair = OSDictionary::withCapacity(2);
	OSNumber *p = OSNumber::withNumber(page, 32);
	OSNumber *u = OSNumber::withNumber(usage, 32);
	if (pair && p && u) {
		pair->setObject(kIOHIDDeviceUsagePageKey, p);
		pair->setObject(kIOHIDDeviceUsageKey, u);
	}
	OSSafeReleaseNULL(p);
	OSSafeReleaseNULL(u);
	return pair;
}

void
StarbiteTouch::publishModeProperties(void)
{
	fUsagePairs = OSArray::withCapacity(2);
	if (fPointer) {
		/* a Mac pointing device's pairs: the event system treats it as a mouse, not a touch screen */
		OSDictionary *a = usage_pair(kHIDPage_GenericDesktop, kHIDUsage_GD_Mouse);
		OSDictionary *b = usage_pair(kHIDPage_GenericDesktop, kHIDUsage_GD_Pointer);
		if (fUsagePairs && a && b) {
			fUsagePairs->setObject(a);
			fUsagePairs->setObject(b);
		}
		OSSafeReleaseNULL(a);
		OSSafeReleaseNULL(b);
		/* what a Mac USB mouse service publishes for the pointer filter (0378: without a resolution the
		 * acceleration step scaled by zero and the pointer never moved) */
		setProperty("HIDPointerResolution", (unsigned long long)(400 << 16), 32);
		setProperty("HIDScrollResolution", (unsigned long long)(400 << 16), 32);
		setProperty("HIDPointerAccelerationType", "HIDMouseAcceleration");
		setProperty("HIDScrollAccelerationType", "HIDMouseScrollAcceleration");
		setProperty("HIDPointerButtonCount", 2ULL, 32);
		setProperty("HIDPointerReportRate", 120ULL, 32);
	} else {
		OSDictionary *a = usage_pair(kHIDPage_Digitizer, kHIDUsage_Dig_TouchScreen);
		if (fUsagePairs && a) {
			fUsagePairs->setObject(a);
		}
		OSSafeReleaseNULL(a);
		/* the digitizer covers the main display (backboardd maps it 1:1); panel 64 x 132 mm, 16.16 */
		setProperty("DisplayIntegrated", kOSBooleanTrue);
		setProperty("SurfaceWidth", (unsigned long long)(64 << 16), 32);
		setProperty("SurfaceHeight", (unsigned long long)(132 << 16), 32);
	}
	setProperty(kIOHIDBuiltInKey, kOSBooleanTrue);
}

/* ---- IOHIDEventService ---- */

bool
StarbiteTouch::handleStart(IOService *provider)
{
	uint32_t on = 1, mode = 0;
	PE_parse_boot_argn("sbtouch", &on, sizeof(on));
	if (!on) {
		LOG("disabled (sbtouch=0)");
		return false;
	}
	if (!super::handleStart(provider)) {
		return false;
	}
	PE_parse_boot_argn("sbtouchmode", &mode, sizeof(mode));
	fPointer = mode == 1 ? true : (mode == 2 ? false : adt_model_is_mac());
	fPollMS = 5;
	PE_parse_boot_argn("sbtouchpoll", &fPollMS, sizeof(fPollMS));
	fPollMS = fPollMS < 1 ? 1 : (fPollMS > 100 ? 100 : fPollMS);
	fWindowS = 25;
	PE_parse_boot_argn("sbhidwin", &fWindowS, sizeof(fWindowS));
	fWindowS = fWindowS > 60 ? 60 : fWindowS;       /* the chained dead-man is at most 82 s */

	fTransport = OSString::withCString("SPU");
	fManufacturer = OSString::withCString("Starbite");
	fProduct = OSString::withCString(fPointer ? "Starbite Touch Pointer" : "Starbite Touch Screen");
	fWorkLoop = IOWorkLoop::workLoop();
	if (!fTransport || !fManufacturer || !fProduct || !fWorkLoop) {
		return false;
	}
	if (!mapBlocks(provider)) {
		return false;
	}
	configureGeometry();
	publishModeProperties();

	fProbeTimer = IOTimerEventSource::timerEventSource(this, &StarbiteTouch::probeFired);
	fPollTimer = IOTimerEventSource::timerEventSource(this, &StarbiteTouch::pollFired);
	fSteerTimer = IOTimerEventSource::timerEventSource(this, &StarbiteTouch::steerFired);
	fScriptTimer = IOTimerEventSource::timerEventSource(this, &StarbiteTouch::scriptFired);
	fStopTimer = IOTimerEventSource::timerEventSource(this, &StarbiteTouch::stopFired);
	IOTimerEventSource *timers[] = { fProbeTimer, fPollTimer, fSteerTimer, fScriptTimer, fStopTimer };
	for (unsigned i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
		if (timers[i] == NULL || fWorkLoop->addEventSource(timers[i]) != kIOReturnSuccess) {
			return false;
		}
	}
	fGate = IOCommandGate::commandGate(this);
	if (fGate == NULL || fWorkLoop->addEventSource(fGate) != kIOReturnSuccess) {
		return false;
	}
	/* the INT pin as an interrupt once a GPIO interrupt controller (StarbiteGPIO) serves the node's
	 * "interrupts"; until then, polling */
	uint32_t irq = 1;
	int type = 0;
	PE_parse_boot_argn("sbtouchirq", &irq, sizeof(irq));
	if (irq && provider->getInterruptType(0, &type) == kIOReturnSuccess) {
		fIntSource = IOInterruptEventSource::interruptEventSource(this, &StarbiteTouch::interruptFired, provider, 0);
		if (fIntSource && fWorkLoop->addEventSource(fIntSource) != kIOReturnSuccess) {
			OSSafeReleaseNULL(fIntSource);
		}
	}
	return true;
}

bool
StarbiteTouch::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	/* the base class registered us; now the hardware: on the work loop, off the matching thread */
	char script[256];
	if (PE_parse_boot_arg_str("sbtouchscript", script, sizeof(script)) && script[0]) {
		uint32_t onFrame = 0;
		unsigned n = sb_script_parse(&fScript, script);
		PE_parse_boot_argn("sbtouchscriptframe", &onFrame, sizeof(onFrame));
		fScriptOnFrame = onFrame != 0;
		LOG("sbtouchscript: %u steps%s, counted from %s", n, fScript.bad >= 0 ? " (stopped at a bad step)" : "",
		    fScriptOnFrame ? "the first frame" : "now");
		if (n) {
			fScriptTimer->setTimeoutMS(fScriptOnFrame ? 250 : 1);
		}
	}
	fProbeTimer->setTimeoutMS(10);
	__atomic_store_n(&gStarbiteTouch, this, __ATOMIC_RELEASE);
	LOG("registered (%s), bring-up on the work loop", fPointer ? "pointer" : "digitizer");
	return true;
}

void
StarbiteTouch::handleStop(IOService *provider)
{
	__atomic_store_n(&gStarbiteTouch, (StarbiteTouch *)NULL, __ATOMIC_RELEASE);
	if (fGate) {
		if (fWorkLoop) {
			fWorkLoop->removeEventSource(fGate);
		}
	}
	IOTimerEventSource *timers[] = { fProbeTimer, fPollTimer, fSteerTimer, fScriptTimer, fStopTimer };
	for (unsigned i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
		if (timers[i]) {
			timers[i]->cancelTimeout();
			if (fWorkLoop) {
				fWorkLoop->removeEventSource(timers[i]);
			}
		}
	}
	if (fIntSource) {
		fIntSource->disable();
		if (fWorkLoop) {
			fWorkLoop->removeEventSource(fIntSource);
		}
	}
	super::handleStop(provider);
}

void
StarbiteTouch::free(void)
{
	OSSafeReleaseNULL(fProbeTimer);
	OSSafeReleaseNULL(fPollTimer);
	OSSafeReleaseNULL(fSteerTimer);
	OSSafeReleaseNULL(fScriptTimer);
	OSSafeReleaseNULL(fStopTimer);
	OSSafeReleaseNULL(fIntSource);
	OSSafeReleaseNULL(fGate);
	OSSafeReleaseNULL(fBusMap);
	OSSafeReleaseNULL(fCmuMap);
	OSSafeReleaseNULL(fSysregMap);
	OSSafeReleaseNULL(fPinsMap);
	OSSafeReleaseNULL(fIrqMap);
	OSSafeReleaseNULL(fTransport);
	OSSafeReleaseNULL(fManufacturer);
	OSSafeReleaseNULL(fProduct);
	OSSafeReleaseNULL(fUsagePairs);
	OSSafeReleaseNULL(fWorkLoop);
	super::free();
}

IOWorkLoop *
StarbiteTouch::getWorkLoop(void) const
{
	return fWorkLoop;
}

/* The base class publishes these with setProperty (not retained by it): return our own objects. */
OSString *StarbiteTouch::getTransport(void) { return fTransport; }
OSString *StarbiteTouch::getManufacturer(void) { return fManufacturer; }
OSString *StarbiteTouch::getProduct(void) { return fProduct; }
UInt32 StarbiteTouch::getVendorID(void) { return 0x05ac; }
UInt32 StarbiteTouch::getProductID(void) { return 2; }
UInt32 StarbiteTouch::getLocationID(void) { return 0x03000048; }        /* USI03, address 0x48 */
UInt32 StarbiteTouch::getReportInterval(void) { return fPollMS * 1000; }
UInt32 StarbiteTouch::getPrimaryUsagePage(void) { return fPointer ? kHIDPage_GenericDesktop : kHIDPage_Digitizer; }
UInt32 StarbiteTouch::getPrimaryUsage(void) { return fPointer ? kHIDUsage_GD_Mouse : kHIDUsage_Dig_TouchScreen; }
OSArray *StarbiteTouch::getDeviceUsagePairs(void) { return fUsagePairs ? fUsagePairs : super::getDeviceUsagePairs(); }

/* ---- bring-up ---- */

/*
 * Touch supplies through ACPM (0302 st_power_on): tsp_io = S2MPS18 LDO35 (0x6c, 1.8 V = 0x2c), tsp_avdd =
 * LDO43 (0x74, 3.0 V = 0x30). A rail is switched on only when its programmed voltage is the expected one or
 * unset; LDO43 anywhere in 2.8-3.3 V is re-programmed to the DTB's 3.0 V. StarbiteACPM takes this over.
 */
bool
StarbiteTouch::supplyOn(void)
{
#if SB_HAVE_STARLTE_PMIC
	uint8_t id = 0, l35 = 0, l43 = 0;
	if (!starlte_pmic_read(0, 0x00, &id)) {
		LOG("ACPM: no PMIC answer, supplies left as they are");
		return false;
	}
	(void)starlte_pmic_read(1, 0x6c, &l35);
	(void)starlte_pmic_read(1, 0x74, &l43);
	LOG("ACPM: PMIC id %#x, LDO35CTRL %#x, LDO43CTRL %#x", id, l35, l43);
	if ((l35 & 0x3fu) != 0x2cu && (l35 & 0x3fu) != 0) {
		LOG("ACPM: LDO35 voltage %#x unexpected, not touching", l35 & 0x3fu);
		return false;
	}
	if ((l43 & 0x3fu) != 0 && ((l43 & 0x3fu) < 0x28u || (l43 & 0x3fu) > 0x3cu)) {
		LOG("ACPM: LDO43 voltage %#x unexpected, not touching", l43 & 0x3fu);
		return false;
	}
	bool ok = starlte_pmic_write(1, 0x6c, 0xec) && starlte_pmic_write(1, 0x74, 0xf0);
	(void)starlte_pmic_read(1, 0x6c, &l35);
	(void)starlte_pmic_read(1, 0x74, &l43);
	LOG("ACPM: after enable LDO35CTRL %#x, LDO43CTRL %#x (%s)", l35, l43, ok ? "ok" : "FAILED");
	return ok;
#else
	LOG("no PMIC access in this kernel (SB_HAVE_STARLTE_PMIC=0): supplies as S-Boot left them");
	return false;
#endif
}

bool
StarbiteTouch::intLow(void)
{
	uint32_t dat = mmio_rd(NULL, (uintptr_t)fIrqMap->getVirtualAddress(), fIrqBank + 4);
	bool level = ((dat >> fIrqPin) & 1u) != 0;
	return fIrqActiveLow ? !level : level;
}

void
StarbiteTouch::probe(void)
{
	uint32_t pwr = 1;
	PE_parse_boot_argn("sbtouchpwr", &pwr, sizeof(pwr));
	if (fPlat.gpio) {
		unsigned lv = sb_hsi2c_pin_levels(&gTouchMMIO, &fPlat);
		LOG("before bring-up: SCL %u SDA %u INT %s", lv & 1u, (lv >> 1) & 1u, intLow() ? "pending" : "idle");
	}
	if (pwr) {
		bool on = supplyOn();
		IOSleep(150);                           /* the S6SY761 boots its firmware (~100 ms) */
		LOG("supplies %s", on ? "on" : "not switched");
	}
	if (fPlat.cmu && fPlat.sysreg && fPlat.gpio) {
		sb_hsi2c_platform_init(&gTouchMMIO, &fPlat);
		unsigned lv = sb_hsi2c_pin_levels(&gTouchMMIO, &fPlat);
		LOG("USI03 in I2C mode, GPP1-4/5 muxed: SCL %u SDA %u%s", lv & 1u, (lv >> 1) & 1u,
		    lv == 0 ? " (both low: no pull-up power, the tsp_io rail is off)" : "");
	}
	sb_hsi2c_reset(&fBus);
	LOG("hsi2c ctl %#x conf %#x fs1 %#x fs2 %#x fs3 %#x usi %#x",
	    mmio_rd(NULL, fBus.regs, SB_HSI2C_CTL), mmio_rd(NULL, fBus.regs, SB_HSI2C_CONF),
	    mmio_rd(NULL, fBus.regs, SB_HSI2C_FS1), mmio_rd(NULL, fBus.regs, SB_HSI2C_FS2),
	    mmio_rd(NULL, fBus.regs, SB_HSI2C_FS3), mmio_rd(NULL, fBus.regs, SB_HSI2C_USI_CON));

	int r = SB_I2C_ENODEV;
	for (unsigned attempt = 0; attempt < 3 && r != SB_I2C_OK; attempt++) {
		if (attempt) {
			IOSleep(500);
		}
		r = sb_s6_identify(&fChip);
	}
	if (r != SB_I2C_OK) {
		LOG("S6SY761 at %#x does not answer: %s (int %#x trans %#x fifo %#x); touch off, scripted input only",
		    fChip.addr, sb_i2c_strerror(r), fBus.fail_int, fBus.fail_trans, fBus.fail_fifo);
		return;
	}
	LOG("S6SY761: boot status %#04x%s, device id %02x %02x %02x, panel range %u x %u, channels %u x %u",
	    fChip.boot_status, fChip.boot_status == SB_S6_BOOT_APP ? " (application)" : " (NOT application)",
	    fChip.id[0], fChip.id[1], fChip.id[2], fChip.max_x, fChip.max_y, fChip.chan_x, fChip.chan_y);
	int sense = sb_s6_sense_on(&fChip);
	IOSleep(20);
	int clear = sb_s6_clear(&fChip);
	fChipUp = sense == SB_I2C_OK;
	LOG("sense on: %s, event stack cleared: %s; %s", sb_i2c_strerror(sense), sb_i2c_strerror(clear),
	    fIntSource ? "interrupt driven (100 ms safety poll)" : "polling");
}

void
StarbiteTouch::runSelfTest(void)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sbtouchselftest", &on, sizeof(on));
	if (!on) {
		return;
	}
	sb_touch_selftest(fPointer, &fGesture.cfg, &fDigitizer.cfg, fChip.panel_w, fChip.panel_h, &fSelfTest);
	const struct sb_selftest_result *r = &fSelfTest;
	if (fPointer) {
		LOG("self-test %s: %u packets, %u contacts; %u moves, %u abs, button 1 x%u, button 2 x%u, %u scrolls; "
		    "taps %u, drags %u, secondary %u", r->pass ? "PASS" : "FAIL", r->packets, r->contacts, r->rel_moves,
		    r->abs_events, r->presses1, r->presses2, r->scrolls, r->taps, r->drags, r->secondary);
	} else {
		LOG("self-test %s: %u packets, %u contacts; %u digitizer frames, %u from the edge, %u touch changes",
		    r->pass ? "PASS" : "FAIL", r->packets, r->contacts, r->frames, r->edge_frames, r->touch_changes);
	}
}

void
StarbiteTouch::probeFired(OSObject *owner, IOTimerEventSource *sender __unused)
{
	StarbiteTouch *self = (StarbiteTouch *)owner;
	self->probe();
	self->runSelfTest();
	if (self->fChipUp) {
		if (self->fIntSource) {
			self->fIntSource->enable();
		}
		self->fPollTimer->setTimeoutMS(self->fIntSource ? 100 : self->fPollMS);
	}
	self->fLastReport = mach_absolute_time();
#if SB_HAVE_SBSTOP
	if (starlte_sbstop_at("hid")) {
		self->fStopArmed = true;
		self->fStopTimer->setTimeoutMS(self->fWindowS * 1000);
		LOG("sbstop=hid: touch the screen and press a volume key; the result and the stop come in %u s",
		    self->fWindowS);
	}
#endif
}

/* ---- events ---- */

void
StarbiteTouch::contactThunk(void *ctx, int finger, bool down, int x, int y)
{
	((StarbiteTouch *)ctx)->contact(finger, down, x, y);
}

void
StarbiteTouch::contact(int finger, bool down, int x, int y)
{
	if (fInScript) {
		fScriptContacts++;
	} else {
		fLiveContacts++;
		if (fLiveContacts <= 12) {
			LOG("live contact: finger %d %s at %d,%d", finger, down ? "down" : "up", x, y);
		}
	}
	if (fPointer) {
		struct sb_tg_action a[4];
		bool kick = false;
		int n = sb_tg_contact(&fGesture, now_ns(), finger, down, x, y, a, 4, &kick);
		if (fGesture.note && fNotes++ < SB_TOUCH_NOTES_MAX) {
			LOG("pointer: %s (finger %d) at %d,%d", fGesture.note, finger, x, y);
		}
		dispatchActions(a, n);
		if (kick) {
			kickSteer();
		}
	} else {
		struct sb_dig_frame f;
		if (sb_dig_contact(&fDigitizer, finger, down, x, y, &f)) {
			if (f.changed_touch && fNotes++ < SB_TOUCH_NOTES_MAX) {
				LOG("digitizer: finger %d %s at %d,%d%s", finger, down ? "down" : "up", x, y,
				    (f.mask & SB_DIG_FROM_EDGE_TIP) ? " (edge)" : "");
			}
			dispatchFrame(&f);
		}
	}
}

void
StarbiteTouch::dispatchActions(const struct sb_tg_action *a, int n)
{
	uint64_t ts = mach_absolute_time();
	for (int i = 0; i < n; i++) {
		switch (a[i].kind) {
		case SB_TG_REL:
			if (a[i].dx || a[i].dy) {
				fPointerMoves++;
			}
			if (a[i].buttons & ~fRelButtons) {
				fClicks++;
			}
			fRelButtons = a[i].buttons;
			dispatchRelativePointerEvent(ts, a[i].dx, a[i].dy, a[i].buttons);
			break;
		case SB_TG_ABS: {
			/* as 0373: the event itself (dispatchAbsolutePointerEvent's 16.16 scaling overflows above 0x7fff) */
			IOHIDEvent *e = IOHIDEvent::absolutePointerEvent(ts, (IOFixed)a[i].nx, (IOFixed)a[i].ny, 0,
			    a[i].buttons, fAbsButtons);
			fPointerMoves++;
			if (a[i].buttons & ~fAbsButtons) {
				fClicks++;
			}
			fAbsButtons = a[i].buttons;
			if (e) {
				dispatchEvent(e);
				e->release();
			}
			break;
		}
		case SB_TG_SCROLL:
			/* axis 1 is vertical, axis 2 horizontal (IOHIDEventService::dispatchScrollWheelEventWithFixed) */
			fScrolls++;
			dispatchScrollWheelEvent(ts, a[i].lines_y, a[i].lines_x, 0);
			break;
		}
	}
}

/* 0323: a hand collection with a finger child per contact, all display-integrated (iOS 26's touch streams
 * drop "detached" touches). */
void
StarbiteTouch::dispatchFrame(const struct sb_dig_frame *f)
{
	uint64_t ts = mach_absolute_time();
	IOHIDEvent *collection = IOHIDEvent::digitizerEvent(ts, 0, kIOHIDDigitizerTransducerTypeHand, false, 0, 0, 0);
	if (collection == NULL) {
		return;
	}
	collection->setIntegerValue(kIOHIDEventFieldDigitizerCollection, TRUE);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);
	for (int i = 0; i < f->count; i++) {
		const struct sb_dig_contact *c = &f->c[i];
		IOHIDEvent *child = IOHIDEvent::digitizerEvent(ts, c->identity, kIOHIDDigitizerTransducerTypeFinger,
		    c->touch, c->touch ? 1 : 0, (IOFixed)c->nx, (IOFixed)c->ny);
		if (child == NULL) {
			continue;
		}
		child->setIntegerValue(kIOHIDEventFieldDigitizerIsDisplayIntegrated, 1);
		child->setIntegerValue(kIOHIDEventFieldDigitizerIndex, c->index);
		child->setIntegerValue(kIOHIDEventFieldDigitizerIdentity, (SInt32)c->identity);
		child->setIntegerValue(kIOHIDEventFieldDigitizerTouch, c->touch);
		child->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, (SInt32)c->mask);
		collection->appendChild(child);
		child->release();
	}
	collection->setFixedValue(kIOHIDEventFieldDigitizerX, (IOFixed)f->nx);
	collection->setFixedValue(kIOHIDEventFieldDigitizerY, (IOFixed)f->ny);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerRange, f->any);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerTouch, f->any);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, (SInt32)f->mask);
	collection->setIntegerValue(kIOHIDEventFieldDigitizerButtonMask, f->any ? 1 : 0);
	fFrames++;
	dispatchEvent(collection);
	collection->release();
}

void
StarbiteTouch::kickSteer(void)
{
	if (!fSteerActive) {
		fSteerActive = true;
		fSteerTimer->setTimeoutMS(1);
	}
}

void
StarbiteTouch::steerFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteTouch *self = (StarbiteTouch *)owner;
	struct sb_tg_seen seen = { false, 0, 0, 0 };
#if SB_HAVE_SWEET_FB
	seen.valid = true;
	seen.seq = __atomic_load_n(&sweet_fb_cur_seq, __ATOMIC_ACQUIRE);
	seen.x = sweet_fb_cur_x;
	seen.y = sweet_fb_cur_y;
#endif
	struct sb_tg_action a[4];
	bool again = false;
	int n = sb_tg_tick(&self->fGesture, now_ns(), &seen, a, 4, &again);
	if (self->fGesture.note && self->fNotes++ < SB_TOUCH_NOTES_MAX) {
		LOG("pointer: %s (target %d,%d)", self->fGesture.note, self->fGesture.tgt_x, self->fGesture.tgt_y);
	}
	self->dispatchActions(a, n);
	self->fSteerActive = again;
	if (again) {
		sender->setTimeoutMS(16);
	}
}

void
StarbiteTouch::service(void)
{
	fPolls++;
	(void)sb_s6_service(&fChip, intLow());
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - fLastReport, &ns);
	if (ns >= 60ULL * 1000000000ULL) {
		fLastReport = mach_absolute_time();
		report(false);
	}
}

void
StarbiteTouch::pollFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteTouch *self = (StarbiteTouch *)owner;
	self->service();
	sender->setTimeoutMS(self->fIntSource ? 100 : self->fPollMS);
}

void
StarbiteTouch::interruptFired(OSObject *owner, IOInterruptEventSource *sender __unused, int count __unused)
{
	StarbiteTouch *self = (StarbiteTouch *)owner;
	self->fInterrupts++;
	self->service();
}

void
StarbiteTouch::scriptFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteTouch *self = (StarbiteTouch *)owner;
	if (self->fScriptBase == 0) {
#if SB_HAVE_SWEET_FB
		if (self->fScriptOnFrame) {
			if (sweet_fb_first_present == 0) {
				sender->setTimeoutMS(250);
				return;
			}
			self->fScriptBase = sweet_fb_first_present;
			LOG("first frame on the panel: scripted input starts");
		}
#endif
		if (self->fScriptBase == 0) {
			self->fScriptBase = mach_absolute_time();
		}
	}
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - self->fScriptBase, &ns);
	uint64_t ms = ns / 1000000ULL, next = 0;
	int finger, x, y;
	bool down;
	unsigned before = self->fScript.step;
	self->fInScript = true;
	while (sb_script_next(&self->fScript, ms, &finger, &down, &x, &y, &next)) {
		self->contact(finger, down, x, y);
	}
	self->fInScript = false;
	if (self->fScript.step != before) {
		LOG("scripted step %u of %u done", self->fScript.step, self->fScript.n);
	}
	if (next != (uint64_t)-1) {
		sender->setTimeoutMS((uint32_t)(next > ms ? next - ms : 1));
	}
}

void
StarbiteTouch::report(bool final)
{
	LOG("%s: chip %s, polls %u, irqs %u, chip events %u (status %u, bad %u), retries %u, lost %u, clears %u, "
	    "i2c %u ok / %u failed; contacts live %u scripted %u; %s %u, clicks %u, scrolls %u",
	    final ? "result" : "stats", fChipUp ? "up" : "down", fPolls, fInterrupts, fChip.events, fChip.status_events,
	    fChip.bad_events, fChip.retries, fChip.lost, fChip.clears, fBus.xfers, fBus.failures, fLiveContacts,
	    fScriptContacts, fPointer ? "pointer events" : "digitizer frames", fPointer ? fPointerMoves : fFrames,
	    fClicks, fScrolls);
	if (!final) {
		return;
	}
	unsigned keys = 0;
	IOService *buttons = IOService::copyMatchingService(IOService::serviceMatching("StarbiteButtons"));
	if (buttons != NULL) {
		OSNumber *k = OSDynamicCast(OSNumber, buttons->getProperty("StarbiteKeyEvents"));
		keys = k ? k->unsigned32BitValue() : 0;
		buttons->release();
	}
	bool live = fPointer ? (fLiveContacts > 0 && fPointerMoves > 0) : (fLiveContacts > 0 && fFrames > 0);
	bool pass = fChipUp && fChip.boot_status == SB_S6_BOOT_APP && fSelfTest.pass && live;
	LOG("M8 %s %s: chip boot %#04x id %02x%02x%02x, self-test %s, live contacts %u -> %s %u, clicks %u, "
	    "scrolls %u; key events %u%s", pass ? "PASS" : "FAIL", fPointer ? "pointer (the pointer moves)" :
	    "digitizer (touch reaches the HID system)", fChip.boot_status, fChip.id[0], fChip.id[1], fChip.id[2],
	    fSelfTest.pass ? "pass" : "FAIL", fLiveContacts, fPointer ? "pointer events" : "frames",
	    fPointer ? fPointerMoves : fFrames, fClicks, fScrolls, keys, buttons ? "" : " (no StarbiteButtons)");
}

void
StarbiteTouch::stopFired(OSObject *owner, IOTimerEventSource *sender __unused)
{
	StarbiteTouch *self = (StarbiteTouch *)owner;
	self->report(true);
#if SB_HAVE_SBSTOP
	if (self->fStopArmed) {
		starlte_sbstop_now("hid");      /* dumps the console tail to pmsg and resets into TWRP */
	}
#endif
}

/* ---- the legacy entry point (SB_SWEET_HID_COMPAT) ---- */

void
StarbiteTouch::externalContact(int finger, bool down, int x, int y)
{
	fGate->runActionBlock(^IOReturn {
		fInScript = true;               /* legacy callers are scripts (sweetswipe) or the old chip driver */
		contact(finger, down, x, y);
		fInScript = false;
		return kIOReturnSuccess;
	});
}

#if SB_SWEET_HID_COMPAT
extern "C" {
uint64_t sweet_last_input_abs;          /* 0413 stall probe (starlte_touch.c): the last touch down/up */
uint64_t sweet_last_present_abs;        /* written by SweetFramebuffer.cpp */
void sweet_hid_touch(int finger, int down, int x, int y);
}

void
sweet_hid_touch(int finger, int down, int x, int y)
{
	static bool was_down[SB_TG_FINGERS];
	if (finger < 0 || finger >= SB_TG_FINGERS) {
		return;
	}
	if (was_down[finger] != (down != 0)) {
		was_down[finger] = down != 0;
		sweet_last_input_abs = mach_absolute_time();
	}
	StarbiteTouch *t = __atomic_load_n(&gStarbiteTouch, __ATOMIC_ACQUIRE);
	if (t != NULL) {
		t->externalContact(finger, down != 0, x, y);
	}
}
#endif
