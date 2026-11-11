# IOHIDFamily: notes

## Source

| | |
|---|---|
| Repository | https://github.com/apple-oss-distributions/IOHIDFamily |
| Tag | `IOHIDFamily-2238.120.5` (tag object `f98990e8b63e1fda98cac9f261a6137d7f05e4c8`) |
| Commit | `e31e90a24c27ad6cbe981650d12a1a4124065b03` (2026-06-15) |
| Why this tag | `distribution-macOS` `macos-265` (macOS 26.5) pins it together with `xnu-12377.121.6` (our kernel) |
| Licence | APSL 2.0 for everything built here (files with the APSL header; a few kernel files, e.g. `IOHIDElementContainer.cpp`, `IOHIDWorkLoop.cpp`, have no header and fall under the project's APSL release). **Excluded on licence grounds:** `IOHIDFamily/IOFastPath/*` and `IOHIDFamily/IOHIDComplexEventDriver/*`, whose headers read "confidential and proprietary ... may not be reproduced" although they sit in the published tree |
| Clone | `build/families/IOHIDFamily` (gitignored, 9.6 MB, `--depth 1`) |

## Result

- **85 of 85** files in SOURCES.txt compile-check clean (`check.sh` -> `tools/apple/kcheck.py`, macOS STARLTE
  flags, `-include kext-view.h`). 0 errors. Warnings (Apple's code, first 80 per file):
  `-Wzero-as-null-pointer-constant` ~800, `-Wextra-semi` ~100, `-Wimplicit-int-conversion` ~90,
  `-Wdocumentation` ~90, `-Wcast-function-type-mismatch` 37, `-Wcast-align` 37, `-Wmissing-field-initializers` 31,
  `-Wdouble-promotion` 27 (NAN is a float), a few `-Wunused-but-set-variable`, `-Watomic-implicit-seq-cst`.
- Throw-away object build + `nm` against `kernel.development.starlte.unstripped`: 0 symbols missing
  (`___chkstk_darwin`, from `IOHIDDevice::postElementTransaction`'s 4 KiB of stack arrays, is a private extern in the
  kernel and is already referenced by `bsd/.../ah_core.o`; `___dso_handle` comes from `os_log` and is synthesized
  by the linker, as for `osfmk/.../analytics.o`). 238 duplicates = our stand-ins (INTEGRATION.md 5).
- StarbiteTouch.cpp and StarbiteButtons.cpp compile-check against `_inc/IOHIDFamily` (no SDK headers).

## Patches (against the clone; `patches/`, applied by stage.sh)

| Patch | Lines | Why |
|---|---|---|
| `0001-AppleHIDUsageTables-restore-constants.diff` | +232 | Every tag since 1915 publishes `AppleHIDUsageTables.h` as a 22-line stub. The body is restored from **IOHIDFamily-870.60.4** (APSL, the last tag that published it), unchanged. Constants 2238 uses that are newer than 870 get **sentinels that never match** a real usage: `kHIDUsage_AppleVendor_Payload` 0x10000 and `kHIDUsage_AppleVendor_MultipleInterfaces` 0x10001 (compared with 32-bit `getUsage()`), `kHIDPage_AppleVendorUARP` / `kHIDUsage_AppleVendorUARP_Input` 0xffff (stored in a `uint16_t` allow list), `kHIDUsage_AppleVendor_Perf` 0xffff (only emitted with the `kIOHIDDebugPerfEvent` debug mask). Effect: Apple-vendor HID devices lose those special cases; the S9 has none |
| `0002-IOHIDPrivateKeys-unpublished-keys.diff` | +105 | Property keys 2238 uses but no published header defines. **String values recovered** from the cstrings of the host's macOS 27.2 `BootKernelCollection.kc` (`/private/var/db/KernelExtensionManagement/KernelCollections/`, read-only `strings`), each next to the class that uses it, in the order the source uses them: `Privileged`, `RegisterService`, `HIDDKStart`, `MultipleInterfaceEnabled`, `SupportsIOFastPath`, `IsSpatialBluetoothHID`, `SupportsHIDInputTopology`, `IOHIDUserDeviceSuspend`, `HIDRMDeviceState`, `DeviceTypeHint`, `Headset`, `ProtectedAccess`, `ExtendedData`, `GameControllerSupportedHIDDevice`, `IOHIDDeviceForceInterfaceRematch`, `TimeSyncEnabled`, `RequiresTCCAuthorization`, `BlessedUsagePairs`, `BlessedUsagePage`, `BlessedUsage`, `DigitizerCollectionDispatch`, `PrimaryVendorUsages`. Not recoverable, set to the safe default: the two property **lists** `kIOHIDPropagatePropertyKeys` / `kIOHIDDeviceMessagePropertyUpdateKeys` are empty (nothing forwarded to HID devices), the private IOHIDLib open options `kIOHIDOptionsPrivateTypeNoDroppedReports` / `IgnoreTaskSuspend` are 0 (default behaviour), `HIDRMDeviceState` is a kernel-internal enum (any distinct values) |
| `0003-IOHIDEventService-no-fastpath-userclient.diff` | +5 | `IOHIDEventServiceFastPathUserClient.{h,cpp}` are not published. Its include and its `newUserClient` branch are under `#if IOHIDFAMILY_FASTPATH_USERCLIENT` (off); a client asking for the fast-path type now gets IOService's answer (unsupported). The documented plan (StarbiteTouch INTEGRATION.md 1) |

## Event headers (`missing-headers/`)

`IOHIDEventTypes.h`, `IOHIDEventStructDefs.h`, `IOHIDEventMacroDefs.h`, `IOHIDEventFieldDefs.h` are referenced by the
2238 tree but not published since 1915. `upstream-1915.140.3/` holds Apple's 1915.140.3 copies byte for byte
(`IOHIDEventFieldDefs.h` is identical to the copy in our kernel tree; the other three in our tree carry 0323's
edits). `adapt-1915-to-2238.py` regenerates the four used headers from them; every change is one that 2238's own
code forces (it does not compile, or would store wrong values, otherwise):

| Change | Evidence | Verified against 26.x? |
|---|---|---|
| `IOHIDDigitizerEventData`: IOHIDDouble members, `angle` union {twist, roll}, 0xa0 bytes (static asserts) | 2238 `IOHIDEvent.cpp` writes `CAST_FIXED_TO_DOUBLE` into them; layout from 0323 (macOS 26.5 KDK DWARF) | **yes** (0323, iOS 26 touch works with it) |
| `IOHIDTranslationEventData`, `IOHIDPointerEventData`: `position.{x,y,z}` IOHIDDouble | 2238 `translationEvent`, `absolutePointerEvent`, `relativePointerEventWithFixed` write `CAST_FIXED_TO_DOUBLE` (with 1915's IOFixed the compiler flagged the truncation) | no |
| Field macros of those members convert with `CAST_x_TO_DOUBLE` / `CAST_DOUBLE_TO_x` (156 lines) | consistency with the structs | follows the structs |
| `IOHIDCollectionEventData` + usagePage/usage/ungroupForLegacy; GameController + `bottom.m1..m4`; heart-rate struct | fields 2238 writes | no (not used on the S9) |
| `kIOHIDEventTypeHeartRate` = 40 (appended before Count), `kIOHIDEventOptionSynthetic` = 0, `kIOHIDDigitizerEventEstimatedRoll` = 1<<31, `kIOHIDDigitizerEventUpdateRollMask` = 0, `kIOHIDHeartRateSensorLocationUnknown` = 0 | identifiers 2238 uses | no; chosen so nothing existing moves and nothing new is ever set |

Everything else (keyboard, scroll, button, consumer, vendor ... layouts and every event-type / field number) is
1915.140.3's. **The kernel serializes these structs to user space**, so they must match the userland's IOHIDFamily:

- iOS 26 / 27 (IOHIDFamily 2238+): digitizer verified; keyboard events with the 1915 layout were in use by 0323's
  iOS 26 tests (Home presses), pointer/translation follow 2238's own code; the rest is unverified.
- **macOS Sequoia** (userland IOHIDFamily 21xx): not checked at all. If 21xx still had IOFixed digitizer/pointer
  members, the 2238 layout would be wrong for it. Check with the matching KDK's IOHIDFamily.kext DWARF (the 26.5 KDK
  is parked on an external disk, `build/kdk/KDK_26.5_25F71.kdk-PARKED.txt`; a 15.x KDK is needed for Sequoia).

FP: 2238 uses `double` (IOHIDDouble, `NAN`) in kernel code, as Apple's own arm64 build of the kext does. 0323 had
avoided FP ("no FP in the kernel") with integer bit conversions. Our kernel flags do not forbid FP (no
`-mgeneral-regs-only`), so it compiles; whether kernel FP use is safe in every context on our kernel (e.g. events
built from an interrupt-filter path) is **not verified** and should be looked at before the first HID phone test.

## Compat headers (`compat/`)

| Header | Why |
|---|---|
| `math.h` | `IOHIDEvent.cpp`/`IOHIDEventDriver.cpp` include `<math.h>` (SDK Kernel.framework forwards to libm's header) but use only `NAN`; defines `NAN` as `__builtin_nanf("")`, nothing else (a real libm call stays a compile error) |
| `IOKit/IOReporter.h` | internal-SDK header name; the classes (`IOHistogramReporter`, `IOReportLegend`) are XNU's, in `<IOKit/IOKernelReporters.h>` |
| `ipc/ipc_port.h` | osfmk-private header, used only for `ipc_port_release_send()`; declared with XNU's signature (exported in `config/Private.exports`) |

Plus, staged (not copied): `IOKit/graphics/IOGraphics{Types,Device}.h` and `IOKit/usb/*.h` from the 15.5 SDK's
Kernel.framework (types/constants only; no IOGraphics or USB-host symbol is referenced at link time).

## iig

`HIDDriverKit/IOHID{Device,Interface,EventService}.iig` -> kernel-side headers + dispatch impls with Xcode 16.4's
iig (DriverKit-427.100.5), parsed with the DriverKit 24.5 SDK (`TARGET_OS_DRIVERKIT=1`) and the family's headers
first, as Apple's `HIDDriverKit/iigFramework.sh`. XNU's own DriverKit `.iig` path cannot parse these (iig defines
`KERNEL` as a class annotation, so XNU's `#if KERNEL` headers break it). Apple's `iigKernelFramework.sh` is not
published. The generated code only names IOService's method IDs; they resolve against our kernel's
`IOService.h` at compile time (checked).

## Left out

| Files | Reason | What is lost |
|---|---|---|
| `IOHIDEventDriver.cpp` | **Blocked**: ~30 Apple-vendor usage constants used as `switch` cases (pages AppleVendorSensor/Motion/Multitouch/HIDEvent/DFR, keyboard LongPress/Language, heart-rate fields ...), unpublished; values not in strings (integers) | the generic HID-report event driver (USB/BT keyboards, mice, sensors) |
| `IOHIDEventRepairDriver.cpp`, `AppleEmbeddedKeyboard.cpp`, `AppleUserHIDEventService.cpp` | subclasses of IOHIDEventDriver (they compile; they cannot link without it) | DriverKit HID event-service dexts; Apple's embedded keyboard class |
| `IOFastPath/*` (3), `IOHIDComplexEventDriver/*` (2) | proprietary licence header; also need unpublished `IOFastPathCommon.h` / `IOHIDTimeSyncService.h` | fast-path sensor services |
| `IOHIDEventServiceFastPathUserClient`, `IOHIDTimeSyncService`, `IOHIDBTCETimeSyncService`, `IOHIDTestTimeSyncService` | not published | fast-path client (patch 0003), BT time sync |
| `IOHIDUtility.cpp` | not part of the kext (user-space filter plug-ins) | - |

The S9 has no HID-report devices (no USB host mode), so nothing on the phone needs IOHIDEventDriver. Unblocking it
needs the enum values, e.g. from the KDK DWARF (if the enums are emitted) or from Apple's headers if they reappear.

## iOS target

Not compile-checked (kcheck has only macOS flags). Known item: `IOHIDDevice.cpp` under `TARGET_OS_IOS` references
`CoreAnalyticsHub` (closed CoreAnalyticsFamily) for inductive-accessory analytics; an iOS kernel needs a small guard
patch there (or a stub class) before it links. `[osx]` files are left out on iOS (SOURCES.txt).
