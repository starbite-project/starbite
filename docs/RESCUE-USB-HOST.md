# USB host mode for the Starbite Rescue ramdisk

Written 2026-11-05. The Linux 7.1 kernel for the S9 (starlte) can now be a **USB host** with
**5 V on the port**, so the Starbite Rescue ramdisk (`tools/rescue/`, docs/STARBITE-RECOVERY.md
§5) can read a USB stick through a USB-C OTG adapter. **None of this has run on the phone yet.**
Everything builds, and the DTBs, the ramdisk, the stick image and the sample script pass host-side
checks (section 6).

The daily Linux keeps device mode (USB networking, serial) exactly as before: host mode is chosen
per boot by the DTB, and only the rescue entry uses the host DTB.

## 1. How host mode is chosen

| | daily `linux.dtb` | `rescue.dtb` (store entry for the rescue start) |
|---|---|---|
| `/soc/usb@10c00000` | `dr_mode = "peripheral"` | `dr_mode = "otg"`, `usb-role-switch`, `role-switch-default-mode = "host"` |
| `charger@69/usb-otg-vbus` | `status = "disabled"`: no regulator is registered | `status = "okay"`: the OTG boost regulator exists |
| `muic@25` | `vbus-supply` (unused; a disabled supplier creates no device link) | `vbus-supply` + `starbite,usb-role = "host"` |
| `/chosen/bootargs` | `loglevel=4` | `loglevel=7` (driver and USB lines reach tty0 and ramoops) |

`tools/rescue/mk-rescue-dtb.sh` makes `rescue.dtb` from the daily DTB with `fdtput`, so the two
can't drift apart. It checks that the dts diff is exactly those 9 lines.
`tools/linux/build-kernel.sh` runs it after every build, next to `linux-old.dtb`.
`check-kernel.py dtb` checks that the daily DTB is device-only, and `check-kernel.py dtb
--usb-host` checks that the rescue DTB is complete. Compared with master's daily DTB, the new one
differs only by the disabled `usb-otg-vbus` node, the MUIC's `vbus-supply` and renumbered
phandles.

`otg` with a role switch (rather than plain `host`) keeps the rescue init's **Volume Down** serial
shell: it writes `device` to `/sys/class/usb_role/10c00000.usb-role-switch/role`, and Volume Up
switches back to `host`. The role switch also matters for another reason: on DWC3 3.30a and later,
`dwc3_get_dr_mode()` forces `otg` without a role switch (or extcon) to peripheral. I haven't
checked which revision the 9810's DWC3 is, but with `usb-role-switch` any revision works.

The kernel config already had the dual-role DWC3, xHCI and mass storage from the port's
defconfig base. `tools/linux/config/starbite.config` now pins them, plus UAS, SCSI disk, VFAT,
exFAT, NLS 437/8859-1, ext4, the role switch and the regulator core. All are built in: the
ramdisk has no modules.

## 2. What changed

| File | Change |
|---|---|
| `tools/linux/port/src/usb/phy-exynos9810-usbdrd.c` | `.set_mode`: host UTMI setup + host tuning on `PHY_MODE_USB_HOST`, device values back on `PHY_MODE_USB_DEVICE`; the mode survives a re-init |
| `tools/linux/port/src/soc/max77705_charger.c` | `usb-otg-vbus` regulator (reverse boost), BYP over-current handler, boost off on remove and shutdown, a boost found on at probe goes off |
| `tools/linux/port/src/soc/s9p-max77705-muic.c` | host role: MCU auto-VBUS off, Type-C state poll (250 ms), VBUS on only for `cc_SOURCE`, retries and back-off, `vbus` parameter, off on remove and shutdown |
| `tools/linux/mk-starlte-dts.py` | the disabled `usb-otg-vbus` node (5.0 V, 900 mA) and the MUIC's `vbus-supply` |
| `tools/rescue/mk-rescue-dtb.sh` | new: `rescue.dtb` (section 1); `RESCUE_EXTRA_ARGS` appends bootargs for test variants |
| `tools/linux/build-kernel.sh` | builds and checks `rescue.dtb`; fixed `NO_MODULES=1` (an unmatched `modules-*.img` glob failed `sha256sum`) |
| `tools/linux/check-kernel.py` | `dtb` checks the USB role of both DTBs |
| `tools/linux/config/starbite.config` | the USB host section |
| `tools/rescue/init` | real host/device switching, `SB-RESCUE` label alias, `findfs` when busybox has no `blkid`, USB status line, reboots via `sb-reboot` |
| `tools/rescue/sb-reboot` | new: `menu` / `twrp` / `download` with VBUS off first |
| `tools/rescue/mk-rescue-initrd.sh` | accepts `findfs` (Ubuntu's busybox-static has no `blkid`; master's version refused it), installs `sb-reboot` |
| `tools/rescue/mk-rescue-usb.sh`, `tools/rescue/usb/rescue.sh` | new: the rescue USB **image file** and its sample script |

### The label: STARBITE-RESCUE does not fit FAT32 or exFAT

FAT32 and exFAT volume labels hold **11 characters**, and `STARBITE-RESCUE` has 15
(`mkfs.exfat -L STARBITE-RESCUE` fails with "input string is too long"). The init now accepts
**`SB-RESCUE`** as well. `STARBITE-RESCUE` still works on ext4, or on any filesystem whose label
can hold it. The Starbite Installer has to use one of the two.

## 3. Register level

### 3.1 USB 2.0 PHY (USBCON at 0x11100000)

Reference: Samsung's `drivers/phy/phy-exynos-usb3p1.c` `phy_exynos_usb_v3p1_config_host_mode()`
(called through `phy_set(SET_DPDM_PULLDOWN)` by `dwc3_otg_start_host()` in
`drivers/usb/dwc3/otg.c` because the board has `is_not_vbus_pad = 1`), and
`exynos_usbdrd_phy_tune(OTG_STATE_A_HOST)`. Tuning values come from the stock
`exynos9810-starlte_common.dtsi` `usb_hs_tune` (in `ref/starlte-kernel`), whose
`tune_value = <device host>`.

| Register | Device (unchanged) | Host |
|---|---|---|
| UTMI 0x50 | FORCE_VBUSVALID[5] = FORCE_BVALID[4] = 1, DP/DM_PULLDOWN[3:2] = 0 | FORCE_* = 0, DP_PULLDOWN = DM_PULLDOWN = 1 |
| HSP 0x54 | VBUSVLDEXTSEL[13] = VBUSVLDEXT[12] = 1 | both 0 |
| HSP_TUNE 0x58 | tx_vref 0xd, tx_rise 1, tx_res 3, tx_pre_emp 3, rx_sqrx 4, compdis 3 | tx_vref 0x7, tx_rise 1, tx_res 1, tx_pre_emp 3, rx_sqrx 6, compdis 7 |

dwc3 calls `phy_set_mode(HOST)` after `phy_init`, and in role-switch mode after
`dwc3_host_init()`. The driver remembers the mode, so a later `phy_init` (resume) goes straight
back to host. The SuperSpeed PMA stays powered down with the PIPE3 interface forced idle, as in
device mode. The dump line `host mode: ... utmi=... hsp=... hsp_tune=...` shows the result.

### 3.2 MAX77705 charger: OTG boost (I2C 0x69 on HSI2C 0x14360000)

Reference: Samsung's `drivers/battery_v2/max77705_charger.c`: `max77705_set_otg()`,
`max77705_bypass_irq()`, `max77705_charger_shutdown()` and the OTG current limit in
`max77705_charger_initialize()`. Register names come from `ref/starlte-kernel`
`include/linux/mfd/max77705-private.h` and `.../charger/max77705_charger.h`. The stock 4.9
sources I read were the LineageOS tree `android_kernel_samsung_universal9810` (branch
`lineage-17.1`). They are not in `ref/` (only headers and DTs are), and I didn't commit them.

At probe (the existing driver, unchanged): CNFG_00 0xB7 MODE = 0x5 (charger + buck), CNFG_02 0xB9
OTG_ILIM[7:6] = 01 (900 mA), CNFG_11 0xC2 VBYPSET = 0 (5.0 V), CNFG_08 0xBF FSW = 1.5 MHz,
CNFG_12 0xC3 DISKIP = 0 (auto skip).

| Step | Boost on (`regulator_enable`) | Boost off |
|---|---|---|
| 1 | refuse (-EBUSY) if INT_OK 0xB2 has CHGIN_OK[6] or WCIN_OK[5]: a charger or wireless pad already powers the phone | CNFG_00: OTG[1] = BOOST[3] = 0, CHG[0] back to what it was (0x0E → 0x05) |
| 2 | CHGIN_I and AICL_I interrupts off (the vendor masks CHG_I/CHGIN_I in INT_MASK 0xB1) | wait 50 ms (vendor `mdelay(50)`) |
| 3 | CNFG_08 FSW[1:0] = 0 (3 MHz) | CNFG_08 FSW = 2 (1.5 MHz) |
| 4 | CNFG_12 DISKIP[0] = 1 (no skip mode) | CNFG_12 DISKIP = 0 (auto skip) |
| 5 | CNFG_00 CHG[0] = 0, OTG[1] = BOOST[3] = 1: MODE 0x5 → **0xE** | CHGIN_I, AICL_I back on |
| 6 | log `otg on: CNFG_00=.. CNFG_02=.. INT_OK=.. DTLS=../../..` | log `otg off: ...` |

MODE 0xE is the vendor's state with an OTG cable attached: the battery driver stops charging for
the OTG cable type (CHG = 0), then `max77705_set_otg()` sets `CHG_CNFG_00_OTG_CTRL` (OTG | BOOST)
on top of buck.

The current limit is the regulator's (`regulator-min/max-microamp = 900000`, written to
OTG_ILIM through `csel_reg` 0xB9 bits [7:6]: 500/900/1200/1500 mA). 900 mA is the vendor's
`CONFIG_CHARGER_MAX77705_OTG_LIMIT` value, which is plenty for a USB 2.0 stick.

**Over-current**: the BYP_I interrupt (bit 0 of the charger's regmap-irq) reads DETAILS_02 0xB5.
BYP_DTLS bit 0 set while boosting means boost off at once (vendor `max77705_bypass_irq`). The
driver then sends `REGULATOR_EVENT_OVER_CURRENT`. The MUIC sees the boost gone, retries twice
5 s apart, then leaves it off until the device is unplugged.

**Shutdown/remove**: boost off. On the next boot of a Linux with this driver, a boost found on at
probe is switched off.

### 3.3 MAX77705 Type-C MCU: source role (I2C 0x25)

Reference: Samsung's `drivers/ccic/max77705_cc.c` (the CCStat handler),
`drivers/ccic/max77705_pd.c` `max77705_vbus_turn_on_ctrl()`, `drivers/ccic/max77705_usbc.c`
`max77705_usbc_disable_auto_vbus()` (called from probe) and `drivers/muic/max77705-muic.c`
(`ATTACHED_DEV_OTG_MUIC` → `com_to_usb_ap`).

- The MCU firmware is a dual-role port. It toggles Rp/Rd itself and attaches as source when the
  partner shows Rd, as an OTG adapter does. The vendor sends no CC opcode for OTG, and neither
  does this driver.
- At probe in host role: opcode **0x54** with data **0x00**, auto-VBUS off (vendor: `0x54, 0x02` =
  on, `0x00` = off). VBUS is then the AP's decision alone.
- D+/D- to the AP: CONTROL1 (opcode 0x06) = 0x09 COM_USB, as before. It is also re-sent when
  BC_STATUS 0x08 VBUSDet[7] rises, which our own boost now triggers.
- Every 250 ms (`host_poll_ms`), CC_STATUS0 **0x0A** CCStat[2:0] decides:

| CCStat | Meaning | VBUS |
|---|---|---|
| 2 `cc_SOURCE` | the phone is the source: a device (OTG adapter + stick) is attached | **on**, unless VBUSDet already shows 5 V from someone else (powered hub, Y cable) |
| 1 `cc_SINK` | the phone is the sink: a charger or a computer is attached | **off**, always |
| 0, 3..7 | nothing, accessory, error | off |
| read error | the MCU doesn't answer | off |

`/sys/module/s9p_max77705_muic/parameters/vbus`: 1 = as above (default), 0 = never (the init
writes it before Volume Down and `sb-reboot` before every reboot), 2 = **forced**, boost whenever
no one else powers VBUS, whatever CCStat says. Value 2 is a first-test fallback (section 5),
only with nothing but a USB device attached.

## 4. Sequencing and safety summary

- No boost while a charger or wireless pad powers the phone (the regulator refuses), or while
  CCStat says sink (the MUIC switches it off within one 250 ms poll).
- No boost without a Type-C device attachment (unless `vbus=2`).
- No boost after reboots from the ramdisk: sysrq "b" runs no driver shutdown, so `sb-reboot`
  writes `vbus=0` and waits until `usb-otg-vbus` reads `disabled` before INFORM3 and the reset.
  Kernel reboots/poweroffs run the charger's and the MUIC's shutdown hooks.
- Over-current: hardware limit 900 mA, then boost off on BYP_I, two retries, then off until
  unplugged.
- The daily kernel registers no OTG regulator and never touches the boost.

## 5. First hardware test

**Needs**: the store entries from section 7, the phone charged above 50 %, **no charger
connected**, a USB-C OTG adapter (USB-C plug → USB-A socket, with its 5.1 kΩ Rd; any real OTG
adapter has one), a USB stick written with `starbite-rescue-usb.img` (FAT32 `SB-RESCUE`).
Optional: a powered USB hub for test C.

**A. Rescue start, nothing plugged.** Hold Bixby after the Samsung splash → Starbite Rescue mode
(or `reboot-to rescue`). The screen shows the rescue banner and then
`USB: role host, VBUS off, 0 device(s)`. Expected kernel lines (loglevel 7, so also on screen):

```
phy-exynos9810-usbdrd 11100000.phy: host mode: ... utmi=0000000c ... (DP/DM pull-downs on, FORCE_* off)
max77705-charger 2-0069: otg registered: CNFG_00=05 CNFG_02=4x..7x ... (MODE 0x5, OTG_ILIM[7:6] = 01)
s9p-max77705-muic 2-0025: host mode: MCU auto-VBUS off (0, answer 54 ..), vbus=1
s9p-max77705-muic 2-0025: CC_STATUS0 ..: CCStat -2 -> 0 (nothing attached)
xhci-hcd xhci-hcd.0.auto: xHCI Host Controller
xhci-hcd xhci-hcd.0.auto: new USB bus registered, assigned bus number 1
hub 1-0:1.0: USB hub found / 1 port detected
```

(The I2C bus number may differ; the `2-` prefix is an example.)

**B. Plug in the OTG adapter with the stick.** Within about 1 s:

```
s9p-max77705-muic ...: CC_STATUS0 ..: CCStat 0 -> 2 (source, a device is attached)
max77705-charger ...: otg on: CNFG_00=0e ...
s9p-max77705-muic ...: VBUS on (CCStat 2)
s9p-max77705-muic ...: BC_STATUS .. -> 8.   then   CONTROL1 <= 09
usb 1-1: new high-speed USB device number 2 using xhci-hcd
usb-storage 1-1:1.0: USB Mass Storage device detected        (or: uas)
sd N:0:0:0: [sdX] ... Attached SCSI removable disk
```

Then the init says `Running the rescue script from /dev/sdX1`, the sample script prints system
information and `Log saved: starbite-rescue/logs/rescue-....txt`, and offers Volume Up = TWRP,
Volume Down = Download, Power = back. Unplug: `CCStat 2 -> 0`, `VBUS off (detached)`.

**What a failure means:**

| You see | Likely cause | Next |
|---|---|---|
| CCStat stays 0 after plugging | the MCU doesn't attach as source (firmware state after TWRP's reset, or the adapter has no Rd) | test C, then a `vbus=2` DTB |
| `otg: input power present ..., boost refused` | CHGIN/WCIN sees power | unplug chargers; read INT_OK in the line |
| `otg on` but no `new ... USB device` | data path: CONTROL1 routing, PHY host setup, or xHCI | test C (separates VBUS from data); check the `host mode:` dump |
| `device descriptor read/64, error -71` | signal quality | a DTB with `RESCUE_EXTRA_ARGS="phy_exynos9810_usbdrd.tune=0"` |
| `xHCI ... HC died` / `Host halt failed` | the link without the SS PIPE side | report the log: needs the USB3 PHY path |
| `VBUS dropped while on` / `bypass over-current` | the stick draws more than 900 mA, or a short | another stick; never raise the limit blindly |

**C. Powered hub (VBUS without the boost).** Plug a powered hub (its own supply) through the OTG
adapter, with the stick in the hub. The MUIC sees VBUSDet and logs `VBUS already present ..., no
boost`. If the stick enumerates now but not in B, the data path works and the problem is VBUS.

**D. `vbus=2` (only if CCStat never becomes 2).** In the VM:
`RESCUE_EXTRA_ARGS="s9p_max77705_muic.vbus=2" tools/rescue/mk-rescue-dtb.sh exynos9810-starlte.dtb rescue.dtb`.
Store that as `rescue.dtb` and test B with **only** the adapter and stick attached. Put the
normal `rescue.dtb` back afterwards.

**Reading the logs.** The sample script's Volume Up reboots straight into TWRP. From the Mac
(the host does the adb work):

```sh
adb shell cat /sys/fs/pstore/console-ramoops-0 > build/rescue/rescue-console.txt   # the rescue kernel's console (loglevel 7)
adb shell cat /sys/fs/pstore/pmsg-ramoops-0   > build/rescue/rescue-pmsg.bin       # the loader's POST/boot lines
```

The stick itself holds `starbite-rescue/logs/rescue-<time>.txt` (system information, the full
`dmesg` and the ramdisk log). Plug the stick into the Mac and read it there.

## 6. What was checked here (no hardware)

- Kernel `7.1.0-starbite-e2e92d1c` built in the VM with separate outputs (section 7): all three
  drivers compile without warnings, and `check-kernel.py` passes for the Image, both daily DTBs,
  `rescue.dtb` (`--usb-host`) and the config (223 fragment values).
- `rescue.dtb` is reproducible from the daily DTB. Its diff is exactly the 9 lines of section 1.
- `rescue.initrd` (989 KiB) contains `init`, `bin/sb-reboot` and `findfs`.
- `starbite-rescue-usb.img`: MBR, FAT32 `SB-RESCUE`, `fsck` clean, `rescue.sha256` OK. busybox
  `findfs` finds `SB-RESCUE` (FAT32/exFAT) and `STARBITE-RESCUE` (ext4) on loop devices. The
  init's `find_rescue` (findfs path) picks the right device, and the sample script ran against
  the loop-mounted image: system information, the log written to the stick, read-only again
  afterwards.
- **Not checked**: anything on the phone, including enumeration, VBUS, the CC states, the
  device-mode role switch back, the reboots via `sb-reboot`, and the PHY host tuning.

## 7. Deploy

Outputs are in the VM (`~` = `/home/dev.guest`). The build was:

```sh
limactl shell starbite -- bash -c 'MIN_HOST_FREE_GB=4 WORK=$HOME/linux/rescue-usb-work \
  OUT=$HOME/distros/_kernel-rescue REPO_OUT=$HOME/distros/_kernel-rescue/repo NO_MODULES=1 \
  /Users/dev/project-starbite/.claude/worktrees/rescue-usb-host/tools/linux/build-kernel.sh'
limactl shell starbite -- sudo bash .../tools/rescue/mk-rescue-initrd.sh ~/distros/_kernel-rescue/rescue.initrd
limactl shell starbite -- bash .../tools/rescue/mk-rescue-usb.sh ~/distros/_kernel-rescue/starbite-rescue-usb.img
```

| VM file (`~/distros/_kernel-rescue/`) | Store entry / use |
|---|---|
| `Image` (26 MB, release `7.1.0-starbite-e2e92d1c`) | **`rescue.image`** (type image) |
| `rescue.dtb` | **`rescue.dtb`** (type dtb) |
| `rescue.initrd` (989 KiB) | **`rescue.initrd`** (type initrd) |
| `rescue-vbus-forced.dtb` | test D only |
| `starbite-rescue-usb.img` (64 MiB) | write to a USB stick |

```
41db4c322ba19f74b54bdec5555cefe2a802e2a40a73837e301f6acc0e9058af  Image
04d1b93312d1274bd10e30f4371cb2fe9ed997f8a279393c1ac8be610bd939a8  rescue.dtb
68269eca4f74756f013147469fb918b83baa4340b3c6d2206410451a413fb1df  rescue.initrd
d039e9ef30a2e4aaa3a42ea2e2e5b07e7e50d3d6823230528c4ad620fbd53668  rescue-vbus-forced.dtb
f44e1b5a12a504178444233b35250e7bbd7a26c1d575fc3341d42b4cae44adaf  starbite-rescue-usb.img
```

Use `rescue.image` rather than replacing `linux.image`. The rescue start then doesn't depend on
the daily kernel, and the daily entries keep their own Image/modules pair. `rescue.dtb` needs
the drivers of this exact build, so pair it with this Image. Without `rescue.image` the loader
falls back to `linux.image`, which needs this branch's kernel too.

**Catalog: no change.** The rescue start doesn't use the catalog: the loader has a built-in entry
(`shim/starlte/xnu_boot.c` `rescue_ent`: `rescue.image|linux.image`, `rescue.dtb|linux.dtb`,
`rescue.initrd`, cmdline `sb.rescue=1`). The BOOT payload must be a loader with the rescue path
(master `bd9262a` or later).

Steps (the host does them; TWRP for the store):

```sh
mkdir -p build/rescue
limactl shell starbite -- bash -c 'cp ~/distros/_kernel-rescue/{Image,rescue.dtb,rescue.initrd,starbite-rescue-usb.img} /Users/dev/project-starbite/build/rescue/'
tools/store/store-pull.sh build/store/live.img                       # the phone's live store, in TWRP
python3 tools/store/sbstore2.py build build/store/ufs-store-rescue.img --medium ufs \
    --import build/store/live.img \
    rescue.image=build/rescue/Image:image rescue.dtb=build/rescue/rescue.dtb:dtb \
    rescue.initrd=build/rescue/rescue.initrd:initrd
diff <(python3 tools/store/sbstore2.py list build/store/live.img) <(python3 tools/store/sbstore2.py list build/store/ufs-store-rescue.img)
diff <(python3 tools/store/sbstore2.py catalog-keys build/store/live.img) <(python3 tools/store/sbstore2.py catalog-keys build/store/ufs-store-rescue.img)   # empty
tools/store/store-flash.sh build/store/ufs-store-rescue.img          # in TWRP
# the stick (macOS; double-check N with diskutil list first):
#   diskutil unmountDisk /dev/diskN && sudo dd if=build/rescue/starbite-rescue-usb.img of=/dev/rdiskN bs=1m
```

The `SBRESCUE` BOOT bundle (`bundle-rescue.py ... --image Image --dtb rescue.dtb --initrd
rescue.initrd`) would work without UFS. But this Image is 26 MB, which is too big to sit next to
iOS in BOOT. It needs a slim rescue kernel config first.

## 8. Risks

1. **Boost converter.** MODE 0xE and the 3 MHz/no-skip settings follow the vendor flow but have
   never been set by this kernel. If the silicon wants a different MODE, the boost may simply not
   start (`otg on` with no VBUSDet). The 900 mA limit and the BYP_I handler are the protection;
   there is no thermal monitoring.
2. **Two sources on VBUS.** If something connected behind the adapter (a hub with power
   pass-through) starts feeding VBUS while we boost, both drive the line until the next poll
   (≤ 250 ms) or until the CC state changes. The regulator refuses to start with input power
   present, but can't detect it once boosting (CHGIN then sees our own 5 V). Use plain sticks or
   self-powered hubs only.
3. **Boost left on after a crash.** A kernel panic (`panic=10`) or a hang reset is an emergency
   restart: no shutdown hooks run, so the boost can stay on into TWRP, Download mode, iOS or
   macOS. The next Starbite Linux switches it off at probe; the other systems may not.
   Suggestion for the loader: at every start, clear OTG and BOOST in CHG_CNFG_00 (I2C 0x69,
   register 0xB7, bits 1 and 3) before anything else. That's one I2C read-modify-write on HSI2C
   0x14360000.
4. **Type-C MCU state after reset.** TWRP's kernel resets the MCU on shutdown
   (`max77705_reset_ic`, 0x80 = 0x0F), so we start from the firmware's defaults. Whether that
   default is DRP with auto-VBUS on or off is unverified; we switch auto-VBUS off and assume DRP.
   If the MCU never reports source, use test D. After a rescue boot the MCU keeps auto-VBUS off
   until its next reset, which the next TWRP shutdown provides (and the vendor kernel switches it
   off at probe anyway).
5. **Role switch to device (Volume Down) and back** is untested. The `otg` + `usb-role-switch`
   path of dwc3 is new on this SoC; device mode in the daily DTB doesn't use it.
6. **Host without the SuperSpeed PHY.** xHCI also registers a USB 3 root hub with no PHY behind
   it (PIPE3 forced idle, as in device mode). This is expected to be harmless, but it is
   unverified.
7. **Shared environment.** `exfatprogs` was installed in the Lima VM for `mk-rescue-usb.sh --fs
   exfat`. The VM kernel has no exFAT module, so the default is FAT32, which needs only
   `dosfstools` and a loop mount. The isolated kernel build reused the shared `/mnt/build` tree
   (reset and patched by every build, under the build lock).
