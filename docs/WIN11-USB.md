# USB host on Windows 11 (track B, milestone W3)

Started 2026-11-07. Goal: USB keyboards, mice, sticks and hubs work in Windows 11 on the S9
(23H2, 22631.7584, our UEFI = our SEC/PEI + the woa release DXE volume + our added modules).
Today Windows has no USB at all (`docs/STARBITE-MASTER-PLAN.md` 3.6).

**Status (2026-11-07):** **T1 passed on the phone** (v3, image f4432dde…): the DWC3 runs as a
USB host under Windows' inbox xHCI driver ("USB xHCI Compliant Host Controller" + "USB Root Hub
(USB 3.0)", no problem codes), touch and restart unchanged. Tests with devices plugged in (T2-T5)
need the user, an OTG adapter and devices. Section 9.1 has the measured values.

## 1. The idea in one paragraph

Windows ships a generic xHCI driver (`usbxhci.sys`) that binds to any ACPI device with the ID
`PNP0D10`. The S9's USB controller (Synopsys DWC3 at 0x10C00000) contains a standard xHCI host
once its mode bit says "host". So a firmware module can do everything Windows doesn't know about
(clocks, the Samsung USB PHY, the DWC3 host role, D+/D- routing and VBUS on the MAX77705) and
then describe the controller in an extra ACPI table. Windows' own USB stack (xHCI, hubs, HID,
mass storage) does the rest. No Windows driver of ours is needed for the first step. A small
KMDF driver comes later only for what firmware can't do: reacting to plug/unplug at runtime
(section 7, stage H3) and switching between host and device role (stage H4).

Precedents for "firmware sets up the SoC, ACPI says `PNP0D10`, Windows' inbox driver runs it":
the Raspberry Pi edk2 port (`Platform/RaspberryPi/AcpiTables/Xhci.asl`, `_HID "PNP0D10"`) and
Snapdragon Windows laptops, whose USB controllers are DWC3 cores run by the inbox `usbxhci.sys`
under a role-switch parent. Microsoft's ACPI guidance for SoC USB: the xHCI controller carries
`PNP0D10` as its compatible ID.

## 2. Pieces

| Piece | Where | What | Runs |
|---|---|---|---|
| **s9usb** (shared) | `starbite/s9usb/` | Portable C, no OS: USB clocks + PMU isolation, the USB 2.0 PHY (device or host column), the DWC3 core init common to both roles, the MAX77705 bus (HSI2C 0x14360000) with the Type-C MCU's opcodes. Register access through a small platform table (`s9usb.h`) so UEFI, a KMDF driver and libsb can all use it | everywhere |
| **Star2LteUsbDxe** | `tools/woa/modules/Star2LteUsbDxe/` | UEFI DXE module. Maps the register blocks the firmware leaves unmapped, reads the Type-C state, picks the boot role (host/device/auto), runs s9usb, checks the xHCI in host role, routes D+/D- to the AP, sets the VBUS policy, logs `=USB` lines, and **enables** the one gated USB blob of its layout once every check passed (section 4.1) | once per Windows boot |
| **Star2LteSsdtDxe** (shared with the GPU track) | `tools/woa/modules/Star2LteSsdtDxe/` | Installs every Starbite SSDT blob in the firmware volume during DXE dispatch (section 4.1), logs `=SSDT` lines | once per Windows boot |
| **USB blobs** (gated) | `tools/woa/acpi/UsbXhc.asl` (`SBUSBXHC`), `UsbUrs.asl` (`SBUSBURS`), `UsbUfn.asl` (`SBUSBUFN`) | Layout A: `\_SB.USB0` `PNP0D10` (Windows' xHCI driver, no INF of ours). Layout B: `\_SB.URS0` `PNP0CA1` with host and function children. Layout C: `\_SB.UFN0` `INT3445` (function only) (section 5) | read by Windows |
| **Report script** | `tools/woa/winpe/usb-report.ps1` (+ `usb-report.cmd` for the Startup folder) | At logon: USB devices, driver bindings, the controller's resources, problem codes and ACPI path, every ACPI device, written to `C:\Starbite\usb\` | Windows |
| Next: **Star2LteUsbc** (KMDF) | stage H3 | MAX77705 at runtime over its own HSI2C mapping: D+/D- re-routing after each attach, VBUS when the MCU won't, the PHY's role column, a UCM connector that tells URS the role | Windows |

Both modules and a copy of `starbite/s9usb` go into the build by patch 0024 (generated, under
`!ifdef`); `dxe-replace.py --add` puts the modules and the blobs into the release DXE volume.

What s9usb replaces: `starbite/utility/usb_phy.c` + the core part of `usb_dwc3.c` +
`max77705_muic.c` (all ours, GPL-2.0-only, proven on this phone in device mode) are the source
of the shared code. libsb and the loader keep their own copies until the main session decides to
switch; nothing outside this track's files was changed.

## 3. Register level

Bases (Samsung `exynos9810.dtsi`, our Linux port's DTS, woa `Exynos9810.h`):

| Block | Base | Used for |
|---|---|---|
| DWC3 (USB30DRD) | 0x10C00000, 64 KiB | xHCI at +0x0000..0x7FFF, DWC3 globals at +0xC100 |
| USBCON (USB 2.0 PHY + link glue) | 0x11100000 | PHY |
| CMU_TOP | 0x1A240000 | gate `CLKCMU_FSYS0_USB30DRD` +0x2064; mux +0x1060, div +0x1858 (read only) |
| CMU_FSYS0 | 0x11000000 | gates `USB30DRD_REF_CLK` +0x206C, `USBDPPHY_REF_SOC_PLL` +0x2008; Q-channels +0x3000.. (read only) |
| PMU | 0x14060000 | USB2 PHY isolation +0x72C, USB3/DP PHY +0x704 (bit 0 = on) |
| HSI2C (USI_CMGP03) | 0x14360000 | MAX77705: Type-C MCU 0x25, charger 0x69, fuel gauge 0x36 |
| CMU_CMGP / SYSREG_CMGP / GPIO_CMGP | 0x14200000 / 0x14210000 / 0x14220000 | the HSI2C's gates, USI mode, GPM14/15 pins |
| Interrupt | GIC SPI 244 (`INTREQ__USB30DRD_Interrupt_00`) = GSIV 276, level high | xHCI |

**Never touched:** MIF/INT clocks, muxes and dividers (only read), the USB3/DP combo PHY's PMA
beyond keeping it powered down, any storage, the charger's boost registers (section 3.4).

### 3.1 Clocks and PMU (both roles)

Exactly what the loader does (`starbite/utility/usb_phy.c`, proven): set MANUAL | CG_VAL
(bits 20, 21) on the three gates above, set bit 0 of both PMU registers. Muxes, dividers and
Q-channels are snapshotted for the log, never written.

### 3.2 USB 2.0 PHY: vendor CAL enable, then the role's column

The enable sequence is Samsung's CAL `phy_exynos_usb_v3p1_enable()` for `EXYNOS_USBCON_VER_03_0_0`
as transcribed in our Linux driver (`tools/linux/port/src/usb/phy-exynos9810-usbdrd.c`) and the
loader (`starbite/utility/usb_phy.c`): Q-channel forced active, link reset pulse, PHY reset
high, SIDDQ off, UTMI suspend/sleep/pull-downs off, `HSP_EN_UTMISUSPEND | HSP_COMMONONN`,
100 us, PHY reset low, bus filter bypass, OCD select, ReWA parked, PIPE3 forced idle with the
combo PMA in low power. woa's `UsbDebugHid.c` `UsbEnablePhy` (BSD-2-Clause-Patent) does the same
steps. Then the role:

| USBCON register | Device (loader, Linux daily) | **Host** (CAL `config_host_mode`, Linux rescue) |
|---|---|---|
| UTMI 0x50 | FORCE_VBUSVALID[5] = FORCE_BVALID[4] = 1, DP/DM_PULLDOWN[3:2] = 0 | FORCE_* = 0, **DP_PULLDOWN = DM_PULLDOWN = 1** (15 kΩ host pull-downs) |
| HSP 0x54 | VBUSVLDEXTSEL[13] = VBUSVLDEXT[12] = 1 | both 0 |
| HSP_TUNE 0x58 | tx_vref 0xD, tx_rise 1, tx_res 3, tx_pre_emp 3, rx_sqrx 4, compdis 3 | tx_vref **0x7**, tx_rise 1, tx_res **1**, tx_pre_emp 3, rx_sqrx **6**, compdis **7** |

HSP_TUNE fields: tx_vref [31:28], tx_rise [25:24], tx_res [22:21], tx_pre_emp [19:18],
rx_sqrx [10:8], compdis [2:0]. Values: stock `exynos9810-starlte_common.dtsi` `usb_hs_tune`,
`tune_value = <device host>` (`ref/starlte-kernel`). `s9usb_phy_set_role()` switches only the
three rows, as Linux's `phy_set_mode` does, so a later role switch needs no full re-init.

SuperSpeed stays off: the PIPE3 interface is forced idle and the combo PHY's PMA stays in low
power, exactly as in device mode. The xHCI still reports a USB 3 port; nothing can connect to it.
SuperSpeed (the S9 port is USB 3.1 Gen 1) is stage H5.

### 3.3 DWC3 core in host role

Order and values follow our proven device-mode init (`usb_dwc3.c usbd_start`, itself Linux
`dwc3_core_init` + our Linux DT quirks), with only the role bits changed:

| Step | Register | Value | Source |
|---|---|---|---|
| 1 | GSNPSID 0xC120 | must read 0x5533xxxx (DWC_usb3) or 0x3331xxxx (DWC_usb31); logged | Linux core.c |
| 2 | DCTL 0xC704 CSFTRST[30] | set, wait clear (≤ 200 ms; needs the PHY clock). If GCTL already says host (a warm start after a host role): USBCMD R/S = 0, wait HCHalted ≤ 20 ms, then GCTL CORESOFTRESET[11] with GUSB2PHYCFG/GUSB3PIPECTL PHYSOFTRST[31] pulsed | Linux `dwc3_core_soft_reset`, loader; the host case as Samsung's and woa's whole-core reset |
| 3 | GUSB2PHYCFG 0xC200 | clear SUSPHY[6], ENBLSLPM[8], U2_FREECLK_EXISTS[30], PHYSOFTRST[31]; PHYIF/USBTRDTIM left at reset | DT quirks `dis_u2_susphy`, `dis-u2-freeclk-exists` |
| 4 | GUSB3PIPECTL 0xC2C0 | clear SUSPHY[17] | DT quirk `dis_u3_susphy` |
| 5 | GCTL 0xC110 | PRTCAPDIR[13:12] = **1 (host)**, SCALEDOWN[5:4] = 0, SOFITPSYNC[10] = 1, DSBLCLKGTNG[0] = 1 only for 2.10a..2.50a | the loader and woa (SOFITPSYNC: SOF/ITP from ref_clk while the PHY is suspended; Linux sets it only for 2.10a..2.50a, and with PHY suspend off it shouldn't come into play); DSBLCLKGTNG: Linux `dwc3_core_setup_global_control` |
| 6 | GSBUSCFG0 0xC100 | INCRBRSTEN[0], INCR16BRSTEN[3]; REQINFO = 2 in each nibble 16..31 from 2.20a | Samsung `dwc3_core_config` (as woa, an earlier port, loader) |
| 7 | GSBUSCFG1 0xC104 | BREQLIMIT[11:8] = 3 | same |
| 8 | GUCTL 0xC12C | REFCLKPER[31:22] = 0x14, **USBHSTINAUTORETRYEN[14] = 1** (host: retry IN on CRC/overrun) | same |
| 9 | GFLADJ 0xC630 (from 2.50a) | 30MHZ_SDBND_SEL[7] = 1, 30MHZ[5:0] = 0x20 | DT `quirk-frame-length-adjustment = <0x20>` |
| 10 | GUCTL1 0xC11C (DWC_usb3 from 2.90a) | DEV_FORCE_20_CLK_FOR_30_CLK[26] = 1 (no USB3 PIPE clock; maximum-speed high-speed). New compared with the loader, as our Linux daily kernel does it | Linux core.c |

No event buffers, no DCFG, no device registers: the xHCI owns the controller in host role.

**xHCI checks before the ACPI table is armed** (all read-only): CAPLENGTH ≥ 0x20,
HCIVERSION 0x0100, 0x0110 or 0x0120, HCSPARAMS1 MaxPorts 1..4, USBSTS (op + 0x04) HCHalted[0] = 1 and
CNR[11] = 0, HCCPARAMS1 AC64 logged (Windows' memory reaches 0x8_FFFF_FFFF), and the Supported
Protocol capabilities walked (the USB 2 and USB 3 port numbers are logged, for the stage-H2 port
descriptions). Any failure: no SSDT, and Windows boots exactly as today.

The controller is left halted. Windows does its own HCRST.

### 3.4 MAX77705: D+/D- and VBUS

Bus and opcode protocol from `starbite/utility/max77705_muic.c` (proven in the loader and the
Utility OS): CMGP gates and USI I2C mode, S-Boot's timing kept (or 400 kHz), opcode bytes into
AP_DATAOUT0 (0x21..), the end marker at AP_DATAOUT32 (0x41), the answer in AP_DATAIN0 (0x51..).

| Step | What | Why |
|---|---|---|
| read | HW_REV 0x00, FW_REV 0x01, USBC_STATUS1 0x06, BC_STATUS 0x08 (VBUSDet[7]), CC_STATUS0 0x0A (CCStat[2:0]: 0 none, 1 sink, 2 source, 3 audio, 4 debug, 5 error) | logged |
| D+/D- | opcode 0x06 CONTROL1_WRITE, data 0x09 COM_USB, read back with 0x05 | the switches are open after any MCU reset (TWRP's shutdown resets it) |
| VBUS | opcode **0x54** with 0x00 (auto-VBUS off) or 0x02 (on); answer logged | see below |

**VBUS policy** (build-time, `STARBITE_USB_VBUS`; only ever offered in host role):

| Value | Name | Effect | Used for |
|---|---|---|---|
| 0 | `off` | auto-VBUS off: the phone never powers the port | first test with a self-powered hub, which supplies its own devices |
| 1 | `mcu` | auto-VBUS on: the Type-C MCU itself turns VBUS on when it attaches as source (an OTG adapter's Rd) and off when it detaches or sees a charger | second test; the default once it works |

Why the MCU and not our own boost: Windows has no code of ours running when a cable comes or
goes, so firmware can't follow plug and unplug. The MCU already does the Type-C decisions in
hardware. Samsung's driver has both calls (LineageOS `android_kernel_samsung_universal9810`,
lineage-17.1, GPL-2.0): `max77705_usbc_enable_auto_vbus()` sends opcode 0x54 with **0x02**,
`max77705_usbc_disable_auto_vbus()` 0x54 with 0x00 (`drivers/ccic/max77705_usbc.c`), and
`max77705_vbus_turn_on_ctrl()` (`max77705_pd.c`) does nothing in auto mode ("don't turn on because
of auto-mode"; "don't turn off because of auto-mode"): with auto-VBUS on, the MCU switches VBUS
itself. The stock kernel calls only the disable at probe (the enable is commented out there), so
auto mode is a vendor path that exists but that phones don't ship with [unverified on hardware].
0x54 is an option command: the factory debug path sends 0x03 to add water detection, 0x02 to
remove it, so bit 1 is auto-VBUS. **Defensive rule in the module:** with policy 1, auto-VBUS is
still switched off for that boot if VBUSDet is already set (a charger, a computer, a back-powering
hub) or CCStat says sink at boot. The setting stays in the MCU until its next reset (TWRP's
shutdown) or the next OS that sets it (Samsung kernels and our Linux turn it off at probe; the
loader, the Utility OS, iOS and macOS don't touch it): after a vbus1 test, boot TWRP or a vbus0
image before plugging in a charger. The AP-driven boost of `docs/RESCUE-USB-HOST.md` 3.2 (charger 0x69,
CNFG_00 MODE 0xE) is **not** used from firmware: a boost left on with no driver to switch it off
fights a charger plugged in later. If the MCU route fails, the boost moves into the stage-H3
driver, which can poll and switch it off.

## 4. ACPI

Nothing here edits the DSDT: every Starbite device comes in its own SSDT blob, installed by
Star2LteSsdtDxe. The USB blobs:

```
SBUSBXHC (layout A)                         SBUSBURS (layout B)
Scope (\_SB) Device (USB0)                  Scope (\_SB) Device (URS0)
  _HID "PNP0D10", _UID 0, _CCA 0               _HID "PNP0CA1", _UID 0, _CCA 0
  _CRS Memory32Fixed 0x10C00000 0x10000        _CRS Memory32Fixed 0x10C00000 0x10000
       Interrupt Level High Excl 276           Device (USB0) _ADR 0   _CRS Interrupt ... 276   host
                                               Device (UFN0) _ADR 1   _CRS Interrupt ... 276   function
```

`_CCA 0`: not cache-coherent (Linux has no dma-coherent for this controller). No `_S0W`/`_PRW`:
no wake, so Windows keeps the controller in D0 while devices are attached. No root-hub port
objects (`RHUB`, `_UPC`, `_PLD`) yet: the DXE logs the xHCI's port numbering, and stage H2 adds
them (the USB 3 port marked not connectable).

### 4.1 Star2LteSsdtDxe: the blob interface (shared by every track)

**A blob** is one `.aml` (an SSDT exactly as iasl writes it), packed by
`tools/woa/ssdt-ffs.py X.aml X.ffs` into one FFS file and added to the release DXE volume with
`tools/woa/dxe-replace.py ... --add X.ffs`. Adding a device = adding its blob; a BOOT image
without the blob is the kill switch.

| Rule | Detail |
|---|---|
| Table | `DefinitionBlock ("", "SSDT", 2, "STRBTE", "<Table ID>", rev)`. OEM ID **STRBTE**; the **OEM Table ID is the blob's name** and must be unique (taken: `SBUSBXHC`, `SBUSBURS`, `SBGPU`). Self-contained: only `External`s of what the release DSDT has (`\_SB`) |
| FFS file | type FREEFORM (0x02); name GUID **`53534454-5342-5445-<the 8 Table ID bytes>`** for a plain blob, **`53534454-5342-4754-<...>`** for a gated one (`ssdt-ffs.py [--gated] --guid X.aml` prints it); a RAW section with the table, a UI section `SSDT <id>` / `Gated SSDT <id>` |
| Installed | **during DXE dispatch**, through `EFI_ACPI_TABLE_PROTOCOL.InstallAcpiTable`, checksum recomputed; at most 32 blobs. Plain blobs at the installer's entry; gated blobs at its entry if already enabled, else from a protocol notification the moment their driver enables them (dispatch order doesn't matter) |
| Skipped | not "SSDT"; Length < 36 or larger than the section; OEM ID not STRBTE; Table ID ≠ the GUID's last 8 bytes; a Table ID already installed |
| Plain blob (`ssdt-ffs.py X.aml`) | installed unless **vetoed**: a driver that finds its hardware unusable installs a NULL protocol whose GUID is the blob's file GUID (`gBS->InstallProtocolInterface (&h, &BlobGuid, EFI_NATIVE_INTERFACE, NULL)`); a veto that comes after the install uninstalls the table again. A plain blob without a driver is always installed (fine for memory-only devices such as the GPU's) |
| Gated blob (`ssdt-ffs.py --gated X.aml`) | installed only when **enabled**: its driver installs that NULL protocol after checking the hardware. No driver, or a failed check: not installed. Fails closed: use it for anything whose clocks or power a driver must turn on first |
| Log | one line per blob (`=SSDT installed <id> (<len> bytes, rev <n>[, gated], <when>)`, `=SSDT skip <id>: <reason>`, `=SSDT dispatch done: ...`) through `StarbiteLog.h` (section 4.2) |

**Why during dispatch (v3; v2 used ReadyToBoot at TPL_CALLBACK - 1).** The first phone run (T1,
2026-11-07, `win11-usbh0` with v2) booted Windows, but Windows' Enum had neither the USB controller
nor the GPU track's plain `SBGPU` device: nothing installed at ReadyToBoot reached Windows. The
same holds for the release's own GPIO module, whose ReadyToBoot SSDTs (`RWOA0761`, `RWOA0003`, ...)
appear in no hive we have, while the platform module's dispatch-time SSDT `RAMRET01` (`RWOA03E0`)
is in every one (but the GPIO module's absence is explained by its own failing check, below, so it
says nothing about timing). Why v2's tables were lost is not proven, since v2's log was
overwritten. The likely reason: the release modules "P" and the PSCI EL2 shim reference ReadyToBoot
and the ACPI 2.0 configuration table but not the table protocol, so they work on the published
tables at ReadyToBoot and tables added after them may be lost; ReadyToBoot not being signalled as
expected would fit too. Dispatch is the path proven to reach Windows either way. The GPIO module's
exactly-4-tables check that v2 waited for fails on every boot anyway (the platform installs FACP, GTDT, APIC, PPTT and RAMRET01:
5), so installing during dispatch takes nothing away; touch comes from the DSDT's `TSP0`
(`SCSY0761`). Open risk: if P or the shim insisted on an exact table count, the extra tables would
upset them; RAMRET01 shows they live with at least 5. T1 again checks the CPU count (4) and that
Windows starts.

**USB's blobs are gated** (Star2LteUsbDxe enables one, after every check): layout A →
`SBUSBXHC` in host role; layout B → `SBUSBURS`; layout C → `SBUSBUFN` in device role. Any failure,
no USB module in the image, or a role the layout doesn't cover: nothing is enabled, and Windows
sees no USB controller. Each image also carries only its layout's blob.

### 4.2 Logs that survive a Windows session

During Windows, Windows' own UFS driver fills the pmsg ring (`=UFSWRIT` lines), so the modules'
lines there are gone by the next TWRP visit (T1: 63 `=UFSWRIT` lines, nothing else). `StarbiteLog.h`
therefore writes every line to three places: the pmsg ring (as before), an **evidence ring in
ramoops zone 0** (0xFED10000, the same "DBGC" header format; Windows doesn't write it), and DEBUG.
The evidence ring: data capped at 0x2FF4 bytes so it ends at 0xFED13000, below the woa
firmware's diagnostic block (the release UFS module writes from 0xFED13000, the release BDS at
0xFED136D0/13800/13B00/13F00; RWD1 0xFED13D80, P3 0xFED13E80); never wraps (the first line that
doesn't fit becomes `=SB log full` and the ring stops for that boot); reset by the first line of
each boot (a per-boot protocol flag). Today's log is about 3 KB. The page tables map 0xFED10000..0xFED20000 (PlatformMemoryMapLib's
ramoops window).

**Reading it:** after Windows, restart into the **loader menu** (not TWRP first) and run
`fastboot oem log dmesg` then `fastboot get_staged <file>`: the loader's `fbp_log` shows the
zone-0 ring as text. TWRP's 4.9 ramoops discards a zone-0 record without a Linux kmsg header, so
reading in TWRP first loses it. The loader's crash check only counts zone 0 as Linux evidence when
it starts with `====` after a Linux launch; these lines start with `=SB`/`=USB`/`=SSDT`.

## 5. Role: one controller, host and device (with the Starbite Link, L1)

The DWC3 and its PHY serve both this track (host) and the main track's device mode (L1). Facts
from the phone's own files (2026-11-07: `usbxhci.inf`, `urssynopsys.inf`, `ufxsynopsys.inf`,
`genericusbfn.inf`, `urscx01000.sys` 10.0.22621.2506, `ufxsynopsys.sys`, `ufx01000.sys`, copied
read-only to `build/woa/usb-inf/` of the main checkout; the INFs are UTF-16), our image's SYSTEM
hive (`DriverDatabase\DeviceIds`) and Microsoft's "USB dual role driver stack architecture" and
"Bring up the dual-role controller for a USB Type-C Windows system":

| ID | Inbox match (INF section) |
|---|---|
| `ACPI\PNP0D10` | usbxhci.inf `Generic.Install` (hardware ID; adds the DebuggerSafe property) |
| `ACPI\PNP0D15`, `PCI\CC_0C0330` | usbxhci.inf `Generic_NoDebug.Install` / `Generic.Install` |
| `ACPI\PNP0CA1` | urssynopsys.inf `UrsSynopsys.Install` (service UrsSynopsys, depends on urscx01000) |
| `URS\PNP0CA1&FUNCTION` | ufxsynopsys.inf `ufxsynopsys.Install`, as a **compatible** ID (then GenericUsbFn on top) |
| `URS\PNP0CA1&HOST` | no INF lists it, **but** urscx01000.sys carries the compatible IDs `PCI\CC_0C0330` + `ACPI\PNP0D15` (and `PCI\CC_0C0320` + `ACPI\PNP0D20` for EHCI) next to its `&HOST`/`&FUNCTION` names: the "XHCI compatible ID for the host device" Microsoft's page calls work in progress is in this build. Both match usbxhci.inf, so **the host child binds inbox; no INF of ours is needed** [to confirm on the phone: the host child's driver in Device Manager] |
| `ACPI\INT3445` | ufxsynopsys.inf `ufxsynopsys.Install`: the **same generic section** as the URS function child and Intel's PCI IDs; only Qualcomm's IDs get their own section (a lower filter). Nothing Intel-specific at INF level |

The URS driver names its children from the parent's hardware ID (`URS\<HID>&HOST`,
`URS\<HID>&FUNCTION`), finds their ACPI nodes by `_ADR` (0 = host, 1 = function), and loads only
one stack at a time. On a Type-C port the documented way to give it the role is a UCM connector
driver (UcmCx): "That driver determines the role based on the CC pins and reports the results to
the USB connector manager (UCM), which then sends the results to the role-switch drivers."
urscx01000.sys also reads `RoleSwitchMode`, `OsDefaultRoleSwitchMode` (under
`HKLM\SYSTEM\CurrentControlSet\Control\USB`) and `DisableVBusManagement`; none is documented,
so nothing here depends on them.

**Design (agreed with the main session 2026-11-07):**

1. **Firmware** (Star2LteUsbDxe, built): brings up PHY + DWC3 in the boot role.
   `STARBITE_USB_ROLE` 0 host, 1 device, 2 **auto** = device when the MCU sees the phone as a sink
   with VBUS present (the Mac's cable at boot), otherwise host. `STARBITE_USB_ACPI` 0 = layout A
   (host only), 1 = layout B (URS, both roles), 2 = layout C (device only).
2. **No URS host INF**: the host child's compatible IDs reach usbxhci.inf inbox (table above).
3. **Runtime role** (stage H3, KMDF `Star2LteUsbc`): a UCM connector client on the MAX77705
   (CCStat → UcmConnectorTypeCAttach as UFP/DFP, so URS switches stacks), plus what no inbox
   driver does: the PHY's host/device column (pull-downs, forced VBUS-valid, tuning), D+/D-
   re-routing after each attach, and VBUS. L1 needs the D+/D- part in device mode too.
4. **Until 3 exists, URS can't be told the role.** With no ID pin and no UCM connector, urssynopsys
   picks its own initial role and programs the DWC3's role register itself; that need not be
   the firmware's boot role, and the PHY column (only our code sets it) then mismatches: forced
   VBUS-valid and no pull-downs under the host stack, or pull-downs under the function stack.
   So `usb-urs-auto` is for an experiment (what role does URS choose with no detection?), built
   with VBUS off, not a working configuration.
5. **Layout C, the device-mode route for L1 until 3 exists:** `\_SB.UFN0` with `_HID INT3445`
   (`usb-ufn-device`, firmware in device role). It installs ufxsynopsys exactly as the URS function
   child would (same INF section), and the role is really fixed for the boot (no URS to change
   it). Whether ufxsynopsys.sys itself behaves differently by hardware ID is not visible from the
   INF; the first L1 boot shows it. Host tests keep using layout A.

**Attach notification (layout C, learned on the phone 2026-11-07).** With `UFN0` revision 1
(no `_DSM`) ufxsynopsys bound (`Enum\ACPI\INT3445\0`, ConfigFlags 0) but never connected: no
pull-up, the Mac saw nothing, no `Enum\USBFN` child. ufxsynopsys.sys carries Intel's xDCI `_DSM`
GUID `732b85d5-b7a7-4a1b-9ba0-4bbd00ffd511` (the one Linux's dwc3-pci.c calls PCI_INTEL_BXT_DSM)
and waits for the platform to report attach. Intel's xDCI ACPI (Slim Bootloader's Apollo Lake
`ScXdci.asl` and Tiger Lake `PchXdci.asl`, BSD-2-Clause-Patent) documents the functions: 1
"Attach/Detach and Port Detection Properties" (bit 0 = 1: the platform notifies attach/detach),
5 "Attach Notification Enabled" (the function stack is ready; the platform answers with
`Notify (XDCI, 0x80)` attached or `0x81` detached), 3 xDCI status, 4/7 Intel's PMU power state.
`UFN0` revision 2 (`tools/woa/acpi/UsbUfn.asl`) implements 0, 1, 3, 4, 5, 7 and reports
**attached** at function 5 (the blob is enabled only in device role, which L1 boots with the Mac's
cable plugged); detach isn't reported until stage H3's Type-C driver can. Plus `_UPC` Type-C USB2,
as Microsoft's function-controller bring-up page asks.

### 5.1 Device mode needs a USBFN configuration (proposal for L1)

Our image's SYSTEM hive (`build/win/hive/SYSTEM-hk-th`, 2026-11-07) has **no
`Control\USBFN` key at all**: desktop Windows ships the function stack (ufx01000, ufxsynopsys,
genericusbfn are installed) but no configuration. Microsoft's "USB registry settings for a function
controller driver" says every device must set `idVendor`, `idProduct`, `ManufacturerString` and
`ProductString`, and that a custom interface needs `CurrentConfiguration` pointing at a
`Configurations\<name>` whose `InterfaceList` names `Interfaces\<name>` keys; **the interface key's
name becomes the child's hardware ID** (`USBFN\<name>`). genericusbfn.inf matches
`USBFN\GENERICUSBFN` (plain install, no ACL), `USBFN\MTP`, `USBFN\IpOverUSB`, `USBFN\UOSFlashing`,
`USBFN\MainOSFlashing`. (`Control\USB` already has `DualRoleFeatures = 1` and
`OsDefaultRoleSwitchMode = 6`; they don't matter without URS.)

Proposed keys (all under `HKLM\SYSTEM\ControlSet001\Control\USBFN`; written offline by the main
session, clean hive only):

| Key | Value | Type | Data |
|---|---|---|---|
| `USBFN` | `idVendor` | DWORD | L1's choice. The Mac stalls on a never-seen VID:PID until the user approves it (memory note): the first L1 boot is attended, so a new ID is fine there; 1209:0001 (the Utility OS's) is known to the Mac |
| `USBFN` | `idProduct` | DWORD | as above |
| `USBFN` | `ManufacturerString` | SZ | `Starbite` |
| `USBFN` | `ProductString` | SZ | `Starbite Link` |
| `USBFN` | `CurrentConfiguration` | SZ | `StarbiteLink` |
| `USBFN` | `IncludeDefaultCfg` | DWORD | 0 |
| `USBFN\Configurations\StarbiteLink` | `InterfaceList` | MULTI_SZ | `GenericUsbFn` |
| `USBFN\Interfaces\GenericUsbFn` | `InterfaceGUID` | SZ | `{d274dae6-37b0-4c76-bf10-3a9681346dd1}` (new; the user-mode side finds the interface by it) |
| `USBFN\Interfaces\GenericUsbFn` | `InterfaceDescriptor` | BINARY | `09 04 00 00 02 FF 00 00 00  07 05 81 02 00 02 00  07 05 01 02 00 02 00` |

The descriptor: one vendor-class interface (FF/00/00) with bulk IN 0x81 and bulk OUT 0x01, 512
bytes. The stack fills in `bInterfaceNumber` and `iInterface`. The doc calls the value "a binary
representation of an interface descriptor" and says class-specific descriptors can be part of the
set, so the endpoint descriptors are given right behind it [unverified: no Microsoft sample of
the bytes is published; the first L1 boot's Enum keys and the Mac's `ioreg` show whether the
endpoints came through]. With the key named `GenericUsbFn` the child is `USBFN\GenericUsbFn` and
genericusbfn.sys binds inbox. L1's own endpoint layout replaces this one when it exists.

### 5.2 Seeing inside the device stack: an ETW autologger (L1 diagnosis)

L1 with `UFN0` revision 2 (image 7fe37f7e…) was still silent. Windows' PnP log says `ACPI\INT3445\0`
was installed (event 400, ufxsynopsys.Install.NT) and **started without a problem** (event 410,
Problem 0); still no `Enum\USBFN` child and no pull-up. The System log has no UFX events: both
drivers log only to ETW manifest providers, which no default session records:

| Provider | GUID | Useful events |
|---|---|---|
| Microsoft-WindowsPhone-UfxSynopsys (ufxsynopsys.sys) | `49b12c7c-4bd5-4f93-bb75-30fce739600b` | 101/102 every register READ/WRITE (offset, value), 302 charger detection error, 303 port type failed, 304 register information, 305 connected/port type/device state |
| Microsoft-WindowsPhone-Ufx (ufx01000.sys) | `e98ebdbf-3058-4784-8521-47860b1d2b8e` | 108-116 its state machines (created, enqueue/dequeue of events, unhandled events), 101/104 failures, 118/119 notifications |

UFX also reads, besides `Control\USBFN`: the MDM policy
`HKLM\SOFTWARE\Microsoft\PolicyManager\{Default,Current\Device}\Connectivity\AllowUsbConnection`,
`Control\USB\DualRoleFeatures` (+ `DualRoleFeaturesTestOverride`), `UcmIsPresent`,
`Control\ManufacturingMode\Current\USBFN` and `Control\Usbfn\Settings`. Checking the policy
needs the SOFTWARE hive.

**Autologger** (offline, `HKLM\SYSTEM\ControlSet001\Control\WMI\Autologger\StarbiteUfx`,
modelled on this image's own `Tpm` autologger; undo = delete the key):

| Key | Value | Type | Data |
|---|---|---|---|
| `StarbiteUfx` | `Start` | DWORD | 1 |
| | `GUID` | SZ | `{696fc72d-68b5-47e1-8806-aac101ebe127}` |
| | `FileName` | SZ | `%SystemRoot%\System32\LogFiles\WMI\StarbiteUfx.etl` |
| | `LogFileMode` | DWORD | 0x10000001 (sequential: keeps the start, where the init is) |
| | `MaxFileSize` | DWORD | 64 (MB) |
| | `BufferSize` | DWORD | 64 (KB) |
| | `FlushTimer` | DWORD | 1 (s) |
| | `ClockType` | DWORD | 2 |
| `StarbiteUfx\{49b12c7c-4bd5-4f93-bb75-30fce739600b}` and `StarbiteUfx\{e98ebdbf-3058-4784-8521-47860b1d2b8e}` | `Enabled` | DWORD | 1 |
| | `EnableLevel` | DWORD | 5 |
| | `EnableFlags` | DWORD | 0xFFFFFFFF |
| | `MatchAnyKeyword` | QWORD | 0xFFFFFFFFFFFFFFFF |

After the run (same image, nothing else changed), pull
`C:\Windows\System32\LogFiles\WMI\StarbiteUfx.etl` read-only from TWRP and decode it here:
`tools/woa/ufx-etl.py decode StarbiteUfx.etl ufxsynopsys.sys ufx01000.sys` (the drivers from
`build/woa/usb-inf/`; needs `etl-parser==1.0.1` in a venv). `ufx-etl.py manifest <driver>` lists
each driver's events from its own embedded manifest (checked on both drivers).

## 6. Build

```sh
tools/woa/build-win-usbhost.sh                          # the three USB images + all checks
EXTRA_AML="other.aml ..." tools/woa/build-win-usbhost.sh # + other tracks' blobs in every image
```

Every image is **today's Windows UEFI (src38) plus added files only**. The script (Mac side,
edk2 and iasl in the lima VM `starbite`):
1. runs the s9usb host test and checks patch 0024 is up to date (`gen-0024.py --check`);
2. compiles `tools/woa/acpi/*.asl` with iasl and packs each with `ssdt-ffs.py` (plus
   `EXTRA_AML`; the USB blobs gated), and checks their GUIDs equal the enable GUIDs compiled into UsbDxe.c;
3. re-makes src38 from src38's own pieces (its SEC/PEI FD `Star2LtePkg-src38.fd`, the release
   DXE volume with the release RTC killed and `pmic-rtc.ffs` added) and requires it to equal
   `src38.fd` **byte for byte**;
4. per image: builds Star2LtePkg like src38 (patches 0001, 0002, 0005, 0018-0024,
   `STAR2LTE_RAM_STAGE=3`, DEBUG) with `-D STAR2LTE_USB_DXE=TRUE -D STAR2LTE_SSDT_DXE=TRUE` and
   the image's `STARBITE_USB_ROLE/ACPI/VBUS`; extracts the two modules, checks the policy marker
   string (`usb-policy role=r acpi=a vbus=v`) in the USB module, adds the modules and the blob(s)
   to step 3's recipe, and checks with `dxe-diff.py` that the result is src38 plus exactly those
   files (SEC/PEI identical, every other DXE file identical);
5. writes `build/woa/starbite-uefi-<image>.bin` (src38's bootshim + the FD) and sha256s.

| Image | Role | ACPI | VBUS | For |
|---|---|---|---|---|
| `usbh-vbus0` | host | A (`SBUSBXHC`) | off | T1 |
| `usbh-vbus1` | host | A (`SBUSBXHC`) | MCU | T2-T5 |
| `usb-urs-auto` | auto | B (`SBUSBURS`) | off | URS experiment (section 5, item 4) |
| `usb-ufn-device` | device | C (`SBUSBUFN`) | off | L1 without URS (section 5, item 5; run by the main session) |

`SUFFIX=-gpu EXTRA_AML=<StarbiteGpu.aml>` builds the same four with the GPU track's `SBGPU` blob as
`starbite-uefi-<name>-gpu.bin` (sha256s in `build/woa/usbh-SHA256SUMS-gpu`).

A fresh edk2 build is not byte-reproducible (`__DATE__`, `__TIME__` and BUILD_EPOCH move strings
and constants, which changes the SEC/PEI layout), which is why only the new modules come from it.

Patch 0024 (`0024-starbite-usb-ssdt-dxe.patch`) is generated by `tools/woa/patches/gen-0024.py`:
both modules (the USB one with a copy of `starbite/s9usb`) and their DSC/FDF lines under
`!ifdef STAR2LTE_USB_DXE` / `!ifdef STAR2LTE_SSDT_DXE`, so every other build is unchanged.

## 7. Stages

| # | What | Done when |
|---|---|---|
| **H1** | Star2LteUsbDxe + Star2LteSsdtDxe + blob `SBUSBXHC`, VBUS policy `off` and `mcu` (built) | Device Manager shows "USB xHCI Compliant Host Controller" working; a keyboard, a mouse and a stick work |
| H2 | Root-hub port objects (`_UPC`/`_PLD`, USB 3 port not connectable) from H1's logged port numbers | Only one port shown; no phantom SuperSpeed port |
| H3 | KMDF `Star2LteUsbc` (needs `tools/win/`): owns HSI2C 0x14360000, polls CC/BC every 250 ms, re-routes D+/D- after attach, AP boost if the MCU won't, boost off on sink/detach/shutdown | Hot-plug works any number of times; no VBUS with a charger |
| H4 | Layout B (`SBUSBURS`, built; the host child binds inbox usbxhci) + H3's UCM connector: host ↔ device at runtime (shared with L1) | Mac cable → Starbite Link, OTG adapter → host, without a reboot |
| H5 | SuperSpeed: the USBDP combo PHY (0x110A0000/0x110B0000, CAL `sub_phy_version 0x400`) and lane orientation from the MCU | A stick reads at > 40 MB/s |

## 8. Risks

1. **Windows' xHCI and DWC3 quirks.** Linux applies three DWC3 quirks to its generic xHCI driver
   (`drivers/usb/dwc3/host.c`): `xhci-sg-trb-cache-size-quirk`, `write-64-hi-lo-quirk`, and
   `quirk-broken-port-ped` for ≤ 3.00a. Windows knows none of them. Snapdragon laptops show the
   inbox driver copes with DWC3 in practice, but our revision may differ (logged). Symptom would be
   transfer errors or corruption on large copies: the test copies a file both ways and compares
   hashes.
2. **DMA reach.** Windows has RAM at 0x8_8000_0000..0x8_FFFF_FFFF. If the controller's bus
   can't reach above 4 GiB despite AC64, transfers fail or hit wrong memory. UFS DMA (same
   FSYS0 block) works there, which is encouraging, not proof. Test: a large copy plus hash; if it
   fails, a test image with Windows limited to the low 2 GiB (`STAR2LTE_RAM_STAGE=2`) separates it.
3. **The MCU's auto-VBUS** (0x54/0x02) is unverified on hardware. Worst expected case: no VBUS
   (nothing enumerates without a powered hub). The MCU follows Type-C rules, so it shouldn't
   source into a charger. First run with the user present.
4. **D+/D- after attach.** The MCU may move the switches when it detects a new partner; the
   firmware routes them only once at boot. If a device attached after boot doesn't enumerate but
   one attached before boot does, this is why (fixed in H3).
5. **The controller left running by the loader.** The loader's background USB link at the menu
   must be stopped before handoff (it is today: Windows boots). The DXE's soft reset stops it
   anyway.
6. **Boot hang.** Touching DWC3 or USBCON with its clock off can hang the bus. The gates are
   opened before USBCON or DWC3 is read, exactly as the loader does on every boot. A hang shows as
   a stall before the Windows logo, with the last `=USB` line in the pstore ring telling which
   step. The worst case of slow I2C (an MCU that doesn't answer) is about 2-3 s of retries.
7. **Other release modules that read the XSDT.** The release's "P" module (APIC/XSDT) and the PSCI
   EL2 shim (`acpi=`) look at the tables too; they already live with the GPIO module's SSDTs, and
   ours comes last. T1 checks that Windows, touch and restart are unchanged.
8. **Memory map.** The module maps the USBCON, CMU, HSI2C and CMGP blocks itself (the firmware's
   page tables don't, outside its USB debug profile). Blocks already mapped uncached (the PMU page,
   which carries EFI_MEMORY_RUNTIME for the release's ResetSystem) are left exactly as they are.
   New GCD entries are plain MMIO without the runtime bit, so Windows' memory map is unchanged.
9. **USB 3 port.** The xHCI reports a SuperSpeed port with nothing behind it (PIPE forced idle).
   Expected harmless (Linux does the same in the rescue design [unverified]), but Windows may log
   port errors or show a second root port until H2.

## 9. Phone test (H1)

**Images** (`build/woa/` of this worktree, sha256 in `build/woa/usbh-SHA256SUMS`):
`starbite-uefi-usbh-vbus0.bin` (VBUS off) and `starbite-uefi-usbh-vbus1.bin` (MCU auto-VBUS).
Each is today's Windows UEFI (`starbite-uefi-src38.bin`) plus Star2LteUsbDxe, Star2LteSsdtDxe and
the blob `SBUSBXHC` (section 6), same size. They replace it for the test boots. How a UEFI image gets to the phone (store entry, catalog
key `win11`) is the main session's flow; nothing else changes.

**Rules:** leave Windows **only via Start → Power → Restart** (NOTES 2026-11-07: forced resets
damaged the registry hives). A forced reset is fine only for a hang **before** the Windows logo
(NTFS isn't mounted yet). No charger on the phone during the tests.

**Hardware:** a USB-C OTG adapter (USB-C plug → USB-A socket), a USB keyboard, a USB stick (FAT32
or exFAT), ideally a USB-C power meter for T5. A self-powered hub doesn't help with vbus0: a
compliant hub connects upstream only when it sees 5 V there, and must not feed power back.

**Report script (optional, only if NTFS is written offline anyway):** copy
`tools/woa/winpe/usb-report.ps1` to `C:\Starbite\usb\usb-report.ps1` and
`tools/woa/winpe/usb-report.cmd` into
`C:\Users\starbite\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup\`.
60 s and again 4 min after logon it writes `C:\Starbite\usb\report-<time>.txt` (read-only:
the controller's status, problem code, resources and ACPI path, USB devices, disks, `usbxhci.inf`'s
ACPI IDs, the boot's USB events, every ACPI device with its path, so TSP0/BTNS show the GPIO
handoff still published its devices). Undo: delete both files. Without it, photos of Device Manager do.

| Test | Image | Do | Expect | If not |
|---|---|---|---|---|
| **T1** | vbus0 | Boot Windows, nothing plugged | Desktop as today; **touch works as before** (keys never worked in Windows). Device Manager → Universal Serial Bus controllers: "USB xHCI Compliant Host Controller" and "USB Root Hub (USB 3.0)", no yellow mark. Then Start → Restart: **the restart works** | Touch gone: the GPIO handoff saw our table (section 4), report it. Yellow mark: its code. No controller: the module refused (`=USB` lines). Restart broken: the PMU mapping (risk 8) |
| **T2** | vbus1 | **User present.** Desktop up, then the OTG adapter + keyboard alone | VBUS on (keyboard LEDs flash), the keyboard types (Notepad) | Nothing: unplug. The `=USB` lines tell whether the MCU answered 0x54; no VBUS → stage H3 |
| **T3** | vbus1 | Unplug the keyboard, plug it back; then a mouse | Each comes back / works | Only the first plug works: D+/D- (risk 4) → H3 |
| **T4** | vbus1 | The stick through the adapter: copy a ~200 MB file onto it and back, `certutil -hashfile <file> SHA256` both | Same hash both ways | Risks 1/2 |
| T5 | vbus1 | With a power meter if available: unplug everything; the port should show no voltage. Then Restart into TWRP before any charger is plugged | 0 V with nothing attached | VBUS stays on with nothing attached: unplug, report |

**Logs after the tests** (next boot to TWRP, as for the earlier Windows runs): the pstore ring
(`pmsg-ramoops-0`) holds both modules' lines unless Windows' UFS driver flooded it; the zone-0
evidence ring (section 4.2, `fastboot oem log dmesg` at the menu, before TWRP) always does:

```
=SSDT v3 at dispatch
=USB v4 usb-policy role=0 acpi=0 vbus=<0|1>
=USB found top=../../.. fsys=../../../.. pmu=../..           (clocks and PMU as the loader left them)
=USB found qch=../../../../..
=USB found usbcon ver=.. link=.. clkrst=.. utmi=.. hsp=.. tune=.. test=..
=USB max hw=.. fw=.. usbc1=.. bc=.. cc0=.. ccstat=<n> vbusdet=<0|1>
=USB role host (build)
=USB phy link=.. clkrst=00000004 utmi=0000000c hsp=.. tune=712c0607 test=.. line=SE0
=USB dwc3 snpsid=5533xxxx gctl=..1... gusb2=.. gusb3=.. guctl=05004000 guctl1=.. bus0=.. fladj=..
=USB xhci caplen=.. hciver=.. ports=2 ac64=1 usbsts=00000001 usb2=1+1 usb3=2+1
=USB vbus policy 1 -> 0 for this boot: ...                      (only if VBUS was already powered)
=USB muic ctrl1=09 (routed) op54 data=<00|02> answer=54 .. .. .. (answered) i2c_errors=0
=USB enable SBUSBXHC: Success
=USB ready, line=..
=SSDT installed SBUSBXHC (120 bytes, rev 1, gated, enabled by its driver)   (or "at dispatch")
=SSDT dispatch done: ...
```

A failure shows as `=USB FAIL <step>: <why> (no USB blob enabled)`, and the SSDT installer's line
`=SSDT SBUSBXHC waits for its driver's enable at dispatch` is then the last word on that blob.

**Undo:** put `starbite-uefi-src38.bin` back as the Windows UEFI. The module writes no variables,
files or storage. The MCU keeps its auto-VBUS setting until its next reset (TWRP's shutdown) or the
next OS that sets it (section 3.4).

### 9.1 Results

**T1 v2 (2026-11-07, image 7cfacbbd…): inconclusive.** Windows booted, but neither `PNP0D10` nor
the GPU track's plain `SBGPU` appeared in Windows' Enum; the pmsg lines were overwritten by Windows'
UFS driver. Led to v3: tables installed during dispatch (section 4.1) and the zone-0 log (4.2).

**T1 v3 (2026-11-07, `win11-usbh0`, image f4432dde…, the Mac's cable plugged at boot): passed.**
Windows: `Enum\ACPI\PNP0D10\0` service USBXHCI "USB xHCI Compliant Host Controller",
`Enum\USB\ROOT_HUB30\...` USBHUB3 "USB Root Hub (USB 3.0)", both ConfigFlags 0;
`Enum\ACPI\STRB0072\0` present (the GPU track's, no driver yet); touch fine; Start → Restart
worked (offline SYSTEM clean). The zone-0 log (read at the menu, `build/win/results-usbh0-v3/zone0.bin`
of the main checkout; 1040 bytes and nothing else in zone 0 after the Windows session):

| Item | Value | Meaning |
|---|---|---|
| found (loader's state) | utmi 0x30, hsp 0x10003300, tune 0xD16F3433, link 0x53FF0 | the loader's device-mode PHY, as expected |
| PHY after | utmi 0x0C, hsp 0x10000300, tune 0x712F3637, line SE0 | host column; fields outside the tune mask kept |
| GSNPSID | **0x5533310A = DWC_usb3 3.10a** | GUCTL1 FORCE_20_CLK applies (≥ 2.90a); no broken-port-PED quirk (> 3.00a) |
| GCTL / GUCTL / GUCTL1 / GSBUSCFG0 / GFLADJ | 0x30C11404 / 0x0500C010 / 0x0400018A / 0x22220009 / 0xA0 | as designed (PRTCAPDIR 1) |
| xHCI | CAPLENGTH 0x20, HCIVERSION 1.10, 2 ports, AC64 1, USBSTS halted | **USB 2.0 = port 1, USB 3 = port 2** (for H2's port objects) |
| MAX77705 | HW 0x13, FW 0x32; BC 0x81 (VBUSDet), CC_STATUS0 0x71 (CCStat 1: sink, the Mac) | read fine |
| MUIC | CONTROL1 read back 0x09 (routed); opcode 0x54 data 0x00 answered "54 00"; 0 I2C errors | |
| SSDT | UsbDxe ran first: `SBUSBXHC` installed "gated, at dispatch", `SBGPU` "at dispatch", 2 of 2 | |

**L1 v3 (2026-11-07, main session, `win11-usb`, image 75467bf2…, `UFN0` revision 1, USBFN keys
1209:0002 written offline): driver bound, no connection.** Zone 0: role device, PHY device column
(utmi 0x30, hsp 0x10003300), GCTL 0x30C12404 (PRTCAPDIR 2), MUIC routed, `SBUSBUFN` installed.
Windows: `Enum\ACPI\INT3445\0` service ufxsynopsys, ConfigFlags 0; no `Enum\USBFN`; the Mac saw
no device. Cause and fix: the attach `_DSM` above (`UFN0` revision 2, image 7fe37f7e…).

## 10. Side finding: the PPTT has no caches (the 0xC0000094 crashes)

Windows' System log shows `OneDriveSetup.exe` and then the `WerFault.exe` started for it failing at
start with **0xC0000094** (STATUS_INTEGER_DIVIDE_BY_ZERO) on almost every boot **since the first
desktop boot after the install**, long before any of this track's firmware (27 boots in the log,
every firmware since August). OneDriveSetup is an x86 program (ARM64's x86 emulation); a test of an
x86 against an ARM64 program would show whether only emulated programs are hit (asked of the main
session, with Application.evtx).

The release PPTT (the platform module's, woa `Pptt.aslc`, the same in src38: 256 bytes, revision 3)
has the package, two clusters and eight leaf cores, and **no cache structures**. Code that sizes
something by the reported cache line or cache size can then divide by zero. The fix under test:

- `PpttBuild.c` (portable, host test `tools/woa/modules/Star2LteSsdtDxe/test/run.sh`: 94 checks on
  the release's own PPTT, plus iasl's disassembler as an independent parse) adds, for every core of
  the cluster holding ACPI processor ID 0 (the A55s, the only cores Windows runs here), its level-1
  and level-2 caches and one shared level-3 cache, chained by "next level of cache"; the M3 nodes
  stay as they were. Revision-3 cache structures with cache IDs.
- `PpttFix.c`, part of `Star2LteSsdtDxe` when built with `STARBITE_PPTT_FIX=1` (a third module didn't
  fit the release's fixed DXE slot): during dispatch it reads the real geometry from the boot CPU
  (CLIDR_EL1, then CSSELR/CCSIDR_EL1 per cache, both CCSIDR formats per ID_AA64MMFR2_EL1.CCIDX),
  logs it (`=PPTT L1 data line 64 ways 4 sets 128 = 32 KiB` ...), builds the new table and swaps it
  (uninstall + install through the ACPI table protocol; the old table goes back if the install
  fails). Anything unexpected leaves the PPTT unchanged.
- Test image: **src38 + only that module** (no USB module, no blobs), `starbite-uefi-pptt-only-rel.bin`
  (`UEFI_TARGET=RELEASE PPTT=1 SUFFIX=-rel tools/woa/build-win-usbhost.sh`). Passes if OneDriveSetup
  and WerFault stop failing; Windows must still show 4 CPUs.
- Windows' Application log (2026-11-07, `results-t5c`): every 0xC0000094 is in **xtajit64.dll**
  10.0.22621.7582 (the x64/ARM64EC emulator) at offset 0x30C70, during its start, in DllHost,
  backgroundTaskHost, tzsync and the Start menu's shell (MoAppCrash of MicrosoftWindows.Client.CBS):
  x64/ARM64EC code anywhere dies. Not analysed inside the emulator; the PPTT image is the experiment.
- Combined image for one boot with the L1 trace: `starbite-uefi-usb-ufn-device-gpu-pptt.bin`
  (`ONLY=usb-ufn-device PPTT=1 EXTRA_AML=<StarbiteGpu.aml> SUFFIX=-gpu-pptt`): device role, `SBUSBUFN`,
  `SBGPU` and the PPTT fix; it fits.
- Space: the release's outer volume has a fixed slot for the compressed DXE volume (0x51000);
  the host-role USB image + SSDT/PPTT + the USB and GPU blobs is 88 bytes over. Before combining everything, room must
  be made (candidates: the release's SerialDxe, which nothing consumes without a TerminalDxe, or
  shorter log strings in Star2LteUsbDxe).
- Risk: CLIDR/CCSIDR reads at boot (our XNU and Linux read them on this phone too); a malformed PPTT
  would stop Windows early (hidden test entry; forced reset is fine before the logo).

## 11. Licences and credits

- `starbite/s9usb/*` and the DXE module: GPL-2.0-only, like the libsb/loader code they come from
  (which follows Samsung's GPL-2.0 kernel: CAL PHY sequence, MAX77705 headers, DT tuning values).
  The DXE is a separate module in the firmware volume (aggregation with the BSD edk2 modules).
- Cross-checked against woa's `UsbDebugHid.c` (BSD-2-Clause-Patent, exynos9810-woa) and an earlier port's
  `max77705.c` (MIT) for the CONTROL1 read; Linux `drivers/usb/dwc3` (GPL-2.0) for the
  core init order and the xHCI quirk list.
- Sources: Raspberry Pi edk2 `Xhci.asl` (BSD-2-Clause-Patent) for the `PNP0D10` pattern;
  Microsoft "Other ACPI namespace objects" for the xHCI compatible ID; Samsung's MAX77705 driver
  as published in LineageOS `android_kernel_samsung_universal9810` (lineage-17.1, GPL-2.0;
  read, not copied) for the auto-VBUS opcode; woa's `GpioHandoffDxe.c` and
  `Star2LteResetSystemLib.c` (BSD-2-Clause-Patent) for the constraints in section 4 and risk 8;
  Intel's xDCI ACPI in Slim Bootloader (BSD-2-Clause-Patent; read, not copied) for the attach
  `_DSM`; `test/release-pptt.bin` is the woa release's PPTT (BSD-2-Clause-Patent), a test fixture.
