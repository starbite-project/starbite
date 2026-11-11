# StarbiteUSB: integration into the kernel

For the integrating session. Nothing below has been done; this folder is self-contained.

## 1. Files

Stage 1 (kernel only, enough for PHONE-TEST.md T1):

| File | conf/files line (suggested) |
|---|---|
| `StarbiteUSB.cpp` (+ `StarbiteUSB.h`) | `iokit/Kernel/arm/StarbiteUSB.cpp optional iokitcpp` (as the other built-in Starbite drivers) |
| `hw/sbusb_desc.c`, `sbusb_ncm.c`, `sbusb_ep0.c`, `sbusb_phy.c`, `sbusb_dwc3.c`, `sbusb_func.c`, `sbusb_echo.c` | plain C, built for STARLTE only; keep them next to the .cpp so `#include "hw/..."` resolves (or add the folder to the include path) |
| `hw/*.h` | not exported |

Stage 2 (M11 proper): `StarbiteUSBNCM.cpp` (+ `.h`). It needs **IONetworkingFamily**, which is not in XNU and not
in our kernel today. It is open source (apple-oss-distributions/IONetworkingFamily, APSL), so the route is the one
planned for IOStorageFamily: build it from source into the kernel, or put both in the kernel collection. Until
then leave StarbiteUSBNCM out; StarbiteUSB works without it. If building the family proves heavy, the alternative
is a small `ifnet` client of StarbiteUSB using XNU's own BSD KPI (`ifnet_allocate`); the client interface
(`StarbiteUSBClient`) was kept narrow for that. That would be new work, not in this folder.

Compile-checked (syntax and types, nothing written to the build tree):

```
python3 tools/apple/kcheck.py starbite/apple-drivers/StarbiteUSB/StarbiteUSB.cpp starbite/apple-drivers/StarbiteUSB/hw/*.c
python3 tools/apple/kcheck.py starbite/apple-drivers/StarbiteUSB/StarbiteUSBNCM.cpp \
  -I /Applications/Xcode_16.4.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk/System/Library/Frameworks/Kernel.framework/Headers
```

The SDK path is used only for `IOKit/network/*.h`; it is given last, so every other header still comes from our
tree. kcheck does not link: symbols are not checked.

## 2. Personalities (KernelConfigTables)

From `Info.plist`:

- `StarbiteUSB`: `IOClass StarbiteUSB`, `IONameMatch starbite,dwc3`, `IOProviderClass IOPlatformDevice`.
  **Check the provider class** against what StarbitePE's ADT v2 nubs really are (the design mentions
  `AppleARMIODevice`-style nubs); use whatever the other drivers in this folder match on.
- `StarbiteUSBNCM`: `IOClass StarbiteUSBNCM`, `IOProviderClass StarbiteUSB` (stage 2 only).

The driver finds the PHY by name: a registered service named `usb-phy` (the ADT node name). It waits at most 1 s.

## 3. Board file / ADT changes needed (`tools/board/starlte.board`; I did not edit it)

1. **usb-phy `reg`**: the driver takes every address from the nubs, and the PHY bring-up needs five single
   registers that no node carries today. Append them to usb-phy, in this order (index = `SBUSB_BLK_*` - 1):

   ```
   reg = [0x11100000, 0x200,      # 0 USBCON
          0x1406072c, 0x4,        # 1 PMU: USB 2.0 PHY isolation
          0x14060704, 0x4,        # 2 PMU: USB3/DP PHY isolation
          0x1a242064, 0x4,        # 3 CMU_TOP gate CLKCMU_FSYS0_USB30DRD
          0x1100206c, 0x4,        # 4 CMU_FSYS0 gate USB30DRD_REF_CLK
          0x11002008, 0x4]        # 5 CMU_FSYS0 gate USBDPPHY_REF_SOC_PLL
   ```

   Sources: the loader's `shim/starlte/usb_phy.c`. Two of them overlap other nodes' pages (`pmu` 0x14060000,
   `cmu-fsys1` whose reg is 0x1a240000): if the plan is that StarbitePMGR owns PMU and clock writes, say so and I
   switch these five steps to its interface instead. Without the entries the driver still runs, skips those writes,
   logs the missing mask, and then depends on the loader having brought USB up in this boot.
2. **usb-drd `spi`**: the board says `spi = []`; the DTS says `interrupts = <0 0xf4 0>` under
   `usb@10C00000/dwc3` (SPI 244). Needed only for `sbusb_irq=1`.

## 4. Boot-args

`sbusb=1` (off by default: opt-in until M11 has passed), `sbusb_ring`, `sbusb_irq`, `sbusb_echo`, `sbusb_serial=`.
Details at the top of `StarbiteUSB.cpp`. Suggested once M11 passes: default on, with `sbusb=0` to disable.

## 5. One non-KPI dependency

`StarbiteUSB.cpp` calls `starlte_sbstop_at()` / `starlte_sbstop_now()` (kernel 0423, `bsd/dev/herolte_ufs.c`) for
the `sbstop=usbncm` test stop. They are declared `extern "C"` in the .cpp. For a kernel-collection build, compile
with `-DSTARBITE_USB_SBSTOP=0`. Everything else is public KPI (IOKit, libkern, `PE_parse_boot_argn`,
`sys/kpi_mbuf.h`, IONetworkingFamily).

## 6. Coexistence with the loader: the hand-off state

What the driver must expect when XNU starts, and what it does about it.

Verified from the loader's code (`shim/starlte/fastboot_dev.c`, `usb_dwc3.c`, `xnu_boot.c`; `docs/USB-FASTBOOT.md`
section 11):

- Before any OS starts, `star_prepare_xnu` calls `fbbg_off()` on every return path, and every catalog start calls
  `fbbg_stop()` first. Both end in `usbd_stop()` when the loader's USB was up: transfers ended, DCTL RUN_STOP
  cleared (soft disconnect: the Mac sees the loader's fastboot device go away), events drained, DEVTEN 0,
  DALEPENA 0, a core soft reset (DCTL CSFTRST), and the event buffer registers zeroed with its interrupt masked.
  So the **DWC3 is halted, reset, and has no event buffer**; it does no DMA into 0x91000000 any more.
- The **PHY is not undone** ("Nothing here is undone before an OS starts"). If the loader's USB was up in this boot,
  the three clock gates are open, both PMU isolation bits are set, USBCON is configured for device mode with VBUS
  forced valid, and the MAX77705 D+/D- switch is on the AP.
- If the loader's USB was **never up** in this boot (it starts on the first `fbbg_poll`, that is from a menu, the
  crash report or Starbite Recovery, or in fastboot mode), none of that was done: the PHY, gates, PMU bits and the
  switch are as S-Boot left them, which the loader's own notes call unknown.

What the driver does, in every case:

1. If DCTL RUN_STOP is set (not expected after the loader; possible straight from S-Boot), it halts the core and
   drains its events before touching the PHY.
2. It runs the whole PHY sequence again from the gates up (it is idempotent; the loader also re-runs it on every
   link start).
3. It soft-resets the core again and programs everything from zero. It assumes nothing about the registers.
4. It uses its own DMA block. It never touches 0x91000000 or anything else of the loader's.

What it cannot do (inferred risk, NOTES.md open question 1): close the MAX77705 switch. On a boot where the loader
never brought USB up, D+/D- may not reach the connector. The log line `no host yet: line SE0` shows that case.

On the way back: the driver's `stop()` leaves the controller as the loader's `usbd_stop()` does (halted, reset,
no event buffer), so a later loader fastboot start finds what it expects. A reset or panic does not run `stop()`;
the loader's own start then meets a running core, which its `usbd_start()` soft-resets first (verified in its
code), and S-Boot runs in between anyway.

Two devices never exist at once: the loader's fastboot (18d1:4ee0, bcdDevice 0x0100) is gone before the kernel
starts, and this driver's NCM device (same VID:PID, bcdDevice 0x0200) appears only after its own pull-up. Host
tools that look for the loader must match the fastboot interface (class 0xff/0x42/0x03) or bcdDevice, not the
VID:PID alone; `tools/installer/sbinst/` mentions 4ee0 and should be checked.

## 7. Not in this folder (phase 2 and neighbours)

- Phase 2: `IOUSBDeviceController` for Apple's own device stack (USBDeviceMux / NCM).
- MAX77705 CC / VBUS / D+/D- switch (the battery / MUIC driver's chip): replug detection and routing.
- SuperSpeed (the combo PHY is held idle).
