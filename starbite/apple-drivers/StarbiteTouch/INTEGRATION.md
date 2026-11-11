# StarbiteTouch: integration (for the integrating session)

Built into the kernel for M8 (docs/APPLE-S9-DRIVERS.md Part 2.4, way 1). StarbiteButtons has its own
INTEGRATION.md; both need the same IOHIDFamily step (section 1), done once.

## 1. IOHIDFamily: what our kernel has, and what the real family needs

**Today our tree has no IOHIDFamily.** `build/xnu-12377/iokit/Kernel/arm/hid/` carries our own minimal
`IOHIDEventService` (SweetHID.cpp, 0058/0323) plus four Apple files copied from IOHIDFamily-1915.140.3
(`IOHIDEvent.cpp`, `IOHIDEventServiceQueue.cpp`, `IOHIDEventServiceUserClient.cpp` and their headers, including
`IOHIDEventTypes.h`, `IOHIDEventFieldDefs.h`, `IOHIDEventStructDefs.h`, `IOHIDEventMacroDefs.h`, `IOHIDEventData.h`)
and `SweetHIDSystem.cpp` (0367, macOS IOHIDSystem stand-in). StarbiteTouch/Buttons subclass Apple's **real**
`IOHIDEventService` (`handleStart`, `dispatchRelativePointerEvent`, `dispatchKeyboardEvent`, ...), which the stand-in
does not have, so they need the open-source family built in.

Checked 2026-11-11 against `apple-oss-distributions/IOHIDFamily` tag **IOHIDFamily-2238.120.5** (GitHub tree API):

| Needed | In the 2238.120.5 tree? | What to do |
|---|---|---|
| `IOHIDFamily/IOHIDEventService.{cpp,h}`, `IOHIDEvent.{cpp,h}`, `IOHIDEventData.h`, `IOHIDEventServiceQueue.*`, `IOHIDEventServiceUserClient.*`, `IOHIDUtility.*`, `IOHIDFamilyPrivate.*`, `IOHIDDebug.c`, `IOHIDKeyboard.*`, `IOHIDConsumer.*`, `IOHIDPointing.*`, `IOHIDInterface.*`, `IOHIDDevice.*`, `IOHIDElementContainer.*`, `IOHIDDeviceElementContainer.*`, `IOHIDElementPrivate.*`, `IOHIDEventQueue.*`, `IOHIDReportElementQueue.*`, `IOHIDKeys.h`, `IOHIDPrivateKeys.h`, `IOHIDUsageTables.h`, `AppleHIDUsageTables.h`, `IOHIDEventServiceKeys*.h`, `IOHIDEventServiceTypes.h` | yes | build as `optional iokitcpp` in `iokit/conf/files.arm64` (or a `conf/files` of their own), `-DKERNEL -DKERNEL_PRIVATE`, our arm64 flags |
| `IOHIDSystem/` headers used by the above (`IOKit/hidsystem/IOHIDSystem.h`, `ev_private.h`, `ev_keymap.h`, `IOLLEvent.h`, `IOHIDTypes.h`, `IOHIDPrivate.h`, `IOHIDevicePrivateKeys.h`, `OSStackRetain.h`) | yes | include path; on **macOS** also build `IOHIDSystem/IOHIDSystem.cpp` + `IOHIDevice/IOHIKeyboard/IOHIPointing/IOHIDWorkLoop` (replaces SweetHIDSystem.cpp, 0367/0368) |
| `IOHIDEventTypes.h`, `IOHIDEventTypesBase.h`, `IOHIDEventFieldDefs.h`, `IOHIDEventStructDefs.h`, `IOHIDEventMacroDefs.h` | **no** (referenced by `IOHIDFamily.xcodeproj` but not published) | take 1915.140.3's (already in our tree, APSL) and check each event type / field number against the 26.x userland (backboardd / WindowServer read the serialized fields); 0323 is the precedent |
| `IOHIDEventServiceFastPathUserClient.{cpp,h}` | **no** (same) | drop the `kIOHIDEventServiceFastPathUserClientType` branch of `IOHIDEventService::newUserClient` (one small patch) or stub the class |
| `HIDDriverKit/IOHIDEventService.h` (generated from `HIDDriverKit/IOHIDEventService.iig`), `IOHIDDevice.iig`, `IOHIDInterface.iig` | .iig yes | generate with Xcode 16.4's `iig` (`xcrun --find iig` works), or use the SDK's generated `Kernel.framework/Headers/HIDDriverKit/*.h`; XNU's IOUserServer must be built (it is) |
| `IOKit/usb/IOUSBHostFamily.h` | no (other project) | SDK `Kernel.framework/Headers/IOKit/usb/` (constants only) |
| IOHIDFamily's Info.plist personalities | `Info-IOHIDFamily.plist` / `-Embedded.plist` | add to `KernelConfigTables.cpp` the `IOProviderMergeProperties` entry on `IOProviderClass = IOHIDEventService` (`HIDServiceSupport = true`, `IOUserClientClass = IOHIDEventServiceUserClient`): backboardd and WindowServer only open services carrying `HIDServiceSupport` |

Then delete `iokit/Kernel/arm/hid/SweetHID.cpp` and its four 1915 copies (the real family supplies them) and
the `SweetHIDButtons` / `SweetHIDTouch` personalities. SweetHID.cpp's C entry points are still called by
`bsd/dev/{starlte,herolte,a5xelte,sweet}_touch.c`, `a5xelte_keys.c`, `sweet_keys.c` (all `standard` in
`bsd/conf/files`) and `SweetFramebuffer.cpp`: with `SB_SWEET_HID_COMPAT=1` (default) StarbiteTouch defines
`sweet_hid_touch()`, `sweet_last_input_abs`, `sweet_last_present_abs` and StarbiteButtons defines `sweet_hid_key()`,
so those files link unchanged and their callers (sweetswipe scripts, the 0413 stall probe) feed the new services
through each driver's command gate. Set it to 0 when those files are removed.

The compile check used here (`kcheck/IOKit/hid/IOHIDEvent.h` forwards to the 1915 copy; the SDK 15.5
`Kernel.framework/Headers` supplies `IOKit/hidevent/IOHIDEventService.h`, whose subclass API matches 2238.120.5's
line for line in the methods used) is only a stand-in for this step.

## 2. Files

| File | How |
|---|---|
| `StarbiteTouch.cpp` | `optional iokitcpp`; include paths: this folder, the IOHIDFamily headers |
| `hw/sb_hsi2c.c`, `hw/sb_s6sy761.c`, `hw/sb_touch_gesture.c`, `hw/sb_touch_script.c` | plain C, `standard` (no IOKit) |
| `StarbiteKernelHooks.h` | `-DSB_HAVE_STARLTE_PMIC=0` once starlte_touch.c is gone (supplies then untouched), `-DSB_HAVE_SWEET_FB=0` once SweetFramebuffer.cpp is gone (dead-reckoning steering, scripts from driver start), `-DSB_HAVE_SBSTOP=0` on a kernel without 0423 |
| `kcheck/` | compile check only: never build it |

`hw/sb_hsi2c.*` is the shared HSI2C master. When StarbiteHSI2C is written it should take these two files over
(move, not copy) and offer `sb_i2c` transfers as a provider service; StarbiteTouch then gets its `sb_i2c` from it.

## 3. Personality (KernelConfigTables.cpp)

```
"   {"
"     'IOClass'         = StarbiteTouch;"
"     'IOProviderClass' = IOService;"
"     'IONameMatch'     = 'starbite,s6sy761';"
"   },"
```

## 4. ADT: a touch node (tools/board/starlte.board + mkadt-board.py)

A new `/arm-io/touch` node (no `reg` of its own, so mkadt's overlap check is unaffected):

| Property | Value (ADT encoding) |
|---|---|
| `compatible` | `"starbite,s6sy761"`, `"samsung,s6sy761"` (strings) |
| `starbite,i2c-bus` | `"hsi2c-touch"` |
| `starbite,i2c-address` | u32 `0x48` |
| `starbite,bus-deps` | strings `"cmu-peric0"`, `"sysreg-peric0"`, `"pinctrl-peric0"` (in this order: clock, USI mode, pins) |
| `starbite,irq-gpio` | `"pinctrl-alive"` |
| `starbite,irq-pin` | u32 `0x40, 0, 1` (GPA1 bank CON offset, pin 0, active low) |
| `starbite,panel` | u32 `1440, 2960` (optional) |
| `interrupts` / `interrupt-parent` | **not yet**: only once StarbiteGPIO is an interrupt controller for the ALIVE EINTs |

**Board fix needed:** `[[block]] cmu-peric0` has `reg = [0x10400000, 0x1000]`, but the USI03 gates are at +0x2018
... +0x20E0 and the Q-channel at +0x3038 (starlte_touch.c maps 0x4000). Make it `0x4000`; the driver refuses a
shorter mapping and then leaves the bus as S-Boot set it (which on our phone means no touch).

Each named block is mapped through its arm-io nub (`mapDeviceMemoryWithIndex(0)`), or, without a nub, from its
`reg` through arm-io's `ranges` (u64 triplets, as mkadt-board.py writes).

## 5. Today's code this replaces / must be quiet

| Today | M8 |
|---|---|
| `bsd/dev/starlte_touch.c` touch thread | keep the file linked (its ACPM / PMIC functions, the governor, sweetscript), boot with **`startouch=0`**: no second owner of the bus. Note: the CPU governor (0411) only starts from that thread, so `startouch=0` also leaves the CPU clocks at boot values (StarbiteTMU/DVFS takes the governor). |
| `sweet_hid_touch()` in SweetHID.cpp | gone with section 1 |
| `sweetswipe=` / `sweetscript=` | `sbtouchscript=` here (`h` steps go to `sbkeyscript=`, StarbiteButtons) |
| `sweettouchmouse/rel/scroll/test` | `sbtouchmode=1/2`, `sbtouchabs=1`, `sbtouchscroll=N`, the bring-up self-test |
| stall probe 0413 (`sweet_last_input_abs`) | not ported (diagnostic); re-add on demand |

## 6. Boot-args

`sbtouch=0`, `sbtouchmode=0|1|2`, `sbtouchabs=1`, `sbtouchscroll=N`, `sbtouchpwr=0`, `sbtouchpoll=MS`,
`sbtouchirq=0`, `sbtouchselftest=0`, `sbtouchscript=...`, `sbtouchscriptframe=1`, `sbstop=hid` + `sbhidwin=S`.
Read with `sweetfb_native`, `sweetfb_scale`, `sweetfb_lw`, `sweetfb_lh` (the shown canvas, as today).
