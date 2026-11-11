# IONetworkingFamily: building it into the kernel (for the integrating session)

Apple's IONetworkingFamily-188 (the version in macOS 26.5, the release that ships xnu-12377.121.6) compiled into
our xnu-12377 STARLTE kernel, so StarbiteUSB's stage 2, `StarbiteUSBNCM : IOEthernetController`
(`starbite/apple-drivers/StarbiteUSB/INTEGRATION.md` 1), can be built in for M11. Nothing here has been applied to
`build/xnu-12377` or `build/dxb-12377`.

Status: **12 of 12** sources compile-check clean (warnings only); **no patches**. Link-level check (objects with
the kernel's flags, `nm` against `kernel.development.starlte.unstripped`): **no undefined symbol, no duplicate**.
`StarbiteUSBNCM.cpp` also compile-checks against these headers (instead of the SDK's).

## 1. Stage

```
sh starbite/apple-families/stage.sh IONetworkingFamily
sh starbite/apple-families/check.sh IONetworkingFamily     # "IONetworkingFamily: 12 of 12 OK"
```

Leaves the clone in `build/families/IONetworkingFamily` (tag `IONetworkingFamily-188`) and the include tree
`build/families/_inc/IONetworkingFamily/` (`IOKit/network/*.h` = the family's headers, `TargetConditionals.h`).

## 2. Kernel tree layout (suggested)

```
D=build/xnu-12377/iokit/Families/IONetworkingFamily
mkdir -p $D/include
cp build/families/IONetworkingFamily/*.cpp build/families/IONetworkingFamily/*.h $D/
cp -RL build/families/_inc/IONetworkingFamily/. $D/include/
cp starbite/apple-families/kext-view.h build/xnu-12377/iokit/Families/kext-view.h   # if not there yet
```

## 3. conf/files and flags

```
iokit/Families/IONetworkingFamily/IONetworkController.cpp   optional iokitcpp
iokit/Families/IONetworkingFamily/IOEthernetController.cpp  optional iokitcpp
iokit/Families/IONetworkingFamily/IONetworkInterface.cpp    optional iokitcpp
iokit/Families/IONetworkingFamily/IOEthernetInterface.cpp   optional iokitcpp
iokit/Families/IONetworkingFamily/IONetworkStack.cpp        optional iokitcpp
iokit/Families/IONetworkingFamily/IONetworkData.cpp         optional iokitcpp
iokit/Families/IONetworkingFamily/IONetworkMedium.cpp       optional iokitcpp
iokit/Families/IONetworkingFamily/IONetworkUserClient.cpp   optional iokitcpp
iokit/Families/IONetworkingFamily/IOOutputQueue.cpp         optional iokitcpp
iokit/Families/IONetworkingFamily/IOPacketQueue.cpp         optional iokitcpp
iokit/Families/IONetworkingFamily/IOMbufMemoryCursor.cpp    optional iokitcpp
iokit/Families/IONetworkingFamily/IOKernelDebugger.cpp      optional iokitcpp
```

`iokit/conf/Makefile.template` (inside the board filter; same mechanism as `bsd/sweet_hfs`):

```make
IONF_ROOT = $(SRCROOT)/iokit/Families/IONetworkingFamily
IONF_OBJS = IONetworkController.o IOEthernetController.o IONetworkInterface.o IOEthernetInterface.o \
	IONetworkStack.o IONetworkData.o IONetworkMedium.o IONetworkUserClient.o IOOutputQueue.o \
	IOPacketQueue.o IOMbufMemoryCursor.o IOKernelDebugger.o
$(foreach o,$(IONF_OBJS),$(eval $(o)_CFLAGS_ADD += -include $(SRCROOT)/iokit/Families/kext-view.h -D__PRIVATE_SPI__))
$(foreach o,$(IONF_OBJS),$(eval $(o)_INCFLAGS += -I$(IONF_ROOT) -I$(IONF_ROOT)/include -I$(SRCROOT)/bsd))
```

Both flags are required:

- **`kext-view.h`** (un-defines `XNU_KERNEL_PRIVATE`, `BSD_KERNEL_PRIVATE`, `MACH_KERNEL_PRIVATE`,
  `IOKIT_KERNEL_PRIVATE`, `IOKITSTATS`). With the kernel's private view the family fails in `<sys/mbuf.h>`
  (private `m_add_hdr_crumb` overloads), `<net/dlil.h>`, `IOSkywalkSupport.h` and `kern/startup.h` (C++ templates
  under `extern "C"`): only 4 of 12 compiled on the first attempt. With the kext view, 12 of 12.
- **`-D__PRIVATE_SPI__`**: Apple's only preprocessor definition for the kext target
  (`IONetworkingFamily.xcodeproj`). It selects the private half of `IONetworkInterface.h` (pull-model output,
  packet polling, `setInterfaceSubType` ...). The public half declares the same vtable slots as
  `OSMetaClassDeclareReservedUnused(IONetworkInterface, 5..10)`, so StarbiteUSBNCM may stay without it.
- **`-I$(SRCROOT)/bsd`**, searched after the normal include path: `<net/dlil.h>` includes `<net/classq/classq.h>`,
  which XNU installs only into Kernel.framework's PrivateHeaders (`INSTALL_KF_MI_LCL_LIST`), not into its own
  `EXPORT_HDRS`. Only that header is taken from there; everything else still comes from `EXPORT_HDRS`.

## 4. Personalities (KernelConfigTables.cpp)

`personalities-osx.kct.inc` (identical for iOS), generated from the family's Info.plist by `plist2kct.py`:

| Personality | Matches | Note |
|---|---|---|
| IONetworkStack | IOResources, `IOResourceMatch = IOBSD` | Needed: names interfaces and attaches them to BSD. `IOKitBSDInit.cpp` (`IORegisterNetworkInterface`, network root) already waits for a service of this class |
| Kernel Debugging Driver (`IOKDP`) | `IOKernelDebugger` | KDP over Ethernet. An `IOKernelDebugger` nub exists only if a controller calls `attachDebuggerClient()` with polled send/receive; StarbiteUSBNCM does not, so this never matches. Safe to leave out |

Plus StarbiteUSBNCM's own personality (`IOProviderClass = StarbiteUSB`, StarbiteUSB INTEGRATION.md 2).

## 5. What XNU already provides / how it fits

- The family is a client of XNU's ifnet KPI (`ifnet_allocate_extended`, `ifnet_attach`, `mbuf_*`, BPF, dlil);
  every symbol it references is in our kernel (checked). Our kernel has `SKYWALK` on: legacy-ifnet drivers like this
  family are still supported there (Apple ships IONetworkingFamily on Skywalk kernels).
- Interface naming: as on a Mac, `en0`... is assigned when user space (configd's InterfaceNamer) asks
  IONetworkStack, or by the kernel for a network root. Until configd runs, StarbiteUSBNCM's interface exists in the
  registry but has no BSD name. Relevant to M11: the ping test needs configd up, or a kernel-side naming call
  (IONetworkStack's user client is what configd uses; not done in this folder).
- No class of the family exists in XNU or our tree today (StarbiteUSB's INTEGRATION.md: "IONetworkingFamily is not
  in XNU and not in our kernel"); no object-name collisions.

## 6. Symbol sets / exports

Nothing needed while built in. For a later Boot KC with Apple's closed network kexts: a pseudo-kext
`com.apple.iokit.IONetworkingFamily` like `config/System.kext/PlugIns/IONVRAMFamily.kext` (Info.plist:
`CFBundleVersion 3.4`, `OSBundleCompatibleVersion 1.0.0b1`, `OSKernelResource true`) with the clone's
`config/IONetworkingFamily.exports` (815 symbols) added to the symbol-set build in `config/Makefile`.

## 7. iOS target

Same files and personalities. Not compile-checked for iOS (no iOS STARLTE flags).
