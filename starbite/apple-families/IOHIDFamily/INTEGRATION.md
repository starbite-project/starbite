# IOHIDFamily: building it into the kernel (for the integrating session)

Apple's IOHIDFamily-2238.120.5 (macOS 26.5, the release that ships xnu-12377.121.6) compiled into our xnu-12377
STARLTE kernel, so StarbiteTouch and StarbiteButtons subclass the **real** `IOHIDEventService`
(`starbite/apple-drivers/StarbiteTouch/INTEGRATION.md` 1) and backboardd / WindowServer get Apple's user clients.
It replaces our stand-ins in `iokit/Kernel/arm/hid/`. Nothing here has been applied to `build/xnu-12377` or
`build/dxb-12377`.

Status: **85 of 85** sources in SOURCES.txt compile-check clean (macOS target; warnings only). Left out with a reason
(SOURCES.txt bottom, NOTES.md): the IOHIDEventDriver group (4 files, **blocked**: unpublished Apple-vendor usage
tables), IOFastPath + IOHIDComplexEventDriver (5 files, proprietary licence header, unpublished headers), 4 files
Apple does not publish. Link-level check (objects with the kernel's flags, `nm` against
`kernel.development.starlte.unstripped`): nothing missing (`___chkstk_darwin` and `___dso_handle` resolve inside the
kernel link), **238 duplicate symbols = exactly our stand-in classes** (section 5), which must go.
StarbiteTouch.cpp and StarbiteButtons.cpp compile-check against these headers (instead of the SDK's).

## 1. Stage

```
sh starbite/apple-families/stage.sh IOHIDFamily     # clone, patches 0001-0003, header tree, iig
sh starbite/apple-families/check.sh IOHIDFamily     # "IOHIDFamily: 85 of 85 OK"
```

| Path (gitignored) | What |
|---|---|
| `build/families/IOHIDFamily/` | clone at `IOHIDFamily-2238.120.5`, with `patches/0001..0003` applied |
| `build/families/_inc/IOHIDFamily/` | include tree (symlinks): `IOKit/hid/*.h` (family headers + the 4 adapted event headers from `missing-headers/`), `IOKit/hidevent/`, `IOKit/hidsystem/`, `HIDDriverKit/` (family headers + iig output), `IOKit/graphics/IOGraphics{Types,Device}.h` and `IOKit/usb/*.h` (constants/types only, from the 15.5 SDK's Kernel.framework), `compat/` headers (`math.h`, `IOKit/IOReporter.h`, `ipc/ipc_port.h`), `TargetConditionals.h` |
| `build/families/_gen/IOHIDFamily/HIDDriverKit/IOHID{Device,Interface,EventService}.{h,iig.cpp}` | iig output (Xcode 16.4, DriverKit-427.100.5) from `HIDDriverKit/*.iig`, parsed against the DriverKit 24.5 SDK as Apple's `iigFramework.sh` does |

## 2. Kernel tree layout (suggested)

```
D=build/xnu-12377/iokit/Families/IOHIDFamily
mkdir -p $D/include
cp -R build/families/IOHIDFamily/IOHIDFamily build/families/IOHIDFamily/IOHIDSystem $D/   # patched clone, 4 MB
cp build/families/_gen/IOHIDFamily/HIDDriverKit/*.iig.cpp $D/
cp -RL build/families/_inc/IOHIDFamily/. $D/include/              # -L: the staged tree is symlinks
cp starbite/apple-families/kext-view.h build/xnu-12377/iokit/Families/kext-view.h   # if not there yet
```

Copying both source folders whole keeps every header the sources include by quoted name (`IOHIDFamily/*.h`,
`IOHIDSystem/*.h`, `IOHIDSystem/IOKit/hidsystem/*.h`, `IOHIDSystem/IOHIDDescriptorParser/*.h`,
`IOHIDEventDummyService/*.h`); only the SOURCES.txt entries are compiled. Do not copy `IOFastPath/` or
`IOHIDComplexEventDriver/` (proprietary licence header, NOTES.md).

## 3. conf/files and flags

One line per SOURCES.txt entry, `optional iokitcpp` (paths under `iokit/Families/IOHIDFamily/`), e.g.

```
iokit/Families/IOHIDFamily/IOHIDFamily/IOHIDEventService.cpp          optional iokitcpp
iokit/Families/IOHIDFamily/IOHIDFamily/IOHIDEvent.cpp                 optional iokitcpp
...
iokit/Families/IOHIDFamily/IOHIDSystem/IOHIDDescriptorParser/HIDParseDescriptor.c   optional iokitcpp
iokit/Families/IOHIDFamily/IOHIDEventService.iig.cpp                  optional iokitcpp
```

On an **iOS-target** kernel leave out the `[osx]` entries (SOURCES.txt tags them as in Apple's IOHIDFamily-Embedded
target).

`iokit/conf/Makefile.template` (inside the board filter; the `bsd/sweet_hfs` mechanism):

```make
IOHF_ROOT = $(SRCROOT)/iokit/Families/IOHIDFamily
IOHF_OBJS = <the .o of every SOURCES.txt entry>
$(foreach o,$(IOHF_OBJS),$(eval $(o)_CFLAGS_ADD += -include $(SRCROOT)/iokit/Families/kext-view.h))
$(foreach o,$(IOHF_OBJS),$(eval $(o)_INCFLAGS += -I$(IOHF_ROOT)/include -I$(IOHF_ROOT)/include/IOKit/hid \
	-I$(IOHF_ROOT)/IOHIDFamily -I$(IOHF_ROOT)/IOHIDFamily/IOHIDEventDummyService -I$(IOHF_ROOT)/IOHIDSystem \
	-I$(IOHF_ROOT)/IOHIDSystem/IOKit/hidsystem -I$(IOHF_ROOT)/IOHIDSystem/IOHIDDescriptorParser -I$(IOHF_ROOT)))
```

(the same `-I` list `starbite/apple-families/check.sh` uses; `include/IOKit/hid` is there because
`IOHIDEvent.cpp` includes `"IOHIDEventTypes.h"` quoted). No `-D` is needed: Apple's project defines none that the
sources test. `kext-view.h` gives the sources the Kernel.framework view of XNU's headers (XNU's
`*_KERNEL_PRIVATE` and `IOKITSTATS` cleared): with the kernel's private view `IOHIDShared.h` includes
`<IOKit/system.h>` inside `extern "C"` and meets C++ templates; with the kext view nothing needs patching there.

The `.c` files were checked with kcheck's BSD C flags (`herolte_ufs.c`'s); under `iokitcpp` they get the iokit C
flags, which differ only in `-D` options the parser does not test.

## 4. Personalities (KernelConfigTables.cpp)

Paste `personalities-osx.kct.inc` (macOS target, from `IOHIDFamily/Info-IOHIDFamily.plist`) or
`personalities-ios.kct.inc` (iOS target, from `Info-IOHIDFamily-Embedded.plist`); generated by
`starbite/apple-families/plist2kct.py`, OSUnserialize syntax checked.

| Personality | What it does |
|---|---|
| `IOHIDEventServiceUserClient` (class `IOHIDProviderPropertyMerger` on `IOProviderClass = IOHIDEventService`) | **The HIDServiceSupport merge**: copies `HIDServiceSupport = Yes`, `IOUserClientClass = IOHIDEventServiceUserClient` and the IOHIDLib plug-in types onto every IOHIDEventService. backboardd / hidd / WindowServer only open services that carry `HIDServiceSupport`; StarbiteTouch/Buttons inherit it from here |
| `IOHIDLibUserClient` (merger on `IOHIDDevice`) | IOHIDLib plug-in types + `IOHIDLibUserClient` for HID devices |
| `IOHIDResource` (IOResources, `IOResourceMatch IOBSD`) | `IOHIDUserDeviceCreate` (virtual HID devices) |
| `IOHIDSystem` (IOResources, `IOResourceMatch IOBSD`, macOS only) | the real IOHIDSystem with Apple's properties (`HIDServiceSupport`, scroll-count defaults, NX event router plug-in); **replaces** the existing stand-in personality of the same class |
| `R1 Merge` | `ProtectedAccess` on one Apple vendor device class; harmless |

Left out on purpose: `HID Game Controller Pointing Driver` (IOClass `IOHIDEventDriver`, not built),
`IOHIDBTCETimeSyncService` (class unpublished), and on iOS `ShuttleExpress` / `StarkPorscheWheel`
(`IOHIDEventOverrideDriver`, not in the kext target).

The `IOCFPlugInTypes` values are paths relative to `/System/Library/Extensions`
(`IOHIDFamily.kext/Contents/PlugIns/IOHIDLib.plugin`); the OS root's own IOHIDFamily.kext bundle provides them, as
on a Mac. Nothing in the kernel needs the bundle.

## 5. What it replaces (must be removed in the same change)

The link check finds 238 symbols defined both by the family and by today's kernel, all from our stand-ins:

| Today (`iokit/conf/files.arm64`) | Classes duplicated | Action |
|---|---|---|
| `iokit/Kernel/arm/hid/SweetHID.cpp` (0058/0323) | `IOHIDEventService` (36 symbols) + `SweetHIDButtons`, `SweetHIDTouch` | remove; StarbiteTouch/Buttons replace the services |
| `iokit/Kernel/arm/hid/IOHIDEvent.cpp` (1915 copy + 0323) | `IOHIDEvent` (87) | remove (object name `IOHIDEvent.o` also collides) |
| `iokit/Kernel/arm/hid/IOHIDEventServiceQueue.cpp` | `IOHIDEventServiceQueue` (26) | remove (object name collides) |
| `iokit/Kernel/arm/hid/IOHIDEventServiceUserClient.cpp` | `IOHIDEventServiceUserClient` (46) | remove (object name collides) |
| `iokit/Kernel/arm/hid/SweetHIDSystem.cpp` (0367/0368) | `IOHIDSystem` (20), `IOHIDUserClient` (23) | remove; macOS gets the real IOHIDSystem (`[osx]` entries) |

KernelConfigTables: remove the `SweetHIDButtons`, `SweetHIDTouch` and stand-in `IOHIDSystem` entries.

C entry points that live in SweetHID.cpp today, `sweet_hid_touch()`, `sweet_hid_key()`, `sweet_last_input_abs`,
`sweet_last_present_abs` (called from `bsd/dev/{starlte,herolte,a5xelte,sweet}_touch.c`, `a5xelte_keys.c`,
`sweet_keys.c`, `SweetFramebuffer.cpp`, `starbite/StarbiteDisplay/StarbiteDisplay.cpp`): StarbiteTouch and
StarbiteButtons define them with `SB_SWEET_HID_COMPAT=1` (their INTEGRATION.md), so those callers link unchanged.
The headers in `iokit/Kernel/arm/hid/*.h` must not be on the families' include path (they are not: they were only
reachable as quoted includes from that folder).

`NO_KERNEL_HID` (set in our kernel's options) only disables the display wrangler in `IOPMrootDomain.cpp`; it does not
interact with the family.

## 6. Symbol sets / exports

Nothing needed while everything is built in. For a later Boot KC with Apple's closed HID drivers, a pseudo-kext
`com.apple.iokit.IOHIDFamily` like `config/System.kext/PlugIns/IONVRAMFamily.kext` plus an export list would be
needed; IOHIDFamily publishes no `.exports` file in this tag, so the list would be generated from the built objects.

## 7. Before the first phone test (NOTES.md "Event headers")

The event structs the kernel serializes to user space come from IOHIDFamily-1915.140.3's headers, adapted where
2238's own code forces it. **Only the digitizer layout is verified** (0323, macOS 26.5 KDK DWARF). Keyboard and
consumer events (StarbiteButtons) use 1915's layout unchanged; pointer/translation positions are IOHIDDouble as 2238
writes them. For **macOS Sequoia** userland (IOHIDFamily 21xx, not 2238) the layouts may differ again. Check against
the KDK DWARF (macOS 26.5 for iOS 26; a 15.x KDK for Sequoia) when it is reachable, before relying on touch/keys.
