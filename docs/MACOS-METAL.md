# Metal for macOS 26.5 on the Galaxy S9: what WindowServer needs and what is built

Status 2026-11-03. Host work only: nothing was written to the phone, and nothing has run on it yet.
The goal is macOS 26.5's WindowServer drawing the loginwindow on the panel. Snapintosh's lesson is the
first gate: WindowServer will not start without a Metal device.

The 26.5 root is not on the external drive yet: `/Volumes/Untitled/starbite-macos26/` holds only
`BuildManifest.plist`, `aea-head` and `keys.txt`. So the userland evidence below comes from **this Mac's
macOS 27.2 (26B5112a) shared cache**, read-only, as a stand-in. The kernel-side evidence comes from the
**macOS 26.5 KDK** (`build/kdk/KDK_26.5_25F71.kdk`). Section 8 lists what to re-check on the 26.5 cache
with the same tools.

Contents: 1 summary · 2 the iOS 26 chain · 3 what macOS needs · 4 the table · 5 what is built ·
6 the first phone test · 7 missing pieces · 8 how to re-check on 26.5 · 9 sources.

---

## 1. Summary

- **The WindowServer gate.** SkyLight's `WSSystemCanCompositeWithMetal()` calls
  `MTLCopyAllDevicesWithObserver()`. If the array is empty, it waits 30 s for a device-added
  notification. Then it prints "No suitable Metal devices present after 30 seconds." and calls
  `abort()`. Nothing more is asked of the device at that point: the array only has to be non-empty.
- **The display path is the iOS one.** WindowServer composites with QuartzCore's render server
  (`CA::WindowServer`, through SkyLight's `CAWSBackend` / `WS::Displays::SLCADisplay`). That server
  drives the panel through IOMobileFramebuffer. macOS 26.5's IOMobileFramebuffer user client has the
  same selector numbers our kernel's `AppleCLCD` implements for iOS 26. Its swap record is 0x588 bytes,
  the size iOS 26 already sends. CoreDisplay's IOAccelerator/IOFramebuffer code is the Intel/eGPU
  path and plays no part here.
- **One kernel gap blocks macOS.** macOS's IOSurface.framework looks its kernel root up by the name
  `IOSurfaceRoot`. Our kernel calls it `IOCoreSurfaceRoot`, iOS's name. The user-client ABI matches
  (26.5 KDK: 0xc68-byte lock result, 0x28-byte limits, the same selector numbers). The fix is
  `xnu/patches/pending-macos26/0360-sweet-iosurface-root-macos-name.patch`.
- **Built** (`tools/macos/metal/build.sh`):
  - The same Metal stack as iOS 26, rebuilt for the macOS platform: frontend + metal2vulkan bridge +
    PanVK → starbite-drm → StarbiteMali. It is packaged as `StarbiteMetal.bundle`.
  - `sbmac.dylib`, the injection (`sweetinject=/usr/lib/sbmac.dylib,WindowServer`). It publishes the
    device with Metal's own `MTLAddDevice()` before WindowServer's check, so no kernel accelerator
    service is needed.
  - `sbmtltest`, a 7-step command-line test that runs from the card root before WindowServer
    (section 6).
- **Checked on the Mac:**
  - The bundle loads in a macOS process. PanVK (the macOS build) opens a Vulkan instance and refuses
    cleanly with no Mali ("device: NoDevice").
  - The injection's interposers catch `MTLCopyAllDevicesWithObserver` in an injected process.
  - macOS 27.2's `MTLAddDevice` accepts a dynamic `_MTLDevice` subclass that carries the frontend's
    `initLimits`/`featureProfile` overrides. After it, `MTLCopyAllDevicesWithObserver` returns that
    device (2 devices on this Mac: the M4 and the fake one).

---

## 2. The iOS 26 Metal chain, exactly

The daily command line is `build/ios26-daily-cmdline.txt`. Its GPU arguments are `sweetfb_gpu=1`,
`sbmtl=1`, `sbgl=0` and `sweetinject=/usr/lib/sbca.dylib,backboardd,SpringBoard,iconservicesagent`.

### 2.1 Who provides the MTLDevice

**The kernel publishes the GPU as Metal sees it.** Patch 0336 adds `StarbiteAccelerator`
(`iokit/Kernel/arm/StarbiteAccelerator.cpp`). It is a subclass of an `IOAcceleratorES` class (an
`IOService`), matched on IOResources, and published only with `sbmtl=1`. It sets these properties:

| Property | Value |
|---|---|
| `MetalPluginName` | `StarbiteMetal` |
| `MetalPluginClassName` | `HotMTL2Device` |
| `IOGLBundleName` | `StarbiteMetal` |
| `model` | `Mali-G72` |

**Metal.framework loads the plugin.** iOS 26 Metal iterates `IOServiceMatching("IOAcceleratorES")`. It
opens `/System/Library/Extensions/StarbiteMetal.bundle` and calls
`-[HotMTL2Device initWithAcceleratorPort:]`.

**The plugin returns the frontend's device.** The plugin is `tools/metal2/HotMetal2.mm` (from
IOS-26-Hot, MIT). It gates the device on the client list `/var/local/mtl2-clients`, sets up
`/var/local/m2v-cache/<proc>`, and turns off CA's deep and compressed framebuffers. It then returns
the object from `native_bridge_metal_open()` instead of `self`.

**The frontend is the MTLDevice.** `tools/metal2/NativeBridgeMetalFrontend.mm` (UniXNU/IOS-26-Hot, MIT)
builds the object:
- The device is a class made at run time, `NativeBridgeMetalDevice`, a subclass of the process's own
  `_MTLDevice`. It adopts `MTLDevice` and `MTLDeviceSPI`.
- It overrides `initLimits`, `featureProfile` and the other methods `MTLAddDevice` runs.
- Every other Metal object is an `NB*` class: queue, command buffer, encoders, textures, buffers,
  pipelines and so on.

### 2.2 How it reaches the Mali

```
Metal API calls (CoreAnimation's CA::OGL::MetalContext, CoreImage, IconServices ...)
  -> NB* objects (frontend, Objective-C++)
  -> nmb_* C ABI: tools/metal2/bridge/NativeMetalBridge.rs, NativeMetalObjects.rs (Rust)
     over metal2vulkan 720ef1ce + tools/metal2/bridge/patches (AIR -> SPIR-V, the Metal object model on ash)
  -> PanVK, Contents/libvulkan_panfrost.dylib (Mesa 26.2.6 + tools/mesa-ios/mesa26-panvk-ios.patch,
     PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1, MESA_VK_VERSION_OVERRIDE=1.2)
  -> starbite-drm (tools/mesa-ios/starbite-drm/starbite_drm.c, the libdrm stand-in): every Panfrost ioctl
     becomes IOConnectCallMethod on the "StarbiteMali" service. Selector = ioctl nr - 0x40 (SUBMIT,
     WAIT_BO, CREATE_BO, MMAP_BO, GET_PARAM, GET_BO_OFFSET, MADVISE), plus 0x100 GEM_CLOSE,
     0x103 SURFACE (an IOSurface by id -> a BO) and 0x104 IMPORT_HOST
  -> kernel StarbiteMali + StarbiteMaliUserClient (iokit/Kernel/arm/StarbiteMali.cpp, 0305 + 0326-0341):
     Mali-G72 job slots, one GPU address space, jobs synchronous inside SUBMIT
```

**No Apple GPU interface is involved.** There is no IOGPU, no AGX and no IOAccelerator user client.
`IOAcceleratorES` only carries the two plugin names. The GPU work goes through our own user client.

### 2.3 How frames reach the panel

1. **The renderer.** CoreAnimation's render server is in backboardd on iOS:
   `CA::WindowServer::AccelServer::renderer()`, with `CAMetalContextCreate` → `CA::OGL::MetalContext`.
   It renders into IOSurfaces. The kernel's `SweetSurface` (`IOCoreSurfaceRoot` and
   `IOSurfaceRootUserClient`) allocates them. The frontend turns them into Metal textures with
   SURFACE (0x103), so the GPU writes the surface's own pages.
2. **The swap.** `CA::WindowServer::IOMFBDisplay` calls IOMobileFramebuffer:
   SwapBegin (selector 4), then SwapEnd (selector 5). SwapEnd carries a 0x588-byte record: the layer
   surface IDs and their source and destination rects.
3. **The kernel's display.** The kernel's `AppleCLCD : IOMobileFramebuffer`
   (`iokit/Kernel/arm/SweetFramebuffer.cpp`, matched on the ADT's `disp0`) keeps the latest record. A
   60 Hz timer stands in for vsync and presents the record:
   - With `sweetfb_gpu=1` and a full-screen 828x1792 frame, the Mali scales it to the 1440x2960 panel:
     a precompiled job (0303/0332, `starlte_gpu_present_*`).
   - Otherwise the CPU does a scaled copy.
4. **The panel.** The panel scans out the boot (continuous-splash) framebuffer that `AppleCLCD`
   writes into.

### 2.4 How it is injected

- **0316 (`kern_exec.c`).** The `sweetinject=LIB,NAME,...` boot-arg (also the `kern.exec_inject`
  sysctl) adds `DYLD_INSERT_LIBRARIES=LIB` to the environment of every exec whose file name is one of
  the NAMEs. launchd drops `DYLD_` variables from jobs, so the kernel adds it.
- **0324.** The same processes get the apple string `ptrauth_disabled=1`. dyld then grades with
  `arm64e_keysOff`, so the **arm64** libraries (sbca, the bundle, PanVK) load into arm64e processes.
  "Metal needs sweetinject: the plugin is arm64" (NOTES 2026-11-02).
- **The AMFI stand-in (`bsd/kern/sweet_amfi.c`).** Op 90 is `amfi_check_dyld_policy_self`. With the
  `sweetinject` boot-arg present it answers "path variables, print variables, failed insertion,
  interposing allowed", so dyld honours the variable.
- **sbca.dylib (`tools/ca-gpu/sbca.c`).** With `sbgl=0` it leaves CA's own Metal renderer in place.
  It still patches two things in iOS 26's QuartzCore:
  - `IOMFBDisplay::framebuffer_pixel_format` → 'BGRA', because metal2vulkan has no 'b3a8';
  - the `push_surface` assert.

---

## 3. What macOS 26.5 needs (evidence: macOS 27.2 userland, 26.5 KDK)

### 3.1 WindowServer's Metal device check

**The check.** SkyLight `___WSSystemCanCompositeWithMetal_block_invoke` (0x1828bed28 in the 27.2
cache) runs this sequence:
1. `dispatch_semaphore_create`
2. `MTLCopyAllDevicesWithObserver(&obs, handler)`
3. If the array has a count: signal and continue.
4. Else: log "Metal device initially absent.", then `dispatch_semaphore_wait` 30 s
   (0x6fc23ac00 ns). The handler (`_block_invoke_2`) signals on a device-added notification.
5. Still none: `asprintf("No suitable Metal devices present after %llu seconds.", 30)`,
   `SLSLogBreak`, `abort()`.

**What the check needs from the device: only that one is listed.** The compositor uses it afterwards:
- SkyLight's shaders, `SkyLight.framework/Resources/SkyLightShaders.air64.metallib` (AIR, which
  metal2vulkan translates);
- IOSurface-backed textures;
- CA's MetalContext.

### 3.2 How macOS Metal finds devices

**Metal's own discovery:**
1. `-[MTLIOAccelServiceGlobalContext init]` matches `IOServiceMatching("IOAccelerator")`. That is not
   `IOAcceleratorES`, so iOS's accelerator is invisible to macOS Metal.
2. `getMetalPluginClassForService()` (0x1941007e0) reads `MetalPluginName`.
3. It calls libsystem_sandbox's `gpu_bundle_find_trusted(name, path, 1024)`. That function tries
   `/Library/GPUBundles`, `/System/Library/Extensions` and `/AppleInternal/Library/Extensions` +
   `/<name>.bundle`.
4. Each path must pass `gpu_bundle_is_path_trusted` →
   `rootless_check_trusted_class(path, "KernelExtensionManagement")` →
   `csr_check(CSR_ALLOW_UNRESTRICTED_FS)`:
   - 0 (SIP off) means trusted.
   - Otherwise the path must be on an authenticated root volume, or the Sandbox's rootless check must
     pass. Neither exists for us: the root is HFSX, and there is no Sandbox kext.
5. Metal then reads `MetalPluginClassName` and loads the class.

On our kernel `csr_check` reads the ADT: `/chosen/asmb` `lp-sip0` (`bsd/kern/kern_csr.c`). With no such
property, everything is restricted and the bundle is not trusted. So **Metal's own discovery needs
two things**:
- an `IOAccelerator` service, which is pending patch 0361 (`sbmtl=2`);
- SIP off in the Mac ADT: `/chosen/asmb` `lp-sip0` with bit 1, as `csrutil disable` sets
  (0x7f is the usual value), and `lp-sip1` for an unauthenticated root.

**The userland way, which needs neither.** `MTLAddDevice(id)` is exported (0x193f3a5ec):
- It asserts that `_deviceArray != NULL`, so `MTLCopyAllDevices` must have run once.
- It asserts that the device conforms to `MTLDevice` and `MTLDeviceSPI`.
- It runs `-[_MTLDevice initLimits]`, which calls `-featureProfile`.

The frontend already answers all of this: its `featureProfile`/`initLimits`/`initFeatureQueries`/
`initGPUFamilySupport`/`initWorkarounds` overrides, and both protocols. **Tested on this Mac:** a
dynamic `_MTLDevice` subclass with exactly those overrides goes into the list. Without them it throws
`-[ProbeDevice featureProfile]: unrecognized selector`.

**CoreAnimation needs nothing more.** CA in WindowServer takes its device with `MTLCopyAllDevices`.
There is no 30 s IOAcceleratorES wait on macOS, unlike iOS 26's `CAMetalContextCreate`. The 27.2
`CAMetalContextCreate` (0x18dcf8914) calls `MTLCopyAllDevices` and picks from the array.

### 3.3 The display

- **SkyLight → CA → IOMFB.**
  - SkyLight's Apple-silicon display backend is CoreAnimation's: `CAWSBackend.mm`,
    `WS::Displays::SLCADisplay`. SkyLight imports only `IOMobileFramebufferOpenByName` and
    `IOMobileFramebufferCopyProperty`, to read the optional `DisplayAttributes` dictionary
    (`SLCADisplay::copy_ioav_display_attributes`).
  - macOS QuartzCore imports **76** IOMobileFramebuffer functions (SwapBegin/SetLayer/End/Wait,
    GetDisplaySize, the notifications, and so on).
  - `CA::WindowServer::IOMFBDisplay` is there, `framebuffer_pixel_format` included.
- **Finding the display.** `IOMobileFramebufferCreateDisplayList` → `iomfb_populate_all_display_infos`
  works as on iOS: it counts `IODeviceTree:/arm-io` children with `device_type` "display-subsystem"
  (or "ext-display-subsystem"), checks `AppleParavirtGPU`, then waits for matching
  `IOMobileFramebuffer` services. `IOMobileFramebufferOpen` reads `canvas-width`/`canvas-height`.
  The Mac ADT keeps the iPhone 11 ADT's `disp0` unchanged (MACOS.md §5), and our `AppleCLCD` matches
  it. **Provided already.**
- **The user-client ABI (26.5 KDK, `IOMobileGraphicsFamily.kext`).** Decoded from
  `IOMobileFramebufferUserClient`'s dispatch table:

  | Selector | 26.5 method | Our `AppleCLCD` |
  |---|---|---|
  | 3 | default_fb_surface | GetLayerDefaultSurface |
  | 4 | swap_start | SwapBegin |
  | 5 | swap_submit (struct in **0x588**) | SwapEnd (0x46c..0x600; iOS 26 logs "rec size 0x588") |
  | 6 | swap_wait | SwapWait |
  | 7 | get_framebuffer_id | GetID |
  | 8 | get_display_size | GetDisplaySize |
  | 12 | request_power_change | RequestPowerChange |
  | 18 | is_main_display | IsMainDisplay |
  | 28 | get_dot_pitch | GetDotPitch |
  | 72 | set_notify | Enable/DisableNotifications |

  Everything else is accepted, logged once and answered with zeros, as for iOS.
- **Modes and timing.** `TimingElements` is read only by `kern_GetSupportedDigitalOutModes` (external
  displays). The built-in display has one mode, the canvas: 828x1792 with `sweetfb_lw/lh` today; the
  MAC3 landscape canvas comes later.
- **EDID.** None. Apple-silicon built-in panels have no EDID. SkyLight's `DisplayAttributes`
  (`ProductAttributes`: ManufacturerID, ProductID, ...) is optional, and a missing one is only logged.
  The real Mac's `IOMobileFramebufferShim` also publishes `DisplayWidth/Height`, `IOMFBNumLayers`,
  `DisplayPipeStrideRequirements` and so on. Section 7 lists which to add if a log asks for them.
- **CoreDisplay plug-ins.** Not on this path. In 27.2, CoreDisplay does not link IOMobileFramebuffer.
  Its `IOAccelerator`/`IOAccel*` use (`CGXMappedDisplayStart`, `get_display_pipe_vtable`) is the
  IOFramebuffer display pipe of Intel and eGPU Macs.

### 3.4 IOSurface

- **The name differs.** macOS IOSurface.framework connects with
  `IOServiceGetMatchingServices(IOServiceNameMatching("IOSurfaceRoot"))`
  (`____ioSurfaceConnectInternal_block_invoke`). The 26.5 KDK's IOSurface.kext class is
  `IOSurfaceRoot`.
- **The ABI matches.** `IOSurfaceRootUserClient::sMethodDescs` in the 26.5 KDK has 0 create
  (out 0xc68), 4 lookup (0xc68), 6 create_fast_path (in 0x20), 7 create_client_mem (2 scalars),
  13 get_limits (out 0x28), 34 lookup_from_port, 35 create_port_from_surface, and the
  value/notify/shared-event selectors. `SweetSurface` already speaks this generation: the iPadOS 18
  `0xc68` result and the `0x28` limits with the ABI version at +0x22.
- **The fix is the name only.** Patch 0360 publishes the same class a second time as `IOSurfaceRoot`.

### 3.5 What WindowServer does not need from us

- **IOGPU / AGX user clients.** Apple's own Metal driver (`MTLIOAccelDevice`, IOGPUFamily) would open
  them. Our device is the frontend's object and talks only to StarbiteMali.
- **`IOGLBundleName` / OpenGL.**
- **The kernel accelerator service,** as long as the injection publishes the device (3.2).

---

## 4. What our kernel provides, and what is macOS-only

| Piece | macOS 26.5 WindowServer/CA needs | Kernel today (iOS 26) | macOS-only work |
|---|---|---|---|
| Metal device | `MTLCopyAllDevicesWithObserver` non-empty within 30 s | StarbiteMali (the GPU) + `IOAcceleratorES` (iOS name, `sbmtl=1`) | **userland:** `sbmac.dylib` + the macOS bundle (built). Optional kernel: `IOAccelerator` (0361, `sbmtl=2`) + SIP off in the ADT |
| GPU command path | our own UAPI | StarbiteMali user client, unchanged | none (the same selectors from a macOS process) |
| IOSurface | `IOSurfaceRoot` by name, 0xc68 ABI | `IOCoreSurfaceRoot`, same ABI | **kernel 0360** (name) |
| Display | IOMFB user client, 0x588 swaps, `/arm-io` display-subsystem, canvas size | `AppleCLCD` + ADT `disp0`, 0x588 already handled | none known; watch the once-logged selectors |
| Display attributes / EDID | optional `DisplayAttributes` dict | none | optional property (7.7) |
| Present to the panel | SwapEnd → panel | GPU scale for 828x1792 (0332), CPU otherwise | a new GPU blob only for MAC3's landscape canvas |
| Injection | `DYLD_INSERT_LIBRARIES` + keys-off for arm64 libs into arm64e WindowServer | 0316 + 0324 + AMFI op 90, generic `exec` code | none in the kernel; 3.6 below |
| Shader cache | writable dir, or none | — | runtime patch `m2v-0001` (read-only card root) |
| Pixel format | 8-bit BGRA framebuffer | iOS: sbca patches `framebuffer_pixel_format` | `CAEnableDeepFramebuffer=false` from sbmac; code patch if needed (7.4) |

### 3.6 Does sweetinject's kernel side work for a macOS process?

Yes, by reading the code. On iOS 26 the same code is proven. Not run on macOS yet.

- **Name match.** 0316 matches the last path component of the exec path.
  `/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer` (launchd job
  `com.apple.WindowServer`, args `-daemon`) is "WindowServer". loginwindow is
  `.../loginwindow.app/Contents/MacOS/loginwindow`.
- **Nothing iOS-specific.** The code is in `exec_extract_strings` and `exec_add_apple_strings` (0324)
  under `ARM64_BOARD_CONFIG_SWEET || A5XELTE`, which every exec passes through. macOS's `posix_spawn`
  from launchd is the same `exec`.
- **dyld honours the variable.** dyld-13xx on macOS honours `DYLD_INSERT_LIBRARIES` and interposing
  when `amfi_check_dyld_policy_self` allows them. Our op 90 answer allows them whenever the
  `sweetinject` boot-arg exists. With AMFI flags present, a `__RESTRICT` segment does not matter.
  WindowServer has none anyway.
- **Library validation is not enforced.** WindowServer's code directory carries
  `flags=0x2000(library-validation)`, but nothing enforces it in our kernel: there is no AMFI kext, as
  for iOS 26 backboardd, which loads the ad-hoc sbca.
- **WindowServer drops to `_windowserver`, uid 88.** Everything the device needs from that user
  still works:
  - `/dev/console` is 0622 (`devfs_vfsops.c`);
  - StarbiteMali has `IOUserClientEntitlements = false` and no privilege check;
  - `AppleCLCD` and `SweetSurface` check nothing;
  - the shader-cache directory needs 0777, or the read-only fallback.
- **Limits.**
  - The boot-arg buffer is 512 bytes, and the whole command line at most 608 (shim).
  - `sweetinject=/usr/lib/sbmac.dylib,WindowServer` is 45 bytes.
- **An arm64 test binary does not need injection.** iOS 26 ran the arm64 `vktest` as a launchd job
  without injection (build/ios26-t3-log.txt). If macOS's dyld refuses an arm64 main executable,
  add `sbmtltest` to `sweetinject`; the kernel then passes `ptrauth_disabled=1`.

---

## 5. What is built (`tools/macos/metal/`)

| File | What |
|---|---|
| `build.sh` | `mount`, `bridge`, `panvk`, `bundle`, `shim`, `test`, `dest`, `stage DIR`, `all`. Intermediates go in an APFS sparse image on the external drive (`/Volumes/Untitled/starbite-metal/sbmtl-build.sparseimage`, mounted at `/Volumes/sbmtl-build`: Rust target 1.5 GB, Mesa 0.2 GB). exFAT puts `._` files beside every file, which Cargo build scripts that hash `src/` would read. `TMPDIR` is inside the image too. Products are copied to `out/macos-metal/` (34 MB, git-ignored). |
| `StarbiteMetalMac.mm` | The macOS principal class `StarbiteMTLDevice` and `sbmtl_device()`/`sbmtl_status()`. Contents: the client gate (`/private/var/local/mtl2-clients`, default `WindowServer sbmtltest`); the driver path `Contents/Frameworks/libvulkan_panfrost.dylib`; the PanVK environment; a shader-cache directory search plus `METAL2VULKAN_CACHE_OPTIONAL=1`; the CA preferences; `sys_dcache_flush` with `dc civac` (the Mali does not snoop); and the iOS refusals (no stitched/dynamic libraries or pipeline archives). |
| `sbmac.m` | The injection. `__DATA,__interpose` for `MTLCopyAllDevices`, `MTLCopyAllDevicesWithObserver` and `MTLCreateSystemDefaultDevice`, each returning retained like Metal's. The first call dlopens the bundle, gets the device, calls the original `MTLCopyAllDevices` (Metal's list now exists), then `MTLAddDevice`. The constructor sets `CAEnableDeepFramebuffer`/`CAEnableFramebufferCompression`/`CAEnableIOSurfaceCompression` = false and `CA_DISABLE_FRAMEBUFFER_COMPRESSION=1`. Logs `SBMAC[pid name]:` on `/dev/console`. |
| `sbmtltest.m` | The test of section 6. |
| `Info.plist`, `mtl2-clients`, `com.starbite.sbmtltest.plist` | Bundle metadata, the default client list, the launchd job. |
| `patches/m2v-0001-shader-cache-optional.patch` | metal2vulkan runtime: with `METAL2VULKAN_CACHE_OPTIONAL=1`, a cache directory that cannot be made or written is not an error (every read and write is a miss). The card root is read-only, and the runtime otherwise refuses the device. Applied only to the macOS copy (`$WORK/m2v-src`), so iOS's source digest does not change. |
| `re/xref.py`, `re/dsc.py` | Low-memory shared-cache tools: strings and CFStrings, ADRP references, stub/GOT resolution through the cache's own mappings and `.map`, annotated disassembly. `ipsw dsc extract --stubs --objc` needed 8 GB of RAM and left a 1.7 GB `.a2s` in `$TMPDIR` (since deleted); these need about 100 MB. |

The sources are shared with iOS (unchanged): `tools/metal2/NativeBridgeMetalFrontend.mm`,
`tools/metal2/bridge/*.rs`, `build/m2v/metal2vulkan` (720ef1ce + `tools/metal2/bridge/patches`), and
`build/mesa-ios/mesa-26.2.6` with the PanVK patch and starbite-drm. All the `__APPLE__` changes apply
to macOS as they are.

Build settings: Xcode 16.4, MacOSX15.5 SDK, `-arch arm64 -mmacosx-version-min=14.0`
(`LC_BUILD_VERSION` platform 1). Rust 1.98.0 `aarch64-apple-darwin`. Times: bridge 4 min, PanVK
2 min (host tools reused from `build/mesa-ios/build-clc-host`), bundle under 1 min.

**Products** (`out/macos-metal/SHA256SUMS`):

| Path on the root | sha256 | Size |
|---|---|---|
| `/System/Library/Extensions/StarbiteMetal.bundle/Contents/MacOS/StarbiteMetal` | `91951f9bcf242f271bf51700498a54b6f7c80259653415a7eb565672ad79ffa8` | 17,753,968 |
| `.../StarbiteMetal.bundle/Contents/Frameworks/libvulkan_panfrost.dylib` | `4555f540167e84ea96ae2c3bc47b6cf4bc440c12a91b60a5b14f2ba97d69dbce` | 17,416,224 |
| `.../StarbiteMetal.bundle/Contents/Info.plist` | `964460fbdad5d3f41113ebaffd50219cad3b6e7f5b6cd32b033bd7679b6cfec6` | |
| `/usr/lib/sbmac.dylib` | `3d81fb90bda922ab7445bf23aed4e9c38f50fcba00b270851f1150242c2aac06` | 51,648 |
| `/usr/local/bin/sbmtltest` | `2538f5e99b3622cac4f44504070235e248f24c946153cc70f35b66393dcb70a3` | 54,112 |
| `/Library/LaunchDaemons/com.starbite.sbmtltest.plist` | `e7223987f5bad564a308b5c9a72105ad3fb191743c863857b83910552b9ee4bd` | |
| `/private/var/local/mtl2-clients` | `2b518be3d7ca4d8b54563bee52687686d616d765edfedd90b5798efe54f5a92f` | |

Symbols for `atos` are in `/Volumes/sbmtl-build/out/sym/` (StarbiteMetal, libvulkan_panfrost.dylib,
sbmac.dylib and sbmtltest, with dSYMs).

**Kernel patches** (written against `build/xnu-12377` at f1e38734; not compiled here). They are
numbered 0360+ so that they do not collide with the a64e work in 0351-0352:

| Patch | sha256 | What |
|---|---|---|
| `xnu/patches/pending-macos26/0360-sweet-iosurface-root-macos-name.patch` | `600dceba...28e49` | `class IOSurfaceRoot : IOCoreSurfaceRoot` + a KernelConfigTables personality. iOS keeps using the other name. **Needed.** |
| `xnu/patches/pending-macos26/0361-starbite-metal-accelerator-macos.patch` | `1e44e1ac...b0a88` | `IOAccelerator` + `StarbiteAcceleratorMac` (`MetalPluginName` StarbiteMetal, `MetalPluginClassName` StarbiteMTLDevice) with `sbmtl=2`. **Optional** (3.2). |

Both were checked by applying them to copies of the three files and comparing with the edited copies.

**Host checks done** (this Mac, macOS 27.2; no Mali here):

1. **The bundle in a macOS process** (`sbmtltest --bundle ... --no-present`).
   - Step 2 IOSurface PASS.
   - The bundle loads ("SBMTL[...]: bundle loaded").
   - With `STARBITE_REGISTRY_ID` forced: PanVK loads, Vulkan has no physical device, "device: NoDevice"
     in 340 ms.
   - Without the override: "no StarbiteMali service".
2. **The injection** (`DYLD_INSERT_LIBRARIES=sbmac.dylib`, a probe calling
   `MTLCopyAllDevicesWithObserver`).
   - "SBMAC[...]: injected", the bundle loads and the device is refused (no Mali).
   - The interposer logs, and the caller gets Metal's own list (1, the M4).
   - The first build crashed here (+0 return for a retained API); fixed.
3. **MTLAddDevice with a dynamic `_MTLDevice` subclass:**
   - with the frontend's overrides, 1 → 2 devices, also through `MTLCopyAllDevicesWithObserver`;
   - without them, an exception in `initLimits`.

---

## 6. The first phone test

**Test M1: "sbmtltest from the card root".** It needs no WindowServer, no loginwindow and no display
stack beyond our kernel's, so it can run as soon as the 26.5 root's launchd starts LaunchDaemons. It
is the same idea as `com.starbite.vktest` on iOS 26.

What it proves, step by step (one `SBMTLTEST:` line each, on the console, so in pmsg):

| # | Step | Pass means |
|---|---|---|
| 1 | kernel | StarbiteMali, `IOSurfaceRoot` (0360) and an IOMobileFramebuffer service exist for a macOS process |
| 2 | iosurface | macOS 26.5's IOSurface.framework creates, locks, writes and reads a surface on SweetSurface |
| 3 | device | the Mali's MTLDevice: PanVK + metal2vulkan + frontend built for macOS, StarbiteMali opened from macOS userland |
| 4 | wscheck | **WindowServer's gate:** after `MTLAddDevice`, `MTLCopyAllDevicesWithObserver()` (SkyLight's exact call) returns ≥ 1 |
| 5 | clear | a Metal render pass runs on the Mali; the readback is (0.25, 0.5, 0.75) → 0xff4080bf |
| 6 | shaders | WindowServer's own `SkyLightShaders.air64.metallib` loads, and every vertex/fragment pair becomes a pipeline (AIR → SPIR-V → Bifrost); refusals are named |
| 7 | present | a panel-sized IOSurface is cleared and striped by the GPU, checked by the CPU, and swapped with IOMobileFramebuffer for 30 s |

Steps 3-6 do not depend on 0360. The test continues after a failed step 1 or 2 and stops only if
there is no device (step 3), so a kernel without 0360 still shows how far the GPU path goes.

### 6.1 Preparation (main session)

1. **Kernel.** The 12377 kernel the macOS 26.5 boots use, plus
   `xnu/patches/pending-macos26/0360-sweet-iosurface-root-macos-name.patch`. 0361 is not needed.
2. **Root.** The 26.5 root plus `tools/macos/metal/build.sh stage <root dir>`. It copies the bundle to
   `/System/Library/Extensions/`, `sbmac.dylib` to `/usr/lib/`, `sbmtltest` to `/usr/local/bin/`, and
   the plist to `/Library/LaunchDaemons/`. It creates `/private/var/local/{mtl2-clients,m2v-cache}`
   (0777).
   - Owners root:wheel. `hfs-owners.py` defaults to 0:0. launchd ignores a plist that is not
     root-owned or is group/world-writable (0644).
   - For M1 only: WindowServer's job stays out (disabled, as `starlte-disable-jobs.py` does for iOS),
     so `sbmtltest` owns the panel.
3. **Command line.** The 26.5 boot's command line (the `macos13-t1.txt` pattern), plus:
   - `sweetinject=/usr/lib/sbmac.dylib,WindowServer`, harmless for M1 and needed for M2;
   - `a5fb_noconsole=1`, so the console stops drawing at the first swap (its lines still reach pmsg);
   - `sweetreboot=300`.

   Keep `sweetfb_scale=1 sweetfb_lw=828 sweetfb_lh=1792 sweetfb_gpu=1`, for the kernel's 828x1792
   GPU present. No `sbmtl=`.

### 6.2 Expected

```
SBMTLTEST: start: macOS 26.5.0, pid N uid 0
SBMTLTEST: 1 kernel PASS StarbiteMali 0x..., IOSurfaceRoot 0x... (IOCoreSurfaceRoot 0x...), IOMobileFramebuffer x1, IOAccelerator x0
SBMTLTEST: 2 iosurface PASS IOSurfaceCreate 64x64 BGRA: made, id N, bytes per row 256
SBMTL[N sbmtltest]: bundle loaded
SBMTL[N sbmtltest]: no writable shader cache directory: translations stay in memory (...)     (read-only root)
SBMTL[N sbmtltest]: device <PanVK's name> in ... ms (driver .../Contents/Frameworks/libvulkan_panfrost.dylib ...)
SBMTLTEST: 3 device PASS ...
SBMTLTEST: 4 wscheck PASS Metal listed 0 before, MTLAddDevice(ours); MTLCopyAllDevicesWithObserver -> 1; MTLCreateSystemDefaultDevice -> ...
SBMTLTEST: 5 clear PASS 256x256 BGRA8 clear + readback in ... ms: center 0xff4080bf
SBMTLTEST: 6 shaders PASS SkyLightShaders: loaded (...); N pairs: N pipelines made, 0 refused, ... ms
HEROLTE FB: rec 0 size 0x588 ...          (the kernel's dump of the first swap records)
SBMTLTEST: 7 present: surface N 828x1792, CPU reads bar 1 0xff6a3cff (want 0xff6a3cff), clear 0xff0a081f
SBMTLTEST: 7 present PASS IOMFB ... 828x1792, Metal texture over the IOSurface made, ... 31 swaps ...
SBMTLTEST: RESULT PASS: the Mali-G72 draws for macOS userland through Metal
```

**On the panel:** eight horizontal bars (night blue, violet, amber, pink, red, green, blue, white).
One of them shows the dark clear colour, and that gap moves down one bar per second for 30 s.

**Pass:** steps 3, 4 and 5 PASS. That is the Metal device WindowServer needs, from macOS 26.5's own
Metal.framework, on the Mali. 6 and 7 are the next two links: WindowServer's shaders, and the IOSurface
+ IOMFB path its CoreAnimation uses.

**Interesting failures:**

| Symptom | Meaning |
|---|---|
| `1 kernel FAIL ... lacks pending-macos26/0360` (2 and 7 fail too) | the kernel lacks the IOSurfaceRoot name |
| `dlopen: ... incompatible platform` or `... (have 'arm64', need 'arm64e')` | dyld grading; add `sbmtltest` to `sweetinject` (3.6) |
| `device refused: device: NoDevice` | PanVK found no Mali: starbite-drm could not open StarbiteMali, or a GET_PARAM answer differs |
| `device refused: ... ShaderCache` | the cache patch is missing from the bridge build |
| `MISSING NativeBridgeMetalDevice -<selector>` | a selector macOS asks for that iOS never did; add it to the frontend |
| a crash in `MTLAddDevice` | 26.5's `initLimits` family differs from 27.2's |
| `6 shaders FAIL ... first refusal` | the metal2vulkan/PanVK gap list for WindowServer (framebuffer fetch, formats, descriptor counts) |
| `7 present` swaps fail, or the panel shows nothing | the 0x588 record's layer fields; the kernel's `HEROLTE FB: rec` dump shows them |

### 6.3 Test M2: WindowServer passes its Metal check (right after M1)

Same image, with WindowServer's job enabled and `sweetinject=/usr/lib/sbmac.dylib,WindowServer`.

**Expected:**
```
SWEET INJECT: /System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer gets DYLD_INSERT_LIBRARIES=/usr/lib/sbmac.dylib
SBMAC[N WindowServer]: injected (uid 0)
SBMTL[N WindowServer]: bundle loaded
SBMAC[N WindowServer]: Metal device "..." registry id 0x... published with MTLAddDevice in ... ms: MTLCopyAllDevices now has 1
SBMAC[N WindowServer]: MTLCopyAllDevicesWithObserver -> 1 device(s) (WindowServer's Metal check needs one)
```

**Pass:** no "No suitable Metal devices present after 30 seconds." and no WindowServer abort or
respawn loop for 60 s after those lines. **Next:** CA's display bring-up messages (`[ Display:Config ]`
lines), the first swaps (`SWEET FB: swap ... surface ...`), and once-logged IOMFB selectors.

---

## 7. Missing pieces, in priority order

1. **Kernel 0360 (IOSurfaceRoot) compiled and booted.** Every IOSurface in macOS userland depends on
   it: WindowServer, CA and the present.
2. **The 26.5 root with the staged files** (6.1), their owners, and WindowServer's job.
   - The card root is read-only: the shader cache stays in memory, and every boot compiles cold.
   - WindowServer also writes preferences and `/private/var/folders`. That is MAC1/MAC2's problem
     (a writable `/private/var`).
3. **Re-check the 27.2 findings on 26.5's cache** (section 8): the gate string and its 30 s, the
   `MTLAddDevice` preconditions, the IOSurface name, CA's IOMFB use.
4. **CA's framebuffer format in WindowServer.**
   - iOS 26 needed sbca's code patch of `IOMFBDisplay::framebuffer_pixel_format` → 'BGRA'; metal2vulkan
     has no 'b3a8'.
   - macOS 27.2's version also chooses by EDR headroom ('RGhA' half-float when headroom > threshold) and
     by flags.
   - sbmac sets `CAEnableDeepFramebuffer=false`. If WindowServer's swaps are refused or a pipeline
     fails with `Unsupported(Format)`, port sbca's `patch_instruction` to sbmac with 26.5's
     `CA::WindowServer::IOMFBDisplay::framebuffer_pixel_format` address (`re/dsc.py`; 27.2:
     0x18e09f1c4).
5. **Frontend gaps that only macOS shows.**
   - Selectors SkyLight and macOS CA send that iOS never did (MISSING lines).
   - `supportsFamily:` answers for the Mac families (`MTLGPUFamilyMac2`).
   - `isHeadless`/`isLowPower`/`isRemovable`/`location`.
   - SkyLight shader refusals (step 6).
6. **The GPU ↔ display link.** macOS CA has `CAMetalContextCreateWithGPURegistryID` ("Could not find
   Metal device with registry id %llx": the HDR processor and some contexts). Our device's
   `registryID` is StarbiteMali's entry. If a display asks for another ID, name the GPU on the IOMFB
   service or answer `registryID` with what CA asks for.
7. **IOMFB properties macOS reads that iOS did not:** `DisplayAttributes` (`ProductAttributes`
   ManufacturerID/ProductID/name), and possibly `IOMFBNumLayers`, `DisplayPipeStrideRequirements`,
   `IOMFBMaxSrcPixels` and the brightness properties. Add them to `AppleCLCD::start` as the logs ask.
   This Mac's values are in `/Volumes/Untitled/starbite-metal/ref/iomfb.txt`.
8. **More Metal clients:** loginwindow, Dock, the Finder, etc.
   - Each must be in `sweetinject` (keys-off for the arm64 bundle) and in `mtl2-clients`.
   - The 512-byte sweetinject buffer and the 608-byte command line limit this. A wildcard injection,
     or 0324's keys-off for every arm64e process (IOS-26-Hot 0129), would lift it.
   - The proper route: 0361 + SIP off in the ADT, so Metal's own discovery loads the bundle.
9. **A writable shader cache.** Cold translation on 4 A55s costs seconds to minutes per pipeline set;
   iOS needed priming.
10. **MAC3's landscape canvas** needs its own GPU present blob (0332 is 828x1792 only).

---

## 8. How to re-check on the 26.5 cache

Once the 26.5 root (or just its SystemOS cryptex cache) exists, read it in place:

```sh
IPSW=build/tools/ipsw/ipsw; DSC=<root>/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e
X=tools/macos/metal/re/xref.py; D=tools/macos/metal/re/dsc.py; cd /Volumes/Untitled/starbite-metal
mkdir -p dsc265
for i in SkyLight Metal QuartzCore IOSurface IOMobileFramebuffer libsystem_sandbox.dylib; do
  TMPDIR=$PWD/tmp $IPSW dsc extract $DSC $i -o dsc265 --slide      # never --stubs/--objc (8 GB RAM)
done
python3 -I $X find dsc265/SkyLight "No suitable Metal"              # -> WSSystemCanCompositeWithMetal
DSC_EXTRACT=dsc265 python3 -I $D dis $DSC dsc265/SkyLight <start> <end>   # calls + strings resolved
python3 -I $X str dsc265/IOSurface IOSurfaceRoot                     # the root's name
nm -u dsc265/QuartzCore | grep -c _IOMobileFramebuffer               # CA drives IOMFB
nm -n dsc265/QuartzCore | grep framebuffer_pixel_format              # 7.4's address
```

The macOS 26.5 build of `sbmtltest` also answers 3.2 on the phone itself (step 4).

---

## 9. Sources

- **macOS 27.2 (26B5112a) on this Mac, read-only.** The shared cache at
  `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/`, and images extracted to
  `/Volumes/Untitled/starbite-metal/dsc27/`.
  - SkyLight: `___WSSystemCanCompositeWithMetal_block_invoke` 0x1828bed28,
    `SLCADisplay::copy_ioav_display_attributes`.
  - Metal: `getMetalPluginClassForService` 0x1941007e0, `MTLAddDevice` 0x193f3a5ec,
    `MTLIOAccelServiceGlobalContext`.
  - libsystem_sandbox: `gpu_bundle_find_trusted` 0x18fe601dc, `rootless_check_trusted_internal`.
  - QuartzCore: `CAMetalContextCreate` 0x18dcf8914, `IOMFBDisplay::framebuffer_pixel_format`
    0x18e09f1c4.
  - IOMobileFramebuffer: `iomfb_populate_all_display_infos`.
  - IOSurface: `____ioSurfaceConnectInternal_block_invoke`.
  - CoreDisplay: `CGXMappedDisplayStart`.
- **This Mac's registry (`ioreg`), saved in `/Volumes/Untitled/starbite-metal/ref/`.**
  - `AGXAcceleratorG16G` (`MetalPluginName` AGXMetalG16G_B0, `MetalPluginClassName` AGXG16GDevice,
    `IOGLBundleName`, `model`, `gpu-core-count`).
  - `IOMobileFramebufferShim` on `disp0` (DisplayAttributes, TimingElements, ColorElements, ...).
  - `IOSurfaceRoot`.
  - WindowServer runs as `_windowserver` (88). Its code directory has `library-validation`.
- **macOS 26.5 KDK** (`build/kdk/KDK_26.5_25F71.kdk`).
  - `IOSurface.kext`: class `IOSurfaceRoot`, `IOSurfaceRootUserClient::sMethodDescs`, 70 entries.
  - `IOMobileGraphicsFamily.kext`: the `IOMobileFramebufferUserClient` dispatch table at 0x51ec0,
    24-byte entries; `swap_submit` struct 0x588.
- **Repo.**
  - `NOTES.md` 2026-11-01 .. 11-02 (Metal on iOS 26), `build/metal-port-plan.md`.
  - `tools/metal2/`, `tools/mesa-ios/`, `tools/ca-gpu/sbca.c`.
  - `xnu/series-12377/0316, 0324, 0322, 0336`.
  - `build/xnu-12377/iokit/Kernel/arm/{StarbiteMali,SweetFramebuffer,SweetSurface,StarbiteAccelerator}.cpp`,
    `bsd/kern/{kern_csr.c,sweet_amfi.c}`.
  - `docs/MACOS.md`.
- **Licences.**
  - The frontend and bridge are MIT (IOS-26-Hot/UniXNU).
  - metal2vulkan is GPL-2.0-or-later OR LGPL-3.0-or-later, linked statically: keep the source with
    the image, as for iOS.
  - Mesa is MIT.
  - Apple's files never leave `build/`, the external drive and the card.
