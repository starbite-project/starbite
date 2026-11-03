/*
 * sbmtltest: the first macOS 26.5 Metal test on the Galaxy S9 (docs/MACOS-METAL.md section 6), run by
 * launchd from the card root (Library/LaunchDaemons/com.starbite.sbmtltest.plist) before WindowServer.
 * One step after another, each a "SBMTLTEST: <step> PASS|FAIL ..." line on stdout and /dev/console:
 *
 *  1 kernel     the services macOS userland needs from our kernel: StarbiteMali (the GPU),
 *               IOSurfaceRoot (macOS IOSurface.framework's name for it), IOMobileFramebuffer (the panel)
 *  2 iosurface  IOSurfaceCreate + lock/write/read/unlock (macOS IOSurface.framework on SweetSurface)
 *  3 device     the Mali's MTLDevice from StarbiteMetal.bundle (frontend + metal2vulkan + PanVK)
 *  4 wscheck    WindowServer's own test: after MTLAddDevice, MTLCopyAllDevicesWithObserver() is not empty
 *               (SkyLight's WSSystemCanCompositeWithMetal aborts otherwise)
 *  5 clear      a render pass clears a texture on the GPU; a blit reads it back: the color must match
 *  6 shaders    WindowServer's own shader library (SkyLightShaders.air64.metallib): every vertex/fragment
 *               pair that shares a name stem becomes a render pipeline (AIR -> SPIR-V -> Bifrost)
 *  7 present    a panel-sized IOSurface: cleared and striped by the GPU, checked by the CPU, and swapped
 *               onto the panel with IOMobileFramebuffer for --seconds (default 20) with a moving bar
 *
 *   sbmtltest [--seconds N] [--no-present] [--bundle PATH]
 * Exit status: 0 when every step passed, else the number of the first failed step.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <CoreGraphics/CGGeometry.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

#define BUNDLE "/System/Library/Extensions/StarbiteMetal.bundle/Contents/MacOS/StarbiteMetal"
#define IOMFB "/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/Versions/A/IOMobileFramebuffer"
#define SKYLIGHT_SHADERS "/System/Library/PrivateFrameworks/SkyLight.framework/Resources/SkyLightShaders.air64.metallib"

static int console = -1;
static int firstFailure;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char msg[700], line[760];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	int n = snprintf(line, sizeof(line), "SBMTLTEST: %s\n", msg);
	if (n > (int)sizeof(line) - 1)
		n = (int)sizeof(line) - 1;
	fwrite(line, 1, (size_t)n, stdout);
	fflush(stdout);
	if (console >= 0 && !isatty(1))
		write(console, line, (size_t)n);
}

/* A step that never returns is reported: the step number and how long, every 15 s (M1 boot 1: IOSurfaceCreate
 * blocked in IOSurface.framework's first connect, which asks LaunchServices; coreservicesd was missing). */
static volatile int current_step = 1;
static void *watchdog(void *arg)
{
	(void)arg;
	int last = 0, seconds = 0;
	for (;;) {
		sleep(15);
		seconds = current_step == last ? seconds + 15 : 15;
		last = current_step;
		say("still in step %d after %d s (step 2 hangs: IOSurface's connect waits on cfprefsd or LaunchServices/coreservicesd)",
		    last, seconds);
	}
	return NULL;
}

static bool step(int number, const char *name, bool ok, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static bool step(int number, const char *name, bool ok, const char *fmt, ...)
{
	char detail[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	say("%d %s %s %s", number, name, ok ? "PASS" : "FAIL", detail);
	current_step = number + 1;
	if (!ok && !firstFailure)
		firstFailure = number;
	return ok;
}

static uint64_t service_id(const char *how, const char *name)
{
	CFMutableDictionaryRef match = !strcmp(how, "name") ? IOServiceNameMatching(name) : IOServiceMatching(name);
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, match);
	uint64_t id = 0;
	if (s) {
		IORegistryEntryGetRegistryEntryID(s, &id);
		IOObjectRelease(s);
	}
	return id;
}

static unsigned service_count(const char *cls)
{
	io_iterator_t it = 0;
	unsigned n = 0;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(cls), &it) == KERN_SUCCESS) {
		for (io_object_t o; (o = IOIteratorNext(it)); n++)
			IOObjectRelease(o);
		IOObjectRelease(it);
	}
	return n;
}

static IOSurfaceRef new_surface(size_t w, size_t h)
{
	size_t bpr = (w * 4 + 63) & ~(size_t)63;
	NSDictionary *props = @{
		(id)kIOSurfaceWidth : @(w),
		(id)kIOSurfaceHeight : @(h),
		(id)kIOSurfaceBytesPerElement : @4,
		(id)kIOSurfaceBytesPerRow : @(bpr),
		(id)kIOSurfaceAllocSize : @(bpr * h),
		(id)kIOSurfacePixelFormat : @(0x42475241),	/* 'BGRA' */
	};
	return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

static bool wait_ok(id<MTLCommandBuffer> cb, double *ms)
{
	double t0 = CFAbsoluteTimeGetCurrent();
	[cb commit];
	[cb waitUntilCompleted];
	if (ms)
		*ms = (CFAbsoluteTimeGetCurrent() - t0) * 1000;
	return cb.status == MTLCommandBufferStatusCompleted;
}

/* IOMobileFramebuffer.framework (private): the calls CoreAnimation's IOMFB display makes */
typedef void *IOMFBRef;
static IOReturn (*IOMobileFramebufferGetMainDisplay_)(IOMFBRef *);
static IOReturn (*IOMobileFramebufferOpen_)(io_service_t, task_port_t, uint32_t, IOMFBRef *);
static IOReturn (*IOMobileFramebufferGetDisplaySize_)(IOMFBRef, CGSize *);
static IOReturn (*IOMobileFramebufferSwapBegin_)(IOMFBRef, int *);
static IOReturn (*IOMobileFramebufferSwapSetLayer_)(IOMFBRef, int, IOSurfaceRef, CGRect, CGRect, int);
static IOReturn (*IOMobileFramebufferSwapEnd_)(IOMFBRef);
static IOReturn (*IOMobileFramebufferSwapWait_)(IOMFBRef, int, int);

static bool load_iomfb(void)
{
	void *h = dlopen(IOMFB, RTLD_NOW);
	if (!h)
		return false;
	IOMobileFramebufferGetMainDisplay_ = dlsym(h, "IOMobileFramebufferGetMainDisplay");
	IOMobileFramebufferOpen_ = dlsym(h, "IOMobileFramebufferOpen");
	IOMobileFramebufferGetDisplaySize_ = dlsym(h, "IOMobileFramebufferGetDisplaySize");
	IOMobileFramebufferSwapBegin_ = dlsym(h, "IOMobileFramebufferSwapBegin");
	IOMobileFramebufferSwapSetLayer_ = dlsym(h, "IOMobileFramebufferSwapSetLayer");
	IOMobileFramebufferSwapEnd_ = dlsym(h, "IOMobileFramebufferSwapEnd");
	IOMobileFramebufferSwapWait_ = dlsym(h, "IOMobileFramebufferSwapWait");
	return IOMobileFramebufferSwapBegin_ && IOMobileFramebufferSwapSetLayer_ && IOMobileFramebufferSwapEnd_ &&
	       IOMobileFramebufferGetDisplaySize_ && (IOMobileFramebufferGetMainDisplay_ || IOMobileFramebufferOpen_);
}

/* 8 bars, the Starbite colors and the primaries, as BGRA words */
static const uint32_t bars[8] = { 0xff1b1440, 0xff6a3cff, 0xffffb000, 0xffff4f8b,
				  0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };

int main(int argc, char **argv)
{
	@autoreleasepool {
		int seconds = 20;
		bool present = true;
		const char *bundle = BUNDLE;
		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
				seconds = atoi(argv[++i]);
			else if (!strcmp(argv[i], "--no-present"))
				present = false;
			else if (!strcmp(argv[i], "--bundle") && i + 1 < argc)
				bundle = argv[++i];
		}
		console = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
		pthread_t wd;
		pthread_create(&wd, NULL, watchdog, NULL);
		pthread_detach(wd);
		NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
		say("start: macOS %ld.%ld.%ld, pid %d uid %d", (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion,
		    getpid(), getuid());

		/* 1 kernel */
		uint64_t mali = service_id("class", "StarbiteMali");
		uint64_t surfRoot = service_id("name", "IOSurfaceRoot");
		uint64_t coreSurfRoot = service_id("name", "IOCoreSurfaceRoot");
		unsigned fbs = service_count("IOMobileFramebuffer");
		unsigned accels = service_count("IOAccelerator");
		step(1, "kernel", mali && surfRoot && fbs,
		     "StarbiteMali %#llx, IOSurfaceRoot %#llx (IOCoreSurfaceRoot %#llx), IOMobileFramebuffer x%u, IOAccelerator x%u%s",
		     (unsigned long long)mali, (unsigned long long)surfRoot, (unsigned long long)coreSurfRoot, fbs, accels,
		     !surfRoot && coreSurfRoot ? " -- kernel lacks pending-macos26/0351 (macOS looks up \"IOSurfaceRoot\")" : "");

		/* 2 iosurface */
		IOSurfaceRef probe = new_surface(64, 64);
		bool surfOK = false;
		if (probe) {
			IOSurfaceLock(probe, 0, NULL);
			uint32_t *p = IOSurfaceGetBaseAddress(probe);
			p[0] = 0x12345678;
			p[64 * 63] = 0x9abcdef0;
			IOSurfaceUnlock(probe, 0, NULL);
			IOSurfaceLock(probe, kIOSurfaceLockReadOnly, NULL);
			surfOK = p[0] == 0x12345678 && p[64 * 63] == 0x9abcdef0;
			IOSurfaceUnlock(probe, kIOSurfaceLockReadOnly, NULL);
		}
		step(2, "iosurface", surfOK, "IOSurfaceCreate 64x64 BGRA: %s, id %u, bytes per row %zu", probe ? "made" : "NULL",
		     probe ? IOSurfaceGetID(probe) : 0, probe ? IOSurfaceGetBytesPerRow(probe) : 0);

		/* 3 device */
		double t0 = CFAbsoluteTimeGetCurrent();
		void *h = dlopen(bundle, RTLD_NOW | RTLD_LOCAL);
		id (*sbmtl_device)(mach_port_t) = h ? dlsym(h, "sbmtl_device") : NULL;
		size_t (*sbmtl_status)(char *, size_t) = h ? dlsym(h, "sbmtl_status") : NULL;
		id<MTLDevice> dev = sbmtl_device ? sbmtl_device(MACH_PORT_NULL) : nil;
		char why[400] = "";
		if (!h)
			snprintf(why, sizeof(why), "dlopen: %s", dlerror());
		else if (!dev && sbmtl_status)
			sbmtl_status(why, sizeof(why));
		if (!step(3, "device", dev != nil, "%s in %.0f ms, registry id %#llx%s%s", dev ? dev.name.UTF8String : "none",
			  (CFAbsoluteTimeGetCurrent() - t0) * 1000, (unsigned long long)dev.registryID, why[0] ? ": " : "", why)) {
			say("RESULT FAIL at step %d", firstFailure);
			return firstFailure;
		}

		/* 4 wscheck: what sbmac.dylib does inside WindowServer, then WindowServer's own call */
		NSArray *before = MTLCopyAllDevices();
		BOOL listed = [before containsObject:dev];
		void (*add)(id) = dlsym(RTLD_DEFAULT, "MTLAddDevice");
		if (!listed && add)
			add(dev);
		id<NSObject> observer = nil;
		NSArray *all = MTLCopyAllDevicesWithObserver(&observer, ^(id<MTLDevice> d, MTLDeviceNotificationName n) {
			(void)d;
			(void)n;
		});
		if (observer)
			MTLRemoveDeviceObserver(observer);
		id<MTLDevice> sysDefault = MTLCreateSystemDefaultDevice();
		step(4, "wscheck", all.count >= 1, "Metal listed %lu before, %s; MTLCopyAllDevicesWithObserver -> %lu; MTLCreateSystemDefaultDevice -> %s",
		     (unsigned long)before.count, listed ? "ours already there" : add ? "MTLAddDevice(ours)" : "no MTLAddDevice",
		     (unsigned long)all.count, sysDefault ? sysDefault.name.UTF8String : "nil");

		/* 5 clear */
		id<MTLCommandQueue> queue = [dev newCommandQueue];
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											     width:256
											    height:256
											 mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModePrivate;
		id<MTLTexture> target = [dev newTextureWithDescriptor:td];
		id<MTLBuffer> back = [dev newBufferWithLength:256 * 256 * 4 options:MTLResourceStorageModeShared];
		bool clearOK = false;
		double clearMs = 0;
		uint32_t got = 0;
		if (queue && target && back) {
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = target;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
			id<MTLCommandBuffer> cb = [queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
			[enc endEncoding];
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
			[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				   sourceSize:MTLSizeMake(256, 256, 1) toBuffer:back destinationOffset:0
			  destinationBytesPerRow:256 * 4 destinationBytesPerImage:256 * 256 * 4];
			[blit endEncoding];
			bool done = wait_ok(cb, &clearMs);
			got = ((uint32_t *)back.contents)[128 * 256 + 128];
			/* BGRA8 of (0.25, 0.5, 0.75, 1): B 0xbf G 0x80 R 0x40 A 0xff, +-1 for rounding */
			uint8_t b = got & 0xff, g = (got >> 8) & 0xff, r = (got >> 16) & 0xff, a = got >> 24;
			clearOK = done && abs(b - 0xbf) <= 1 && abs(g - 0x80) <= 1 && abs(r - 0x40) <= 1 && a == 0xff;
		}
		step(5, "clear", clearOK, "256x256 BGRA8 clear + readback in %.1f ms: center %#010x (want ~0xff4080bf)", clearMs, got);

		/* 6 shaders */
		NSError *error = nil;
		double tl = CFAbsoluteTimeGetCurrent();
		id<MTLLibrary> sky = [dev newLibraryWithURL:[NSURL fileURLWithPath:@SKYLIGHT_SHADERS] error:&error];
		double libMs = (CFAbsoluteTimeGetCurrent() - tl) * 1000;
		unsigned made = 0, refused = 0, pairs = 0;
		double pipeMs = 0;
		NSString *firstRefusal = nil;
		if (sky) {
			NSArray<NSString *> *names = sky.functionNames;
			for (NSString *name in names) {
				id<MTLFunction> vf = [sky newFunctionWithName:name];
				if (vf.functionType != MTLFunctionTypeVertex)
					continue;
				/* pair a vertex function with the fragment function of the same stem (..._vertex / ..._fragment, ..._vs / ..._fs) */
				NSString *frag = nil;
				for (NSArray *swap in @[ @[ @"vertex", @"fragment" ], @[ @"Vertex", @"Fragment" ], @[ @"_vs", @"_fs" ],
							   @[ @"_vert", @"_frag" ], @[ @"VS", @"FS" ] ]) {
					NSString *f = [name stringByReplacingOccurrencesOfString:swap[0] withString:swap[1]];
					if (![f isEqualToString:name] && [names containsObject:f]) {
						frag = f;
						break;
					}
				}
				if (!frag)
					continue;
				pairs++;
				MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
				d.vertexFunction = vf;
				d.fragmentFunction = [sky newFunctionWithName:frag];
				d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
				double tp = CFAbsoluteTimeGetCurrent();
				NSError *pe = nil;
				id<MTLRenderPipelineState> st = [dev newRenderPipelineStateWithDescriptor:d error:&pe];
				pipeMs += (CFAbsoluteTimeGetCurrent() - tp) * 1000;
				if (st)
					made++;
				else {
					refused++;
					if (!firstRefusal)
						firstRefusal = [NSString stringWithFormat:@"%@: %@", name, pe.localizedDescription];
				}
			}
		}
		step(6, "shaders", sky && made > 0 && refused == 0,
		     "SkyLightShaders: %s (%.0f ms, %lu functions); %u pairs: %u pipelines made, %u refused, %.0f ms%s%s",
		     sky ? "loaded" : error.localizedDescription.UTF8String, libMs, (unsigned long)sky.functionNames.count, pairs, made,
		     refused, pipeMs, firstRefusal ? "; first refusal " : "", firstRefusal ? firstRefusal.UTF8String : "");

		/* 7 present */
		if (!present) {
			say("7 present SKIPPED (--no-present)");
		} else if (!load_iomfb()) {
			step(7, "present", false, "IOMobileFramebuffer.framework: %s", dlerror());
		} else {
			IOMFBRef fb = NULL;
			IOReturn kr = IOMobileFramebufferGetMainDisplay_ ? IOMobileFramebufferGetMainDisplay_(&fb) : kIOReturnUnsupported;
			if (!fb && IOMobileFramebufferOpen_) {
				io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOMobileFramebuffer"));
				kr = s ? IOMobileFramebufferOpen_(s, mach_task_self(), 0, &fb) : kIOReturnNotFound;
			}
			CGSize size = { 0, 0 };
			if (fb)
				IOMobileFramebufferGetDisplaySize_(fb, &size);
			size_t w = (size_t)size.width, ht = (size_t)size.height;
			IOSurfaceRef frame = fb && w && ht ? new_surface(w, ht) : NULL;
			MTLTextureDescriptor *fd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
												     width:w ? w : 1
												    height:ht ? ht : 1
												 mipmapped:NO];
			fd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			fd.storageMode = MTLStorageModeShared;
			id<MTLTexture> ft = frame ? [dev newTextureWithDescriptor:fd iosurface:frame plane:0] : nil;
			size_t barH = ht / 8;
			id<MTLBuffer> stripe = ft ? [dev newBufferWithLength:w * 4 * barH options:MTLResourceStorageModeShared] : nil;
			bool drawn = false, checked = false;
			int swaps = 0;
			IOReturn swapErr = kIOReturnSuccess;
			double frameMs = 0;
			for (int tick = 0; ft && stripe && tick <= seconds; tick++) {
				/* GPU: clear to the Starbite night blue, then 7 bars by blit from a CPU-filled buffer; one bar
				 * (tick % 8) is left as the clear color, so the panel shows the frames are new */
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = ft;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0.04, 0.03, 0.12, 1.0);
				id<MTLCommandBuffer> cb = [queue commandBuffer];
				[[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
				drawn = wait_ok(cb, &frameMs);
				/* the bars: 32-bit colors the CPU writes into a buffer, copied into the surface by GPU blits */
				for (int b = 0; b < 8 && drawn; b++) {
					if (b == tick % 8)
						continue;
					uint32_t *p = stripe.contents;
					for (size_t i = 0; i < w * barH; i++)
						p[i] = bars[b];
					id<MTLCommandBuffer> c2 = [queue commandBuffer];
					id<MTLBlitCommandEncoder> e2 = [c2 blitCommandEncoder];
					[e2 copyFromBuffer:stripe sourceOffset:0 sourceBytesPerRow:w * 4 sourceBytesPerImage:w * 4 * barH
						sourceSize:MTLSizeMake(w, barH, 1) toTexture:ft destinationSlice:0 destinationLevel:0
					 destinationOrigin:MTLOriginMake(0, (NSUInteger)b * barH, 0)];
					[e2 endEncoding];
					drawn = wait_ok(c2, NULL);
				}
				if (tick == 0 && drawn) {
					/* CPU check through the IOSurface: the GPU wrote the surface's own pages */
					IOSurfaceLock(frame, kIOSurfaceLockReadOnly, NULL);
					const uint8_t *base = IOSurfaceGetBaseAddress(frame);
					size_t bpr = IOSurfaceGetBytesPerRow(frame);
					uint32_t clearPx = *(const uint32_t *)(base + (barH / 2) * bpr + (w / 2) * 4);	/* bar 0 left clear at tick 0 */
					uint32_t bar1 = *(const uint32_t *)(base + (barH + barH / 2) * bpr + (w / 2) * 4);
					IOSurfaceUnlock(frame, kIOSurfaceLockReadOnly, NULL);
					checked = bar1 == bars[1] && (clearPx >> 24) == 0xff && (clearPx & 0xff) >= 0x1c && (clearPx & 0xff) <= 0x20;
					say("7 present: surface %u %zux%zu, CPU reads bar 1 %#010x (want %#010x), clear %#010x", IOSurfaceGetID(frame), w,
					    ht, bar1, bars[1], clearPx);
				}
				int token = 0;
				swapErr = IOMobileFramebufferSwapBegin_(fb, &token);
				if (swapErr == kIOReturnSuccess) {
					CGRect r = CGRectMake(0, 0, (CGFloat)w, (CGFloat)ht);
					IOMobileFramebufferSwapSetLayer_(fb, 0, frame, r, r, 0);
					swapErr = IOMobileFramebufferSwapEnd_(fb);
					if (IOMobileFramebufferSwapWait_)
						IOMobileFramebufferSwapWait_(fb, token, 1000);
				}
				if (swapErr == kIOReturnSuccess)
					swaps++;
				if (tick < seconds)
					sleep(1);
			}
			step(7, "present", fb && ft && drawn && checked && swaps > 0,
			     "IOMFB %p (%#x) %zux%zu, Metal texture over the IOSurface %s, GPU frame %.1f ms, %d swaps (last %#x) over %d s", fb,
			     kr, w, ht, ft ? "made" : "refused", frameMs, swaps, swapErr, seconds);
		}

		char status[400] = "";
		if (sbmtl_status)
			sbmtl_status(status, sizeof(status));
		say("bridge status: %s", status);
		if (firstFailure)
			say("RESULT FAIL at step %d", firstFailure);
		else
			say("RESULT PASS: the Mali-G72 draws for macOS userland through Metal");
		return firstFailure;
	}
}
