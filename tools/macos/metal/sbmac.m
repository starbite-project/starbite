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

static id<MTLDevice> gOurs;

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
		/* MTLAddDevice asserts _deviceArray != NULL: Metal's list must exist first (this is the original
		 * MTLCopyAllDevices: calls from inside the interposing image are not interposed). */
		NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
		BOOL present = NO;
		for (id d in all)
			present |= d == dev;
		void (*add)(id) = (void (*)(id))dlsym(RTLD_DEFAULT, "MTLAddDevice");
		if (!present && add)
			add(dev);
		NSUInteger count = MTLCopyAllDevices().count;
		sblog("Metal device \"%s\" registry id %#llx %s in %.0f ms: MTLCopyAllDevices now has %lu",
		      dev.name.UTF8String, (unsigned long long)dev.registryID,
		      present ? "already listed (kernel accelerator)" : add ? "published with MTLAddDevice" : "NOT published (no MTLAddDevice)",
		      (CFAbsoluteTimeGetCurrent() - t0) * 1000, (unsigned long)count);
	});
}

static NSArray<id<MTLDevice>> *sb_MTLCopyAllDevices(void)
{
	sb_publish();
	return MTLCopyAllDevices();
}

static NSArray<id<MTLDevice>> *sb_MTLCopyAllDevicesWithObserver(id<NSObject> *observer, MTLDeviceNotificationHandler handler)
{
	sb_publish();
	NSArray<id<MTLDevice>> *all = MTLCopyAllDevicesWithObserver(observer, handler);
	static int logged;
	if (!logged++)
		sblog("MTLCopyAllDevicesWithObserver -> %lu device(s) (WindowServer's Metal check needs one)", (unsigned long)all.count);
	return all;
}

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

#define SB_INTERPOSE(replacement, replacee)                                                                      \
	__attribute__((used)) static struct {                                                                    \
		const void *r;                                                                                    \
		const void *e;                                                                                    \
	} sb_interpose_##replacee __attribute__((section("__DATA,__interpose"))) = { (const void *)&replacement, \
										   (const void *)&replacee }

SB_INTERPOSE(sb_MTLCopyAllDevices, MTLCopyAllDevices);
SB_INTERPOSE(sb_MTLCopyAllDevicesWithObserver, MTLCopyAllDevicesWithObserver);
SB_INTERPOSE(sb_MTLCreateSystemDefaultDevice, MTLCreateSystemDefaultDevice);

__attribute__((constructor)) static void sb_loaded(void)
{
	/* CoreAnimation's render server (QuartzCore's CA::WindowServer in WindowServer) reads these at its start,
	 * before Metal is asked: 8-bit BGRA framebuffers, no Apple compression (IOS-26-Hot, NOTES 2026-11-01). */
	setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
	CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	sblog("injected (uid %d)%s", getuid(), getenv("SBMAC_EAGER") ? ", publishing the device now" : "");
	if (getenv("SBMAC_EAGER"))
		sb_publish();
}
