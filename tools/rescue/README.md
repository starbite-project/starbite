# Starbite Rescue mode

Rescue mode is the last way back when nothing else on the phone starts. You pick it from the
Starbite Recovery menu (hold **Bixby** right after S-Boot's splash, see
`docs/STARBITE-RECOVERY.md`), or run `reboot-to rescue` from any Starbite Linux. The phone reboots
straight into a small Linux ramdisk that asks for a **Starbite rescue USB disk** and runs the
script on it. The Starbite Installer is meant to make that disk.

Status: the loader side (finding the rescue system, the reboot, the start) is done. The ramdisk
(`init`, `mk-rescue-initrd.sh`) is written but has not been built or run. **USB host mode is not
wired up in the kernel yet** (see "Kernel"), so today only the microSD fallback can work.

## Files

| File | What |
|---|---|
| `init` | the ramdisk's `/init` (busybox sh): the screen, the keys, finding and running the rescue disk |
| `mk-rescue-initrd.sh` | builds `rescue.initrd` (static busybox + `init`, gzip cpio, about 1 MiB) in the Lima VM |
| `bundle-rescue.py` | appends an `SBRESCUE` bundle (kernel + DTB + initrd) to the BOOT payload |

## How the loader finds it (`shim/starlte/xnu_boot.c`)

The rescue start does not use the catalog or the boot menu. It also skips POST.

1. **A BOOT bundle** `SBRESCUE` (`bundle-rescue.py`). It is read from RAM, needs no UFS and has a
   CRC32 for each part. Append it **after** every other bundle, because older loaders stop walking
   at a magic they do not know. BOOT has about 30 MB free, so the bundle needs a slim kernel (a
   USB-host + mass-storage + simplefb config). The full `linux.image` does not fit next to iOS.
2. **The OS store**: `rescue.initrd` (required), with `rescue.image` or else `linux.image`, and
   `rescue.dtb` or else `linux.dtb`. This path needs UFS.

The kernel command line gets `sb.rescue=1` and the bundle's own cmdline. The loader also switches
on the microSD rails, as it does for every Linux.

The request travels through the boot-next RAM mailbox at 0xFED17FE0: `"SBNEXT2!"` followed by the
key `rescue`. The loader reads it right after the Bixby window and clears it **before** it starts
the rescue system, so a rescue system that crashes is not started again on the next boot. If no
rescue system is found, the recovery menu says so and stays open. Holding Bixby during that boot
also cancels the request.

## The rescue disk (what the installer writes)

- Any filesystem the rescue kernel can mount (FAT32 is the safe choice: macOS, Windows and Linux
  can all create it), with the label **`STARBITE-RESCUE`**.
- A whole disk or a partition, on USB mass storage or on a microSD card.
- `/starbite-rescue/rescue.sh`: run with `sh`. It gets these environment variables:
  - `RESCUE_ROOT`: the mount point, read-only
  - `RESCUE_DIR`: `$RESCUE_ROOT/starbite-rescue`
  - `RESCUE_DEV`: the block device
  - `RESCUE_LOG`: the log file in `/run`
- Optional `/starbite-rescue/rescue.sha256` in `sha256sum -c` format. If any file does not match,
  the script is not run.

`init` sets every UFS LUN read-only before it looks for the disk. A rescue script that restores a
partition must run `blockdev --setrw` on that one partition itself. It must never write EFS,
CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA*, DTB, DTBO, the GPT or the boot LUNs.

Keys in rescue mode:
- **Power** (hold 2 s): reboot to the Starbite menu.
- **Volume Up**: look for the disk again.
- **Volume Down**: switch the port to USB device mode and start a CDC-ACM serial shell, for
  debugging the rescue system from a computer.

## Kernel: what USB host mode needs (not changed here)

Today's Linux port runs the DWC3 (0x10C00000) as a **peripheral** only (`dr_mode = "peripheral"` in
`tools/linux/port/src/mainline-dts/exynos9810-star2lte.dts`), and the USB 2.0 PHY driver
(`port/src/usb/phy-exynos9810-usbdrd.c`) is brought up for device mode. A rescue kernel needs all
of the following:

- **DT**: `dr_mode = "host"` for a host-only rescue kernel, or `"otg"` with `usb-role-switch` so
  the Volume Down serial shell still works.
- **Config**: `USB_XHCI_HCD`, `USB_DWC3_HOST` or `USB_DWC3_DUAL_ROLE`, `USB_STORAGE` (optionally
  `USB_UAS`), `SCSI`, `BLK_DEV_SD`, `VFAT_FS` + `NLS_CODEPAGE_437` + `NLS_ISO8859_1`, `EXFAT_FS`,
  `EXT4_FS`, `DEVTMPFS`, `DEVMEM`, `FRAMEBUFFER_CONSOLE` on the boot framebuffer, `RD_GZIP`.
- **VBUS**: the MAX77705 charger has to switch to reverse boost (OTG, CNFG_00 mode 0xA) to give
  5 V. `max77705_charger` needs an OTG regulator, or the disk needs a powered hub or Y cable.
- **Type-C**: the MAX77705 USB-C/PD MCU has to present Rp (source/DFP role). The MUIC driver
  (`s9p-max77705-muic`) already routes D+/D- to the AP.
- **PHY**: host-mode UTMI setup in `phy-exynos9810-usbdrd.c`: VBUS-valid and ID overrides for
  host instead of device.

Until that exists, a `STARBITE-RESCUE` partition on the **microSD** works the same way (mmc is up
in the port).

## Building and installing (not done yet)

```sh
limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/rescue/mk-rescue-initrd.sh
# store path: add rescue.initrd to the OS store (tools/store/sbstore2.py, type initrd)
# or BOOT path:
python3 tools/rescue/bundle-rescue.py payload.bin payload-rescue.bin \
    --image rescue.Image --dtb linux.dtb --initrd rescue.initrd --cmdline "console=tty0"
```
