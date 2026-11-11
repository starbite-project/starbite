/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteButtons (see StarbiteButtons.h). Debouncing and the key table are in hw/sb_keys.c (host-tested,
 * test/run.sh); this file maps the pin controller from the ADT, polls it every 10 ms on its own work loop
 * (read only: S-Boot / the firmware left the pins as inputs with their pull-ups) and dispatches the keys.
 *
 * Boot-args:
 *   sbkeys=0               do not start
 *   sbkeyscript=...        scripted presses for unattended test boots: <s><letter>,... with p power, u volume
 *                          up, d volume down, h Bixby/Home; seconds from the driver's start (sb_keys.h)
 */
#include "StarbiteButtons.h"

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>

/* The last Power press, read by today's SweetFramebuffer.cpp to wake the panel. Defined by bsd/dev/a5xelte_keys.c;
 * the integration sets SB_HAVE_A5_POWER_PRESSED=0 once neither exists. */
#ifndef SB_HAVE_A5_POWER_PRESSED
#define SB_HAVE_A5_POWER_PRESSED 1
#endif
#if SB_HAVE_A5_POWER_PRESSED
extern "C" uint64_t a5xelte_power_pressed_at;
#endif
extern "C" uint64_t mach_absolute_time(void);
extern "C" void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);

#ifndef SB_SWEET_HID_COMPAT
#define SB_SWEET_HID_COMPAT 1                   /* define sweet_hid_key() for the legacy callers */
#endif

#define super IOHIDEventService
OSDefineMetaClassAndStructors(StarbiteButtons, IOHIDEventService);

static StarbiteButtons *gStarbiteButtons;

#define LOG(fmt, ...) IOLog("StarbiteButtons: " fmt "\n", ##__VA_ARGS__)

static_assert(SB_HID_PAGE_CONSUMER == kHIDPage_Consumer, "usage page");
static_assert(SB_HID_CSMR_POWER == kHIDUsage_Csmr_Power, "usage");
static_assert(SB_HID_CSMR_MENU == kHIDUsage_Csmr_Menu, "usage");
static_assert(SB_HID_CSMR_VOL_UP == kHIDUsage_Csmr_VolumeIncrement, "usage");
static_assert(SB_HID_CSMR_VOL_DOWN == kHIDUsage_Csmr_VolumeDecrement, "usage");

static inline uint32_t
rd32(uintptr_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

/* /arm-io/<name>'s first reg range, through its nub or (no nub yet) its reg and arm-io's ranges. */
static IOMemoryMap *
map_node(const char *name, IOByteCount need)
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
	if (map != NULL && map->getLength() < need) {
		LOG("%s: reg is %#llx bytes, need %#llx", path, (unsigned long long)map->getLength(),
		    (unsigned long long)need);
		OSSafeReleaseNULL(map);
	}
	return map;
}

/*
 * The keys node (INTEGRATION.md):
 *   starbite,gpio   "pinctrl-alive"
 *   starbite,keys   optional <bank pin usage-page usage> x N (u32); without it the S9 table (hw/sb_keys.c)
 */
bool
StarbiteButtons::loadTable(IOService *provider)
{
	OSData *gpio = OSDynamicCast(OSData, provider->getProperty("starbite,gpio"));
	char name[48];
	if (gpio == NULL || gpio->getLength() < 2 || gpio->getLength() >= sizeof(name)) {
		LOG("keys node has no starbite,gpio");
		return false;
	}
	memcpy(name, gpio->getBytesNoCopy(), gpio->getLength());
	name[gpio->getLength()] = '\0';

	OSData *keys = OSDynamicCast(OSData, provider->getProperty("starbite,keys"));
	if (keys != NULL && keys->getLength() >= 16) {
		const uint32_t *v = (const uint32_t *)keys->getBytesNoCopy();
		static const char letters[] = "pudh0123";
		fCount = keys->getLength() / 16;
		fCount = fCount > SB_KEYS_MAX ? SB_KEYS_MAX : fCount;
		for (unsigned i = 0; i < fCount; i++) {
			snprintf(fNames[i], sizeof(fNames[i]), "key %u", i);
			fDefs[i].bank = v[4 * i];
			fDefs[i].pin = (uint8_t)(v[4 * i + 1] & 31u);
			fDefs[i].usage_page = (uint16_t)v[4 * i + 2];
			fDefs[i].usage = (uint16_t)v[4 * i + 3];
			fDefs[i].letter = letters[i];
			fDefs[i].name = fNames[i];
		}
	} else {
		fCount = sb_keys_starlte_count;
		for (unsigned i = 0; i < fCount; i++) {
			fDefs[i] = sb_keys_starlte[i];
		}
	}
	fBankCount = sb_keys_banks(fDefs, fCount, fBanks, SB_KEYS_BANKS);
	uint32_t need = 0;
	for (unsigned b = 0; b < fBankCount; b++) {
		need = fBanks[b] + 0x10 > need ? fBanks[b] + 0x10 : need;
	}
	fGpioMap = map_node(name, need);
	if (fGpioMap == NULL) {
		return false;
	}
	fGpio = (uintptr_t)fGpioMap->getVirtualAddress();
	return true;
}

uint32_t
StarbiteButtons::readPressed(void)
{
	uint32_t dat[SB_KEYS_BANKS];
	for (unsigned b = 0; b < fBankCount; b++) {
		dat[b] = rd32(fGpio, fBanks[b] + 4);
	}
	return sb_keys_pressed(&fKeys, fBanks, dat, fBankCount);
}

bool
StarbiteButtons::handleStart(IOService *provider)
{
	uint32_t on = 1;
	PE_parse_boot_argn("sbkeys", &on, sizeof(on));
	if (!on) {
		LOG("disabled (sbkeys=0)");
		return false;
	}
	if (!super::handleStart(provider)) {
		return false;
	}
	fTransport = OSString::withCString("SPU");
	fManufacturer = OSString::withCString("Starbite");
	fProduct = OSString::withCString("Starbite Buttons");
	fWorkLoop = IOWorkLoop::workLoop();
	if (!fTransport || !fManufacturer || !fProduct || !fWorkLoop || !loadTable(provider)) {
		return false;
	}
	/* which pins are ours: input (CON nibble 0) or wake-up interrupt (0xF); anything else belongs to someone */
	uint32_t usable = 0;
	for (unsigned i = 0; i < fCount; i++) {
		uint32_t con = rd32(fGpio, fDefs[i].bank);
		if (sb_keys_pin_usable(con, fDefs[i].pin)) {
			usable |= 1u << i;
		} else {
			LOG("%s: bank %#x pin %u is function %#x, not a key: ignored", fDefs[i].name, fDefs[i].bank,
			    fDefs[i].pin, (con >> (fDefs[i].pin * 4)) & 0xfu);
		}
	}
	fKeys.defs = fDefs;
	fKeys.n = fCount;
	sb_keys_init(&fKeys, fDefs, fCount, usable, readPressed());
	for (unsigned b = 0; b < fBankCount; b++) {
		LOG("bank %#x: con %#x dat %#x pud %#x (read only)", fBanks[b], rd32(fGpio, fBanks[b]),
		    rd32(fGpio, fBanks[b] + 4), rd32(fGpio, fBanks[b] + 8));
	}
	if (fKeys.held_at_start) {
		LOG("held at start (ignored until released): %#x", fKeys.held_at_start);
	}

	fPollTimer = IOTimerEventSource::timerEventSource(this, &StarbiteButtons::pollFired);
	fScriptTimer = IOTimerEventSource::timerEventSource(this, &StarbiteButtons::scriptFired);
	if (!fPollTimer || fWorkLoop->addEventSource(fPollTimer) != kIOReturnSuccess ||
	    !fScriptTimer || fWorkLoop->addEventSource(fScriptTimer) != kIOReturnSuccess) {
		return false;
	}
	fGate = IOCommandGate::commandGate(this);
	if (fGate == NULL || fWorkLoop->addEventSource(fGate) != kIOReturnSuccess) {
		return false;
	}
	setProperty(kIOHIDBuiltInKey, kOSBooleanTrue);
	setProperty("StarbiteKeyEvents", 0ULL, 32);
	return true;
}

bool
StarbiteButtons::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	char script[128];
	if (PE_parse_boot_arg_str("sbkeyscript", script, sizeof(script)) && script[0]) {
		unsigned n = sb_keyscript_parse(&fScript, fDefs, fCount, script);
		LOG("sbkeyscript: %u presses%s", n, fScript.bad >= 0 ? " (stopped at a bad step)" : "");
		if (n) {
			fScriptBase = mach_absolute_time();
			fScriptTimer->setTimeoutMS(1);
		}
	}
	fPollTimer->setTimeoutMS(SB_KEYS_POLL_MS);
	__atomic_store_n(&gStarbiteButtons, this, __ATOMIC_RELEASE);
	LOG("registered: %u keys, polled every %u ms", fCount, SB_KEYS_POLL_MS);
	return true;
}

void
StarbiteButtons::handleStop(IOService *provider)
{
	__atomic_store_n(&gStarbiteButtons, (StarbiteButtons *)NULL, __ATOMIC_RELEASE);
	if (fGate && fWorkLoop) {
		fWorkLoop->removeEventSource(fGate);
	}
	IOTimerEventSource *timers[] = { fPollTimer, fScriptTimer };
	for (unsigned i = 0; i < 2; i++) {
		if (timers[i]) {
			timers[i]->cancelTimeout();
			if (fWorkLoop) {
				fWorkLoop->removeEventSource(timers[i]);
			}
		}
	}
	super::handleStop(provider);
}

void
StarbiteButtons::free(void)
{
	OSSafeReleaseNULL(fPollTimer);
	OSSafeReleaseNULL(fScriptTimer);
	OSSafeReleaseNULL(fGate);
	OSSafeReleaseNULL(fGpioMap);
	OSSafeReleaseNULL(fTransport);
	OSSafeReleaseNULL(fManufacturer);
	OSSafeReleaseNULL(fProduct);
	OSSafeReleaseNULL(fWorkLoop);
	super::free();
}

IOWorkLoop *
StarbiteButtons::getWorkLoop(void) const
{
	return fWorkLoop;
}

OSString *StarbiteButtons::getTransport(void) { return fTransport; }
OSString *StarbiteButtons::getManufacturer(void) { return fManufacturer; }
OSString *StarbiteButtons::getProduct(void) { return fProduct; }
UInt32 StarbiteButtons::getVendorID(void) { return 0x05ac; }
UInt32 StarbiteButtons::getProductID(void) { return 1; }
UInt32 StarbiteButtons::getReportInterval(void) { return SB_KEYS_POLL_MS * 1000; }
UInt32 StarbiteButtons::getPrimaryUsagePage(void) { return kHIDPage_Consumer; }
UInt32 StarbiteButtons::getPrimaryUsage(void) { return kHIDUsage_Csmr_ConsumerControl; }

void
StarbiteButtons::key(unsigned index, bool down, bool scripted)
{
	const struct sb_key_def *d = &fDefs[index];
	uint64_t ts = mach_absolute_time();
#if SB_HAVE_A5_POWER_PRESSED
	if (d->usage_page == SB_HID_PAGE_CONSUMER && d->usage == SB_HID_CSMR_POWER && down) {
		a5xelte_power_pressed_at = ts;
	}
#endif
	fEvents++;
	setProperty("StarbiteKeyEvents", (unsigned long long)fEvents, 32);
	LOG("%s %s%s (usage %#x:%#x)", d->name, down ? "down" : "up", scripted ? ", scripted" : "",
	    d->usage_page, d->usage);
	dispatchKeyboardEvent(ts, d->usage_page, d->usage, down ? 1 : 0);
}

void
StarbiteButtons::pollFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteButtons *self = (StarbiteButtons *)owner;
	struct sb_key_change ch[SB_KEYS_MAX];
	int n = sb_keys_sample(&self->fKeys, self->readPressed(), ch, SB_KEYS_MAX);
	for (int i = 0; i < n; i++) {
		self->key(ch[i].key, ch[i].down, false);
	}
	sender->setTimeoutMS(SB_KEYS_POLL_MS);
}

void
StarbiteButtons::scriptFired(OSObject *owner, IOTimerEventSource *sender)
{
	StarbiteButtons *self = (StarbiteButtons *)owner;
	uint64_t ns = 0, next = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - self->fScriptBase, &ns);
	uint64_t ms = ns / 1000000ULL;
	unsigned k;
	bool down;
	while (sb_keyscript_next(&self->fScript, ms, &k, &down, &next)) {
		self->key(k, down, true);
	}
	if (next != (uint64_t)-1) {
		sender->setTimeoutMS((uint32_t)(next > ms ? next - ms : 1));
	}
}

/* ---- the legacy entry point (SB_SWEET_HID_COMPAT) ---- */

void
StarbiteButtons::externalKey(uint16_t usage, bool down)
{
	fGate->runActionBlock(^IOReturn {
		for (unsigned i = 0; i < fCount; i++) {
			if (fDefs[i].usage_page == SB_HID_PAGE_CONSUMER && fDefs[i].usage == usage) {
				key(i, down, true);
				break;
			}
		}
		return kIOReturnSuccess;
	});
}

#if SB_SWEET_HID_COMPAT
/* key: 0 power, 1 volume up, 2 volume down, 3 home (SweetHID.cpp's order) */
extern "C" void sweet_hid_key(int key, int down);

void
sweet_hid_key(int key, int down)
{
	static const uint16_t usages[] = {
		SB_HID_CSMR_POWER, SB_HID_CSMR_VOL_UP, SB_HID_CSMR_VOL_DOWN, SB_HID_CSMR_MENU,
	};
	StarbiteButtons *b = __atomic_load_n(&gStarbiteButtons, __ATOMIC_ACQUIRE);
	if (b != NULL && key >= 0 && key < (int)(sizeof(usages) / sizeof(usages[0]))) {
		b->externalKey(usages[key], down != 0);
	}
}
#endif
