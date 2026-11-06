# Starbite Rescue mode

Rescue mode is the last way back when nothing else on the phone starts. You pick it from the
Starbite Recovery menu (hold **Bixby** right after S-Boot's splash, see
`docs/STARBITE-RECOVERY.md`), or run `reboot-to rescue` from any Starbite Linux. The phone reboots
straight into a small Linux ramdisk that asks for a **Starbite rescue USB disk** and runs the
script on it. The Starbite Installer is meant to make that disk.

Status: the loader side (finding the rescue system, the reboot, the start) is done. The ramdisk
(`init`, `sb-reboot`, `mk-rescue-initrd.sh`) builds (2026-11-05) but has not run on the phone.
**USB host mode is written but untested on hardware** (`docs/RESCUE-USB-HOST.md`): the store's
`rescue.dtb` makes the DWC3 a host and gives the port 5 V. The microSD fallback works without it.

**Also inside Starbite Maintenance (2026-11-06, docs/MAINTENANCE.md 7):** SBMAINT's rescue mode
(`sb.rescue=1`) runs this same `init` as `/bin/sb-rescue` with `SB_RESCUE_EMBEDDED=1` (the mounts are
SBMAINT's), `SB_RESCUE_DEVICE_HOOK` (Volume Down: USB networking + ssh, not only the serial shell) and
`SB_RESCUE_HOST_HOOK`. Without those variables this file behaves exactly as before. This small ramdisk
stays for the SBRESCUE bundle in BOOT, which cannot hold SBMAINT.

## Files

| File | What |
|---|---|
| `init` | the ramdisk's `/init` (busybox sh): the screen, the keys, USB host/device, finding and running the rescue disk |
| `sb-reboot` | `/bin/sb-reboot menu\|twrp\|download` in the ramdisk: USB VBUS off, sync, INFORM3, sysrq b |
| `mk-rescue-initrd.sh` | builds `rescue.initrd` (static busybox + `init` + `sb-reboot`, gzip cpio, about 1 MiB) in the Lima VM |
| `mk-rescue-dtb.sh` | `rescue.dtb` from the daily DTB: DWC3 host by default, OTG VBUS, loglevel=7 (run by `build-kernel.sh`) |
| `mk-rescue-usb.sh` | a rescue USB disk **image file** (FAT32 `SB-RESCUE` by default, exFAT, or ext4 `STARBITE-RESCUE`) with `usb/rescue.sh` |
| `usb/rescue.sh` | the sample rescue script: system info, a log on the disk, Volume Up TWRP / Volume Down Download |
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
  can all create it), with the label **`STARBITE-RESCUE`** or **`SB-RESCUE`**. FAT32 and exFAT
  labels hold 11 characters, so on those use `SB-RESCUE`; ext4 can carry `STARBITE-RESCUE`.
  `mk-rescue-usb.sh` makes such a disk as an image file.
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
- **Volume Down**: switch USB VBUS off and the port to device mode (the DWC3 role switch), and
  start a CDC-ACM serial shell, for debugging the rescue system from a computer. Volume Up
  switches back to host mode.

Every reboot from the ramdisk goes through `sb-reboot`: sysrq "b" runs no driver shutdown, so the
OTG boost has to be switched off first.

## Kernel: USB host mode (docs/RESCUE-USB-HOST.md)

Done in the kernel port, selected per boot by the DTB: the store entry `rescue.dtb`
(`mk-rescue-dtb.sh`, built by `tools/linux/build-kernel.sh`) has the DWC3 as `dr_mode = "otg"` +
`usb-role-switch`, default host; the charger's `usb-otg-vbus` regulator on; and the MUIC in host
role. The daily DTB is unchanged in behaviour (`check-kernel.py dtb` checks both). Drivers:
`phy-exynos9810-usbdrd` (host UTMI setup and host tuning on `phy_set_mode`), `max77705_charger`
(OTG boost regulator), `s9p-max77705-muic` (VBUS from the Type-C state). xHCI, mass storage, UAS,
SCSI disk, FAT/exFAT/ext4 are built in (`tools/linux/config/starbite.config`).

## Building and installing

```sh
# kernel + rescue.dtb (outputs in the VM; see docs/RESCUE-USB-HOST.md for the isolated variant)
limactl shell starbite -- /Users/dev/project-starbite/tools/linux/build-kernel.sh
limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/rescue/mk-rescue-initrd.sh
limactl shell starbite -- bash /Users/dev/project-starbite/tools/rescue/mk-rescue-usb.sh \
    ~/distros/out/starbite-rescue-usb.img
# store path: rescue.image (that build's Image), rescue.dtb, rescue.initrd, see
# docs/RESCUE-USB-HOST.md "Deploy". Or the BOOT path (needs a kernel small enough for BOOT):
python3 tools/rescue/bundle-rescue.py payload.bin payload-rescue.bin \
    --image rescue.Image --dtb rescue.dtb --initrd rescue.initrd
```
