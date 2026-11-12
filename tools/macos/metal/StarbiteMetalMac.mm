/*
 * StarbiteMetal.bundle for macOS 26.5 on the Galaxy S9: the Metal device for the Mali-G72 MP18.
 *
 * The same stack as iOS 26's (tools/metal2/HotMetal2.mm, from IOS-26-Hot, MIT): the frontend
 * (tools/metal2/NativeBridgeMetalFrontend.mm: a dynamic subclass of the process's own _MTLDevice and
 * NB* objects) over the metal2vulkan bridge (Rust, AIR -> SPIR-V and the Metal object model on Vulkan),
 * over PanVK (Contents/Frameworks/libvulkan_panfrost.dylib, Mesa 26.2.6), which reaches the GPU through
 * src/starbite-drm -> the kernel's StarbiteMali user client. Built for the macOS platform (dyld refuses
 * iOS-platform images in a macOS process). The code is plain arm64 (no pointer authentication: the S9's
 * Cortex-A75/A55 have none, and the root's a64e rewrite leaves no PAC in any Apple binary either); build.sh
 * marks the finished images arm64e (tools/macos/metal/arm64e-tag.py), so dyld's arm64e grading takes them
 * into arm64e processes such as WindowServer without ptrauth_disabled=1.
 *
 * Two ways in, one device per process:
 *  - Metal's own discovery (the driver path, macOS Sequoia): the kernel publishes an IOAccelerator whose
 *    MetalPluginName is "StarbiteMetal" and MetalPluginClassName "StarbiteMTLDevice" (xnu 0361, sbmtl=2);
 *    Metal finds the bundle with gpu_bundle_find_trusted() (SIP off in the ADT), checks the class is an
 *    _MTLDevice and calls -[StarbiteMTLDevice initWithAcceleratorPort:] with that service.
 *  - sbmac.dylib (the older injection, tools/macos/metal/sbmac.m; Ventura, macOS 26.5) dlopens this bundle
 *    and calls sbmtl_device(), then MTLAddDevice().
 *
 * Who gets a device: the names in /private/var/local/mtl2-clients ("*" = all, "-NAME" = never NAME),
 * or, with no such file, WindowServer and sbmtltest (env STARBITE_MTL_CLIENTS replaces the default).
 * Log: /dev/console (0622 on our kernel's devfs) and /private/var/local/mtl2.log when that file exists.
 * Lines "SBMTL[pid name]: ...".
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static char sbmtl_error[256];

extern "C" void sm2_logs(const char *s)
{
	char buf[700];
	int n = snprintf(buf, sizeof(buf), "SBMTL[%d %s]: %s\n", getpid(), getprogname(), s);
	if (n > (int)sizeof(buf) - 1)
		n = (int)sizeof(buf) - 1;
	static int console = -2, file = -2;
	if (console == -2)
		console = getenv("SBMTL_NO_CONSOLE") ? -1 : open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	if (file == -2)
		file = open("/private/var/local/mtl2.log", O_WRONLY | O_APPEND);
	if (console >= 0)
		write(console, buf, (size_t)n);
	if (file >= 0)
		write(file, buf, (size_t)n);
	if (getenv("SBMTL_STDERR"))
		write(2, buf, (size_t)n);
}

static void sm2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sm2_log(const char *fmt, ...)
{
	char buf[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sm2_logs(buf);
}

/* The Mali-G72 does not snoop the CPU caches: libSystem's sys_dcache_flush (a no-op on Apple GPUs) must really
 * clean and invalidate. EL0 may (SCTLR_EL1.UCI on our kernel, as for iOS 26). Wins at link time for the bridge
 * and the frontend. */
extern "C" void sys_dcache_flush(void *start, size_t len)
{
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Metal validation failures abort by default; frameworks with Apple-GPU-only kernels report one on any other device. */
__attribute__((constructor)) static void sm2_error_mode(void)
{
	void (*set)(unsigned, unsigned) = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "MTLFailureTypeSetErrorModeType");
	for (unsigned t = 0; set && t < 5; t++)
		set(t, 5);	/* 5 = NSLog */
	sm2_log("bundle loaded");
}

static void missing(const char *cls, const char *sel)
{
	static NSMutableSet *seen;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		seen = [NSMutableSet new];
	});
	NSString *k = [NSString stringWithFormat:@"%s %s", cls, sel];
	@synchronized(seen) {
		if ([seen containsObject:k])
			return;
		[seen addObject:k];
	}
	sm2_log("MISSING %s -%s", cls, sel);
}

/* Refusals the iOS plugin learned (HotMetal2.mm): no Apple pipeline archives, no stitched or dynamic libraries. */
@interface NBDeviceMethods : NSObject
@end
@interface NBDeviceMethods (StarbiteMac)
@end
@implementation NBDeviceMethods (StarbiteMac)
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	(void)path;
	if (error)
		*error = [NSError errorWithDomain:@"StarbiteMetal" code:1 userInfo:@{ NSLocalizedDescriptionKey : @"no pipeline libraries" }];
	return nil;
}
- (id)newLibraryWithStitchedDescriptor:(id)descriptor error:(NSError **)error
{
	(void)descriptor;
	if (error)
		*error = [NSError errorWithDomain:@"StarbiteMetal" code:2 userInfo:@{ NSLocalizedDescriptionKey : @"stitched libraries are not supported" }];
	return nil;
}
- (void)newLibraryWithStitchedDescriptor:(id)descriptor completionHandler:(void (^)(id, NSError *))completionHandler
{
	(void)descriptor;
	NSError *err = [NSError errorWithDomain:@"StarbiteMetal" code:2 userInfo:@{ NSLocalizedDescriptionKey : @"stitched libraries are not supported" }];
	if (completionHandler)
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ completionHandler(nil, err); });
}
- (id)newDynamicLibrary:(id)library error:(NSError **)error
{
	(void)library;
	if (error)
		*error = [NSError errorWithDomain:@"StarbiteMetal" code:3 userInfo:@{ NSLocalizedDescriptionKey : @"dynamic libraries are not supported" }];
	return nil;
}
- (id)newDynamicLibraryWithURL:(NSURL *)url error:(NSError **)error
{
	(void)url;
	if (error)
		*error = [NSError errorWithDomain:@"StarbiteMetal" code:3 userInfo:@{ NSLocalizedDescriptionKey : @"dynamic libraries are not supported" }];
	return nil;
}
@end

static bool listed(void)
{
	char text[2048] = { 0 };
	FILE *f = fopen("/private/var/local/mtl2-clients", "r");
	if (f) {
		size_t n = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
		text[n] = 0;
	} else {
		const char *env = getenv("STARBITE_MTL_CLIENTS");
		strlcpy(text, env ? env : "WindowServer sbmtltest", sizeof(text));
	}
	for (char *c = text; (c = strchr(c, '#')) != NULL;)	/* "#" comments to the end of the line */
		while (*c && *c != '\n')
			*c++ = ' ';
	char *save = NULL;
	bool yes = false;
	for (char *name = strtok_r(text, " ,\t\r\n", &save); name; name = strtok_r(NULL, " ,\t\r\n", &save)) {
		if (name[0] == '-' && !strcmp(name + 1, getprogname()))
			return false;
		if (!strcmp(name, "*") || !strcmp(name, getprogname()))
			yes = true;
	}
	return yes;
}

/* A writable shader-cache directory if there is one; the runtime is told a read-only one is fine
 * (tools/macos/metal/patches/m2v-0001: METAL2VULKAN_CACHE_OPTIONAL=1, the card root is read-only). */
static void cache_dir(char *out, size_t size)
{
	const char *tries[] = { getenv("STARBITE_M2V_CACHE"), "/private/var/local/m2v-cache", "/private/var/tmp/m2v-cache",
				"/private/tmp/m2v-cache" };
	for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
		if (!tries[i])
			continue;
		mkdir(tries[i], 0777);
		snprintf(out, size, "%s/%s", tries[i], getprogname());
		mkdir(out, 0777);
		if (access(out, W_OK) == 0)
			return;
	}
	snprintf(out, size, "/private/var/local/m2v-cache/%s", getprogname());
	sm2_log("no writable shader cache directory: translations stay in memory (%s)", out);
}

/* The registry entry the device names as its own (MTLDevice.registryID): the kernel's StarbiteMali service. */
static uint64_t gpu_registry_id(mach_port_t port)
{
	uint64_t id = 0;
	if (port && IORegistryEntryGetRegistryEntryID(port, &id) == KERN_SUCCESS)
		return id;
	if (const char *forced = getenv("STARBITE_REGISTRY_ID"))	/* host smoke tests: no StarbiteMali on a Mac */
		return strtoull(forced, NULL, 0);
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("StarbiteMali"));
	if (s) {
		IORegistryEntryGetRegistryEntryID(s, &id);
		IOObjectRelease(s);
	}
	return id;
}

static id open_device(mach_port_t port)
{
	if (!listed()) {
		snprintf(sbmtl_error, sizeof(sbmtl_error), "%s is not a listed client (/private/var/local/mtl2-clients)", getprogname());
		sm2_log("%s: no Metal device", sbmtl_error);
		return nil;
	}
	Dl_info me = { 0 };
	dladdr((void *)sm2_logs, &me);
	/* <bundle>/Contents/MacOS/StarbiteMetal -> <bundle>/Contents/Frameworks/libvulkan_panfrost.dylib */
	char driver[1024];
	strlcpy(driver, me.dli_fname ? me.dli_fname : "/System/Library/Extensions/StarbiteMetal.bundle/Contents/MacOS/StarbiteMetal",
		sizeof(driver));
	char *slash = strrchr(driver, '/');
	if (slash)
		*slash = 0;
	slash = strrchr(driver, '/');
	if (slash)
		*slash = 0;
	strlcat(driver, "/Frameworks/libvulkan_panfrost.dylib", sizeof(driver));
	if (getenv("STARBITE_VK_DRIVER"))
		strlcpy(driver, getenv("STARBITE_VK_DRIVER"), sizeof(driver));
	setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);	/* Bifrost v6: experimental in PanVK */
	setenv("MESA_VK_VERSION_OVERRIDE", "1.2", 0);		/* metal2vulkan's floor */
	setenv("METAL2VULKAN_CACHE_OPTIONAL", "1", 0);
	char cache[512];
	cache_dir(cache, sizeof(cache));
	/* plain 8-bit BGRA rows: no Apple framebuffer compression, no 10-bit deep framebuffer ('b3a8' on iOS 26) */
	setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
	CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	if (FILE *ef = fopen("/private/var/local/mtl2-env", "r")) {
		char line[256];
		while (fgets(line, sizeof(line), ef)) {
			line[strcspn(line, "\r\n")] = 0;
			char *eq = strchr(line, '=');
			if (line[0] != '#' && eq) {
				*eq = 0;
				setenv(line, eq + 1, 1);
				sm2_log("env %s=%s", line, eq + 1);
			}
		}
		fclose(ef);
	}
	uint64_t registryID = gpu_registry_id(port);
	if (!registryID) {
		snprintf(sbmtl_error, sizeof(sbmtl_error), "no StarbiteMali service (kernel without the Mali user client?)");
		sm2_log("%s", sbmtl_error);
		return nil;
	}
	double t0 = CFAbsoluteTimeGetCurrent();
	id device = nil;
	@try {
		device = native_bridge_metal_open(driver, cache, registryID, missing);
	} @catch (NSException *e) {
		sm2_log("device: exception %s", e.reason.UTF8String);
		device = nil;
	}
	if (!device)
		snprintf(sbmtl_error, sizeof(sbmtl_error), "device refused: %s", nmb_last_error());
	{
		/* load addresses, so "SMP" samples (sweetsample=) of these images can be symbolicated */
		Dl_info vk = { 0 };
		void *h = dlopen(driver, RTLD_NOLOAD | RTLD_LAZY);	/* already loaded by the bridge (RTLD_LOCAL): no new load */
		void *gipa = h ? dlsym(h, "vkGetInstanceProcAddr") : NULL;
		if (gipa)
			dladdr(gipa, &vk);
		sm2_log("images: StarbiteMetal %p, libvulkan_panfrost %p", me.dli_fbase, vk.dli_fbase);
	}
	sm2_log("device %s in %.0f ms (driver %s, cache %s, registry id %#llx)%s%s",
		device ? [[device name] UTF8String] : "refused", (CFAbsoluteTimeGetCurrent() - t0) * 1000, driver, cache,
		(unsigned long long)registryID, device ? "" : ": ", device ? "" : nmb_last_error());
	return device;
}

/* Device selectors the frontend lacks: answered 0/nil and logged once each, instead of
 * doesNotRecognizeSelector's exception (WindowServer aborted on -peerGroupID, macOS 26.5 boot 130455).
 * respondsToSelector: stays honest: only real sends are forwarded here. */
@interface SBMTLZeroStub : NSObject
@end
@implementation SBMTLZeroStub
static id sbmtl_zero(id me, SEL s, ...)
{
	(void)me;
	(void)s;
	return nil;
}
+ (BOOL)resolveInstanceMethod:(SEL)sel
{
	sm2_log("device: unimplemented selector -%s -> 0", sel_getName(sel));
	class_addMethod(self, sel, (IMP)sbmtl_zero, "@16@0:8");
	return YES;
}
@end

/*
 * What the device answers beyond the frontend (sbmac.dylib added these from outside on Ventura and 26.5; they
 * are the driver's own answers now):
 *  - acceleratorPort: SkyLight keys its compositor by it (WSGetDefaultMetalAccelerator takes the first
 *    device's acceleratorPort, SLCADisplay::is_accelerated() is "!= 0", CompositorMetal picks the device
 *    whose port matches); the bare _MTLDevice answers 0, and every frame stayed black (26.5 boots
 *    102422-123708). Apple's devices answer their IOAccelerator service: so does this one (the port Metal
 *    passed to -initWithAcceleratorPort:, held with our own reference), else the StarbiteMali service.
 *  - peerGroupID 0, peerIndex 0, peerCount 1: one GPU, no peer group.
 *  - forwardingTargetForSelector: the zero catch-all above.
 */
/*
 * "new..." methods hand their caller a +1 reference, but a block used as a method (imp_implementationWithBlock) returns
 * its object autoreleased under ARC (+0): one extra retain makes the caller own exactly one (S2n 2026-11-12: without it
 * CoreAnimation released the event it got from -newSharedEventWithMachPort: once too often, and WindowServer died in the
 * autorelease pool's pop).
 */
static id sbmtl_owned(id obj)
{
	if (obj)
		(void)CFBridgingRetain(obj);
	return obj;
}

static void fill_device(id dev, mach_port_t port)
{
	Class c = object_getClass(dev);
	class_addMethod(c, sel_registerName("peerGroupID"), imp_implementationWithBlock(^uint64_t(id me) { (void)me; return 0; }), "Q16@0:8");
	class_addMethod(c, sel_registerName("peerIndex"), imp_implementationWithBlock(^uint32_t(id me) { (void)me; return 0; }), "I16@0:8");
	class_addMethod(c, sel_registerName("peerCount"), imp_implementationWithBlock(^uint32_t(id me) { (void)me; return 1; }), "I16@0:8");
	static id stub;
	stub = [SBMTLZeroStub new];
	/*
	 * Sequoia's Metal hands a plugin device its service object with -_setAcceleratorService: (S2j 2026-11-12:
	 * "MISSING NativeBridgeMetalDevice -_setAcceleratorService:", then doesNotRecognizeSelector aborted WindowServer).
	 * Kept, and answered by the getters Apple's IOAccel devices have; any other one-argument private setter
	 * (-_setX:) is a no-op through the stub instead of an exception. Other private selectors still raise.
	 */
	/*
	 * Shared events: _MTLDevice's own constructors make an _MTLSharedEvent, an IOSurfaceSharedEvent whose value page
	 * our kernel's IOSurface service does not provide (S2m 2026-11-12: WindowServer died reading it, far 0). Every
	 * constructor answers with this device's own events instead. An import (mach port / handle) cannot be honoured
	 * across processes yet: it gets a fresh local event, logged once.
	 */
	class_replaceMethod(c, sel_registerName("newSharedEventWithOptions:"), imp_implementationWithBlock(^id(id me, NSUInteger o) {
		(void)o;
		return sbmtl_owned([(id<MTLDevice>)me newSharedEvent]);
	}), "@24@0:8Q16");
	class_replaceMethod(c, sel_registerName("newEventWithOptions:"), imp_implementationWithBlock(^id(id me, NSUInteger o) {
		(void)o;
		return sbmtl_owned([(id<MTLDevice>)me newEvent]);
	}), "@24@0:8Q16");
	class_replaceMethod(c, sel_registerName("newSharedEventWithMachPort:"), imp_implementationWithBlock(^id(id me, mach_port_t p) {
		(void)p;
		missing(class_getName(object_getClass(me)), "newSharedEventWithMachPort: (local event)");
		return sbmtl_owned([(id<MTLDevice>)me newSharedEvent]);
	}), "@20@0:8I16");
	class_replaceMethod(c, sel_registerName("newSharedEventWithHandle:"), imp_implementationWithBlock(^id(id me, id h) {
		(void)h;
		missing(class_getName(object_getClass(me)), "newSharedEventWithHandle: (local event)");
		return sbmtl_owned([(id<MTLDevice>)me newSharedEvent]);
	}), "@24@0:8@16");
	static const char kAccelServiceKey = 0;
	class_addMethod(c, sel_registerName("_setAcceleratorService:"), imp_implementationWithBlock(^(id me, id service) {
		objc_setAssociatedObject(me, &kAccelServiceKey, service, OBJC_ASSOCIATION_RETAIN);
		sm2_log("device: _setAcceleratorService: %s", service ? class_getName(object_getClass(service)) : "nil");
	}), "v24@0:8@16");
	static const char *const getters[] = { "acceleratorService", "_acceleratorService" };
	for (const char *getter : getters)
		class_addMethod(c, sel_registerName(getter), imp_implementationWithBlock(^id(id me) {
			return objc_getAssociatedObject(me, &kAccelServiceKey);
		}), "@16@0:8");
	class_addMethod(c, @selector(forwardingTargetForSelector:), imp_implementationWithBlock(^id(id me, SEL s) {
		(void)me;
		const char *n = sel_getName(s);
		size_t len = strlen(n);
		if (!strncmp(n, "_set", 4) && len > 5 && n[len - 1] == ':' && strchr(n, ':') == n + len - 1) {
			missing(class_getName(object_getClass(me)), n);	/* logged once, answered as a no-op */
			return stub;
		}
		if (n[0] == '_' || !strncmp(n, "retain", 6) || !strncmp(n, "release", 7) || !strcmp(n, "dealloc"))
			return nil;
		return stub;
	}), "@24@0:8:16");
	static io_service_t accel;
	const char *what = "IOAccelerator";
	if (port && IOObjectRetain(port) == KERN_SUCCESS)
		accel = port;	/* Metal releases its reference after -initWithAcceleratorPort: */
	else {
		accel = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("StarbiteMali"));
		what = "StarbiteMali";
	}
	if (accel) {
		SEL sel = sel_registerName("acceleratorPort");
		Method m = class_getInstanceMethod(c, sel);
		class_replaceMethod(c, sel, imp_implementationWithBlock(^io_service_t(id me) { (void)me; return accel; }),
				    m ? method_getTypeEncoding(m) : "I16@0:8");
	}
	sm2_log("device %s: acceleratorPort %#x (%s service), peerGroupID/peerIndex/peerCount, zero catch-all",
		class_getName(c), accel, accel ? what : "none");
}

static id gDevice;

/* The process's one device (created on the first call; nil when refused, sbmtl_status says why). */
extern "C" id sbmtl_device(mach_port_t port)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		gDevice = open_device(port);
		if (gDevice)
			fill_device(gDevice, port);
	});
	return gDevice;
}

extern "C" size_t sbmtl_status(char *out, size_t size)
{
	if (!gDevice)
		return (size_t)snprintf(out, size, "%s", sbmtl_error[0] ? sbmtl_error : "no device yet");
	return native_bridge_metal_status(out, size);
}

@interface _MTLDevice : NSObject
@end

/* Metal's own discovery (MetalPluginClassName): the object Metal made is dropped for the frontend's device. */
@interface StarbiteMTLDevice : _MTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port;
@end

@implementation StarbiteMTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port
{
	if (!(self = [super init]))
		return nil;
	return sbmtl_device(port);
}
@end
