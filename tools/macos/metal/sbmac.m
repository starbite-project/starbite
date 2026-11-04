/*
 * sbmac.dylib: the Starbite injection for macOS 26.5 processes that need the Mali's Metal device,
 * first of all WindowServer. The kernel starts the processes the boot-arg names with
 * DYLD_INSERT_LIBRARIES=/usr/lib/sbmac.dylib and the apple string ptrauth_disabled=1 (xnu 0316 + 0324),
 * e.g. sweetinject=/usr/lib/sbmac.dylib,WindowServer,loginwindow
 *
 * Why WindowServer needs it: SkyLight's WSSystemCanCompositeWithMetal() calls
 * MTLCopyAllDevicesWithObserver(); with an empty array it waits 30 s for a device-added notification
 * and then aborts: "No suitable Metal devices present after 30 seconds." (macOS 27.2 SkyLight,
 * ___WSSystemCanCompositeWithMetal_block_invoke). Metal's own list comes from IOAccelerator services
 * (getMetalPluginClassForService); our kernel has none for macOS by default.
 *
 * What it does: it interposes Metal's three device entry points (MTLCopyAllDevices,
 * MTLCopyAllDevicesWithObserver, MTLCreateSystemDefaultDevice; SkyLight and QuartzCore reach Metal through
 * stubs, so dyld's __interpose table catches them). The first call loads
 * /System/Library/Extensions/StarbiteMetal.bundle (the frontend + metal2vulkan + PanVK), gets its device
 * (sbmtl_device) and publishes it with MTLAddDevice() after Metal's list exists. Then the original call
 * runs and finds it. If the kernel does publish an accelerator for the bundle (pending-macos26/0352),
 * Metal already has the same device object and nothing is added.
 *
 * Log lines "SBMAC[pid name]: ..." on /dev/console.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <objc/runtime.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>
#include <string.h>
#include <pthread.h>

#define SBMAC_BUNDLE "/System/Library/Extensions/StarbiteMetal.bundle/Contents/MacOS/StarbiteMetal"

static void sblog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sblog(const char *fmt, ...)
{
	char msg[512], line[640];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	int n = snprintf(line, sizeof(line), "SBMAC[%d %s]: %s\n", getpid(), getprogname(), msg);
	if (n > (int)sizeof(line) - 1)
		n = (int)sizeof(line) - 1;
	static int fd = -2;
	if (fd == -2)
		fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	if (fd >= 0)
		write(fd, line, (size_t)n);
	if (getenv("SBMTL_STDERR"))
		write(2, line, (size_t)n);
}

/* Catch-all for MTLDevice selectors the bridge device lacks (WindowServer aborted on -peerGroupID,
 * boot 130455): sends it cannot answer are forwarded to this stub, which answers 0/nil and logs the
 * name once. respondsToSelector: stays honest (only real sends are forwarded). */
@interface SBMacZeroStub : NSObject
@end
@implementation SBMacZeroStub
static id sbzero(id me, SEL s, ...) { (void)me; (void)s; return nil; }
+ (BOOL)resolveInstanceMethod:(SEL)sel
{
	sblog("metal: unimplemented device selector %s -> 0", sel_getName(sel));
	class_addMethod(self, sel, (IMP)sbzero, "@16@0:8");
	return YES;
}
@end
static void sbmac_device_fill(id dev)
{
	Class c = object_getClass(dev);
	class_addMethod(c, sel_registerName("peerGroupID"), imp_implementationWithBlock(^uint64_t(id me) { (void)me; return 0; }), "Q16@0:8");
	class_addMethod(c, sel_registerName("peerIndex"), imp_implementationWithBlock(^uint32_t(id me) { (void)me; return 0; }), "I16@0:8");
	class_addMethod(c, sel_registerName("peerCount"), imp_implementationWithBlock(^uint32_t(id me) { (void)me; return 1; }), "I16@0:8");
	static id stub;
	stub = [SBMacZeroStub new];
	SEL fw = @selector(forwardingTargetForSelector:);
	class_addMethod(c, fw, imp_implementationWithBlock(^id(id me, SEL s) {
		(void)me;
		const char *n = sel_getName(s);
		/* only plain getters/MTLDevice-style sends; leave NSObject/runtime machinery alone */
		if (n[0] == '_' || !strncmp(n, "retain", 6) || !strncmp(n, "release", 7)) return nil;
		return stub;
	}), "@24@0:8:16");
	sblog("device %s: peerGroupID/peerIndex/peerCount + zero catch-all", class_getName(c));
}

static id<MTLDevice> gOurs;
static size_t (*sb_status_fn)(char *, size_t);

/* every 10 s while anything changed: the frontend's counters (commits/failures, completed, pipelines, problems,
 * render passes, draws, full-screen passes, biggest pass KiB, dispatches, blits, last problems), and once a
 * minute the process's thread count, so a starved dispatch pool shows */
static void *sb_status_thread(void *arg)
{
	(void)arg;
	char last[400] = "", now[400];
	for (int tick = 0;; tick++) {
		sleep(10);
		sb_status_fn(now, sizeof(now));
		if (strcmp(now, last) || tick % 6 == 0) {
			mach_msg_type_number_t n = 0;
			thread_act_array_t list;
			if (task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS) {
				for (mach_msg_type_number_t i = 0; i < n; i++)
					mach_port_deallocate(mach_task_self(), list[i]);
				vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(*list));
			}
			sblog("metal %s (threads %u)", now, n);
			strlcpy(last, now, sizeof(last));
		}
	}
	return NULL;
}

static void sb_publish(void)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		const char *path = getenv("SBMAC_BUNDLE") ? getenv("SBMAC_BUNDLE") : SBMAC_BUNDLE;
		double t0 = CFAbsoluteTimeGetCurrent();
		void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (!h) {
			sblog("no Metal device: dlopen %s: %s", path, dlerror());
			return;
		}
		id (*device)(mach_port_t) = (id (*)(mach_port_t))dlsym(h, "sbmtl_device");
		size_t (*status)(char *, size_t) = (size_t (*)(char *, size_t))dlsym(h, "sbmtl_status");
		if (!device) {
			sblog("no Metal device: %s has no sbmtl_device", path);
			return;
		}
		id<MTLDevice> dev = device(MACH_PORT_NULL);
		if (!dev) {
			char why[300] = "?";
			if (status)
				status(why, sizeof(why));
			sblog("no Metal device: %s", why);
			return;
		}
		gOurs = dev;
		/*
		 * SkyLight's compositor is keyed by the device's acceleratorPort: WSGetDefaultMetalAccelerator takes
		 * [MTLCopyAllDevices().firstObject acceleratorPort], SLCADisplay::is_accelerated() is "that != 0", and
		 * CompositorMetal::new_compositor(accel) picks the device whose acceleratorPort equals it. The frontend's
		 * device (a bare _MTLDevice subclass) answered 0: no compositor, the display never accelerated, every
		 * frame black (26.5 boots 102422-123708). Answer a real service port: the StarbiteMali GPU's.
		 */
		sbmac_device_fill(dev);
		if (!(getenv("SBMAC_ACCEL_PORT") && !strcmp(getenv("SBMAC_ACCEL_PORT"), "0"))) {
			io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("StarbiteMali"));
			if (svc) {
				static io_service_t port;
				port = svc;	/* kept: the name stays valid for the process */
				SEL sel = sel_registerName("acceleratorPort");
				IMP imp = imp_implementationWithBlock(^io_service_t(id me) { (void)me; return port; });
				Method m = class_getInstanceMethod(object_getClass(dev), sel);
				class_replaceMethod(object_getClass(dev), sel, imp, m ? method_getTypeEncoding(m) : "I16@0:8");
				sblog("acceleratorPort -> %#x (StarbiteMali service): SkyLight's compositor key", port);
			} else
				sblog("acceleratorPort: no StarbiteMali service, left as it is");
		}
		/* MTLAddDevice asserts _deviceArray != NULL: Metal's list must exist first (this is the original
		 * MTLCopyAllDevices: calls from inside the interposing image are not interposed). */
		NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
		BOOL present = NO;
		for (id d in all)
			present |= d == dev;
		void (*add)(id) = (void (*)(id))dlsym(RTLD_DEFAULT, "MTLAddDevice");
		if (!present && add)
			add(dev);
		/* every 10 s while anything changed: the frontend's counters (commits/failures, completed, pipelines,
		 * problems, render passes, draws, full-screen passes, biggest pass KiB, dispatches, blits, last problems):
		 * whether WindowServer composites through the Mali at all (SBMAC_STATUS=0 turns it off) */
		if (status && !(getenv("SBMAC_STATUS") && !strcmp(getenv("SBMAC_STATUS"), "0"))) {
			/* a plain pthread, not a dispatch queue: if WindowServer's work-queue threads are all blocked, a
			 * dispatch block would never run (boot 123708 printed no status at all) */
			sb_status_fn = status;
			pthread_t t;
			if (pthread_create(&t, NULL, sb_status_thread, NULL) == 0)
				pthread_detach(t);
		}
		NSUInteger count = MTLCopyAllDevices().count;
		sblog("Metal device \"%s\" registry id %#llx %s in %.0f ms: MTLCopyAllDevices now has %lu",
		      dev.name.UTF8String, (unsigned long long)dev.registryID,
		      present ? "already listed (kernel accelerator)" : add ? "published with MTLAddDevice" : "NOT published (no MTLAddDevice)",
		      (CFAbsoluteTimeGetCurrent() - t0) * 1000, (unsigned long)count);
	});
}

/* NS_RETURNS_RETAINED like the originals: the callers release what they get */
static NSArray<id<MTLDevice>> *sb_MTLCopyAllDevices(void) NS_RETURNS_RETAINED;
static NSArray<id<MTLDevice>> *sb_MTLCopyAllDevices(void)
{
	sb_publish();
	return MTLCopyAllDevices();
}

static NSArray<id<MTLDevice>> *sb_MTLCopyAllDevicesWithObserver(id<NSObject> __strong *observer, MTLDeviceNotificationHandler handler) NS_RETURNS_RETAINED;
static NSArray<id<MTLDevice>> *sb_MTLCopyAllDevicesWithObserver(id<NSObject> __strong *observer, MTLDeviceNotificationHandler handler)
{
	sb_publish();
	NSArray<id<MTLDevice>> *all = MTLCopyAllDevicesWithObserver(observer, handler);
	static int logged;
	if (!logged++)
		sblog("MTLCopyAllDevicesWithObserver -> %lu device(s) (WindowServer's Metal check needs one)", (unsigned long)all.count);
	return all;
}

static id<MTLDevice> sb_MTLCreateSystemDefaultDevice(void) NS_RETURNS_RETAINED;
static id<MTLDevice> sb_MTLCreateSystemDefaultDevice(void)
{
	sb_publish();
	id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
	if (!dev && gOurs) {
		/* Metal's default comes from its own discovery (the display's GPU); ours is the only one */
		dev = gOurs;
	}
	return dev;
}

/*
 * ColorSync's display-profile update. WindowServer's WS::Displays::CAWSManager::restore_color_preferences
 * block calls it (looked up with dlsym) and signals a semaphore that the session code waits on; it asks
 * the com.apple.colorsync.displayservices daemon synchronously over XPC (ColorSyncXPCAcquireDisplayInfo).
 * On the card root that reply never came (26.5 boot 20261104-051706: WindowServer thread in
 * ColorSync+0x664b4 <- SkyLight+0x442ad4, loginwindow blocked in CreateSessionWithAuditSessionID).
 * Answer "no profile info" (NULL: SkyLight checks it) at once; SBMAC_COLORSYNC=1 calls the real one.
 */
extern CFTypeRef ColorSyncDisplayServicesUpdateProfiles(void *a, void *b, void *c);
static CFTypeRef sb_ColorSyncDisplayServicesUpdateProfiles(void *a, void *b, void *c)
{
	if (getenv("SBMAC_COLORSYNC"))
		return ColorSyncDisplayServicesUpdateProfiles(a, b, c);
	static int told;
	if (!told++)
		sblog("ColorSyncDisplayServicesUpdateProfiles: answered NULL without asking colorsync.displayservices");
	return NULL;
}

#define SB_INTERPOSE(replacement, replacee)                                                                      \
	__attribute__((used)) static struct {                                                                    \
		const void *r;                                                                                    \
		const void *e;                                                                                    \
	} sb_interpose_##replacee __attribute__((section("__DATA,__interpose"))) = { (const void *)&replacement, \
										   (const void *)&replacee }

SB_INTERPOSE(sb_MTLCopyAllDevices, MTLCopyAllDevices);
SB_INTERPOSE(sb_MTLCopyAllDevicesWithObserver, MTLCopyAllDevicesWithObserver);
SB_INTERPOSE(sb_MTLCreateSystemDefaultDevice, MTLCreateSystemDefaultDevice);
SB_INTERPOSE(sb_ColorSyncDisplayServicesUpdateProfiles, ColorSyncDisplayServicesUpdateProfiles);

/*
 * CA::WindowServer::IOMFBDisplay::framebuffer_pixel_format() -> 'BGRA'.
 * WindowServer composites into the display surface CoreAnimation allocates in this format. macOS 26.5's
 * CA picks 'b3a8'/'w4a0'/'RGhA' (wide colour, EDR) from the display's answers, SkyLight's
 * WSCompositeDestinationCreateWithIOSurface takes only 'BGRA', 'RGBA', 'RGfA', 32, 'R10k', 'l10r', 'b64a'
 * and aborts ("Unimplemented pixel format of %i") on anything else (WindowServer SIGABRT every ~40 s,
 * SLCADisplay::render_update), and metal2vulkan has no 'b3a8' either. sbca does the same on iOS 26.
 * The function's offset from QuartzCore's Mach-O header in the 26.5 (25F71) cache: 0x352ae0
 * (__ZN2CA12WindowServer12IOMFBDisplay24framebuffer_pixel_formatEv); its first three instructions
 * are checked before anything is written, so another build is left alone (and logged).
 */
#define QC_FRAMEBUFFER_PIXEL_FORMAT_26_5 0x352ae0
static const uint32_t fb_format_was[3] = { 0xd503237fu, 0x6dbc23e9u, 0xa90157f6u };	/* pacibsp; stp d9,d8; stp x22,x21 */
static const uint32_t fb_format_bgra[3] = { 0x528a4820u, 0x72a848e0u, 0xd65f03c0u };	/* mov w0,#'RA'; movk 'BG'; ret */

static void sb_patch_pixel_format(const struct mach_header *mh)
{
	uint32_t *at = (uint32_t *)((uintptr_t)mh + (getenv("SBMAC_QC_FBFORMAT") ? strtoul(getenv("SBMAC_QC_FBFORMAT"), NULL, 0) :
						      QC_FRAMEBUFFER_PIXEL_FORMAT_26_5));
	if (at[0] == fb_format_bgra[0] && at[1] == fb_format_bgra[1])
		return;
	if (memcmp(at, fb_format_was, sizeof(fb_format_was)) != 0) {
		sblog("framebuffer_pixel_format: found %#x %#x %#x, not 26.5's prologue: left alone", at[0], at[1], at[2]);
		return;
	}
	vm_address_t page = (vm_address_t)at & ~(vm_address_t)(vm_page_size - 1);
	vm_size_t len = ((vm_address_t)(at + 3) - page + vm_page_size - 1) & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr = vm_protect(mach_task_self(), page, len, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		sblog("framebuffer_pixel_format: cannot make the code writable (%d)", kr);
		return;
	}
	for (int i = 2; i >= 0; i--)	/* the last instruction first: never half patched */
		at[i] = fb_format_bgra[i];
	kr = vm_protect(mach_task_self(), page, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(at, sizeof(fb_format_bgra));
	sblog("CA::WindowServer::IOMFBDisplay::framebuffer_pixel_format -> 'BGRA' (patched, r-x again: %d)", kr);
}


/* WindowServer crashed in MPSCore (+0x88f44, a NULL MPS device record: MPS has no entry for the Mali)
 * from SkyLight's MPSImage filters at the login -> desktop switch (boots 143146, 144945). Every MPS
 * kernel's -initWithDevice... answers nil instead: SkyLight's encode calls then go to nil (no blur,
 * no crash). SBMAC_MPS=1 leaves MPS alone. */
static void *sb_mps_nil(void *me, SEL s, ...) { (void)me; (void)s; return NULL; }
static void sb_mps_disable(void)
{
	if (getenv("SBMAC_MPS") && !strcmp(getenv("SBMAC_MPS"), "1"))
		return;
	Class kernel = objc_getClass("MPSKernel");
	if (!kernel)
		return;
	static int patched;
	unsigned n = 0;
	Class *all = objc_copyClassList(&n);
	for (unsigned i = 0; i < n; i++) {
		Class c = all[i];
		Class k = c;
		while (k && k != kernel)
			k = class_getSuperclass(k);
		if (!k)
			continue;
		unsigned m = 0;
		Method *ms = class_copyMethodList(c, &m);
		for (unsigned j = 0; j < m; j++) {
			const char *name = sel_getName(method_getName(ms[j]));
			if (strncmp(name, "initWithDevice", 14) && strncmp(name, "initWithCoder:device:", 21))
				continue;
			if (method_getImplementation(ms[j]) == (IMP)sb_mps_nil)
				continue;
			method_setImplementation(ms[j], (IMP)sb_mps_nil);
			patched++;
		}
		free(ms);
	}
	free(all);
	sblog("MPS: %d initWithDevice methods answer nil (no MPS on the Mali)", patched);
}

static void sb_image_added(const struct mach_header *mh, intptr_t slide)
{
	(void)slide;
	Dl_info info;
	if (dladdr(mh, &info) && info.dli_fname && strstr(info.dli_fname, "/QuartzCore.framework/") &&
	    !strcmp(getprogname(), "WindowServer") && !getenv("SBMAC_NO_FBFORMAT"))
		sb_patch_pixel_format(mh);
	if (dladdr(mh, &info) && info.dli_fname && strstr(info.dli_fname, "/MetalPerformanceShaders.framework/"))
		sb_mps_disable();
}

__attribute__((constructor)) static void sb_loaded(void)
{
	_dyld_register_func_for_add_image(sb_image_added);
	/* CoreAnimation's render server (QuartzCore's CA::WindowServer in WindowServer) reads these at its start,
	 * before Metal is asked: 8-bit BGRA framebuffers, no Apple compression (IOS-26-Hot, NOTES 2026-11-01). */
	setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
	CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	sblog("injected (uid %d)%s", getuid(), getenv("SBMAC_EAGER") ? ", publishing the device now" : "");
	if (getenv("SBMAC_EAGER"))
		sb_publish();
	sb_mps_disable();
}
