/*
 * StarbiteMetal.bundle for macOS 26.5 on the Galaxy S9: the Metal device for the Mali-G72 MP18.
 *
 * The same stack as iOS 26's (tools/metal2/HotMetal2.mm, from IOS-26-Hot, MIT): the frontend
 * (tools/metal2/NativeBridgeMetalFrontend.mm: a dynamic subclass of the process's own _MTLDevice and
 * NB* objects) over the metal2vulkan bridge (Rust, AIR -> SPIR-V and the Metal object model on Vulkan),
 * over PanVK (Contents/Frameworks/libvulkan_panfrost.dylib, Mesa 26.2.6), which reaches the GPU through
 * src/starbite-drm -> the kernel's StarbiteMali user client. Built for the macOS platform (dyld refuses
 * iOS-platform images in a macOS process), arm64: the processes that load it are started with
 * ptrauth_disabled=1 by the kernel (sweetinject, xnu 0324), which lets dyld take arm64 images into arm64e.
 *
 * Two ways in, one device per process:
 *  - sbmac.dylib (the injection, tools/macos/metal/sbmac.m) dlopens this bundle and calls sbmtl_device(),
 *    then MTLAddDevice(): no kernel accelerator service needed.
 *  - Metal's own discovery, once the kernel publishes an IOAccelerator whose MetalPluginName is
 *    "StarbiteMetal" and MetalPluginClassName "StarbiteMTLDevice" (xnu/patches/pending-macos26/
 *    0352-...): Metal finds the bundle with gpu_bundle_find_trusted() and calls
 *    -[StarbiteMTLDevice initWithAcceleratorPort:].
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
	sm2_log("device %s in %.0f ms (driver %s, cache %s, registry id %#llx)%s%s",
		device ? [[device name] UTF8String] : "refused", (CFAbsoluteTimeGetCurrent() - t0) * 1000, driver, cache,
		(unsigned long long)registryID, device ? "" : ": ", device ? "" : nmb_last_error());
	return device;
}

static id gDevice;

/* The process's one device (created on the first call; nil when refused, sbmtl_status says why). */
extern "C" id sbmtl_device(mach_port_t port)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		gDevice = open_device(port);
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
