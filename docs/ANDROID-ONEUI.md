# One UI 8 as Starbite's main Android (UN1CA 9810)

Decided 2026-11-06 by the user: One UI 8 replaces LineageOS as the main Android, about 16 GiB of data (Windows
kept, shrunk to about 18 GiB), first boot straight to the home screen with no setup. LineageOS (the
Pixel-style image, docs/ANDROID-PIXEL.md section 10) stays the fallback until One UI is proven.

## 1. Source

[Unofficial UN1CA 9810](https://github.com/Eend15/Unofficial-Un1ca-9810) (GPL-3.0; One UI 8.0 / Android 16 for
the Exynos 9810 S9, S9+ and Note9; donor: Galaxy S22 SM-S901B; target firmware: the S9's own). It is built
here, never flashed with its own zip: that zip's installer repartitions the whole UFS with sgdisk (SYSTEM
9.5 GB, VENDOR 1.3 GB, ODM 0.7 GB, CACHE 0.6 GB) and formats USERDATA, which on a Starbite phone holds the iOS
16.5 slot and the OS store.

| Piece | Where | Notes |
|---|---|---|
| Checkout | VM `~/oneui/un1ca` (Android-16 branch, depth 1, LFS pulled, `.git/lfs` cache removed) | `out/` is a link to the build disk |
| Build disk | 50 GB ext4 image `oneui-work.ext4` on the USB stick's exFAT partition `SBWORK`, loop-mounted at VM `/mnt/oneui` | Mac disk was too small; Lima mounts `/Volumes/SBWORK` (remove from `~/.lima/starbite/lima.yaml` after the build) |
| Donor firmware | SM-S901B EUX `S901BXXSOGZH3` (August 2026) | samloader works |
| Target firmware | SM-G960F `G960FXXUHFVG6` from region XXV, stored as `SM-G960F_PHN` | samloader gets 408 for the S9 with a generated IMEI; it works with the device IMEI Bifrost used on 2026-10-26 (read from Bifrost's preferences, never printed or committed) |
| Kernel + DT | UN1CA's DS-ACK V1.12 package (`floyd/G960F-kernel`, `floyd/G960F-dtb`, a Samsung DTBH) | the DT carries the first-stage fstab (SYSTEM, VENDOR, ODM by name) |
| Build tools | `unica build_dependencies` on the arm64 VM | apktool's bundled aapt2 is x86-64: runs through qemu-user-static + binfmt |

Host scripts: `build/oneui-dl.sh` (download + unzip, `SBI` env = device id), `build/oneui-run.sh STEP`
(runs `unica STEP` with the build env), `build/oneui-chain.sh` (waits for both firmwares, then
`make_rom -x -z -F erofs`), `build/oneui-enc-cleanup.sh` (removes the `.enc4` files after decryption).

## 2. Starbite changes (tools/android/oneui/)

`mod-starbite` is a UN1CA mod, installed into the checkout by `prepare-un1ca.sh`:

1. `/data` on **A16DATA**, never USERDATA: the vendor fstab's `by-name/USERDATA` lines are rewritten; the
   build fails if any vendor/odm file still names USERDATA.
2. **adb from the first boot**, accepting only this Mac's key (`product/etc/security/adb_keys`, label
   `adb_keys_file`; the key file is generated into the checkout, never committed) and
   `persist.sys.usb.config=mtp,adb`. This is what lets every unattended boot come back
   (`adb reboot recovery`).
3. **No setup**: `ro.setupwizard.mode=DISABLED` (Google's wizard). Samsung's wizard is finished by
   `first-boot.sh` over adb (provisioned + setup complete, both wizards disabled for user 0, home
   started) until a native way is proven.
4. **Boot guard**: `/system/etc/init/starbite-bootguard.rc` starts `/system/etc/starbite/bootguard.sh` (shell
   user and domain, like `adb shell reboot`). If `sys.boot_completed` is not 1 within `ro.starbite.bootguard`
   seconds (1200; 0 = off), it saves `logcat -b all` and the properties to `/data/local/tmp/starbite-bootguard.*`
   and restarts into recovery. A hang on the boot animation therefore always comes back without the buttons
   (`androidboot.init_fatal_reboot_target=recovery` in the cmdline covers init and critical-service crashes).

**After `make_rom`, always run `tools/android/oneui/postfix-vendor.sh` (VM).** UN1CA's
`exynos9810_boot_restore` patch (build_order 20) runs after every mod and rewrites both vendor fstabs from a
template with /data on USERDATA (ext4 + an f2fs fallback; the build fails without that line), and it sets
`ro.setupwizard.mode=OPTIONAL`. The script fixes the finished work_dir (fstabs -> A16DATA; the uppercase
by-name `userdata_block_device` label in `vendor_file_contexts` moves from USERDATA to A16DATA, so USERDATA
keeps the generic `block_device` label that no Android domain may open) and rebuilds `vendor.img` with
UN1CA's `build_fs_image.sh`, checking the image itself. It never edits the source tree (make_rom's work hash).
The vendor image from the target-files zip must never be written to the phone.

## 3. Boot path

The loader's `android` method starts One UI with four store files: `android.kernel` (DS-ACK G960F kernel),
`android.ramdisk` (UN1CA's), `android.cmdline`, and the new optional **`android.dtb`** (the DS-ACK DTBH):
the loader takes S-Boot's `/chosen`, memory nodes and `model_info-system_rev` and keeps everything else from
the file (loader health update, docs/STARBITE-HEALTH.md). Without the 4th file the old path is unchanged.
The protected DTB partition is never written (on S9 #1 it is all zeros; S-Boot's FDT comes from BOOT's DT
section, packed from LineageOS's dt.img).

`tools/android/oneui/mk-boot-files.py` makes the four files from UN1CA's own inputs:

| File | From | Change |
|---|---|---|
| `android.kernel` | `floyd/G960F-kernel` of `DS-ACK-V1.12-24.10.2026-...-runtime49-AOT-SuSFS-erofs-dtb.zip` (the package UN1CA's EROFS build selects, sha256 87f62131...) | none |
| `android.dtb` | `floyd/G960F-dtb` of the same package (DTBH, one FDT; its first-stage fstab says **erofs** for SYSTEM / VENDOR / ODM) | none, or `--dev OLD=NEW` renames a by-name device in place (same length) |
| `android.ramdisk` | the cpio inside `platform/exynos9810/installer/ramdisk.img` (Samsung's first-stage init + `fstab.samsungexynos9810`) | `/data` USERDATA -> A16DATA, edited entry by entry (owners, modes, every other byte kept) |
| `android.cmdline` | LineageOS's | `androidboot.init_fatal_reboot_target=recovery printk.devkmsg=on` |

The other DS-ACK package in `installer/kernel_dtb/` has an **ext4** first-stage fstab: wrong for this build.

## 4. UFS layout

USERDATA (iOS slot + OS store) sits right after SYSTEM .. CP_DEBUG and is never moved, so One UI's partitions
cannot grow in place. The plan, once the built image sizes are known:

- If system <= 4.75 GiB, vendor <= 0.71 GiB and odm <= 0.68 GiB, One UI uses today's SYSTEM / VENDOR / ODM and
  only A16DATA grows.
- Otherwise the larger ones get new partitions in the space freed from Windows (named so the DT fstab finds
  them, or the DT fstab in `android.dtb` is pointed at them).
- WINDOWS: NTFS 17.9 GB in use; shrink to about 18.5 GB with `ntfsresize` (dry run first). Backup:
  `WINDOWS.ntfsclone.gz` on the card (backup/20261106-oneui/WINDOWS.sha256; verified by a full decompress).
- A16DATA: about 16 GiB, fresh **ext4** (One UI's /data line: `ext4 ... wait,check,encryptable=footer,quota,
  reservedsize=128M,length=-20480`; no file-based encryption, so TWRP and Maintenance can read it). The
  filesystem ends 20 KiB before the partition (`length=-20480`).
- CACHE (12 MiB) and HIDDEN are already ext4 (One UI mounts /cache with `errors=panic`). ODM held LineageOS's
  /metadata; One UI overwrites it.
- UN1CA's sizes (SYSTEM 9500 MiB, VENDOR 1300 MiB, ODM 700 MiB, CACHE 600 MiB) are its ext4 reservations; the
  EROFS images are what count. Source sizes for comparison: S22 system 8.0 GB + product 1.9 GB unpacked (both
  end up in the S9's system), S9 vendor 549 MB unpacked.
- The table change goes through the installer's GPT engine (`tools/installer/sbinst/ufslayout.py`, both GPT
  copies backed up first: backup/20261106-oneui/sda-head-4M.img and sda-tail-4M.img).

## 5. Status (2026-11-06, night)

- Done on S9 #1 (TWRP): NTFS shrunk to 19260243968 B (18 GiB - 64 MiB, no relocations); LineageOS's A16DATA
  saved raw (`A16DATA-lineage.raw.gz` on the card, sha256 03f46a87... = the partition's); both GPT copies
  saved (`backup/20261106-oneui/gpt/orig-head.bin`, `orig-tail.bin`, hashed on the phone and here).
- The `starbite-oneui` table plans cleanly from the phone's GPT (WINDOWS resized in place keeping its type and
  unique GUID, A16DATA moved; nothing else changes): `python3 -m sbinst.gpt plan orig-head.bin orig-tail.bin
  starbite-oneui --blocks 15616000` in tools/installer.
- The loader on the phone (health6) already reads `android.dtb`.
- `make_rom` (EROFS) re-running with the boot guard; then: image sizes -> final table -> writes -> first boot.
