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

VM tool fixes found by full builds (2026-11-07): `patchelf` and `apksigner` installed (apt), and
UN1CA's `out/tools/bin/signapk` replaced by `tools/android/oneui/vm/signapk` (the original kept as
`signapk.orig`): AOSP's signapk.jar loads Conscrypt's JNI library, which it carries only for x86-64, so on
the arm64 VM every APK signing failed. The stand-in runs `zipalign -p 4`, then `apksigner sign` (v1 + v2 + v3,
the same platform certificate). `verify_exynos9810_images.sh` makes its temp dir under the image dir (the VM
root's disk file sits on the nearly full Mac disk). None of these are in make_rom's source hash.

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
3. **No setup**: `ro.setupwizard.mode=DISABLED` (Google's wizard; UN1CA's vendor restore later sets it
   OPTIONAL). Samsung's wizard: section 5 below on the phone, or `first-boot.sh` over adb (the same steps).
4. **Boot guard**: `/system/etc/init/starbite-bootguard.rc` starts `/system/etc/starbite/bootguard.sh` (shell
   user and domain, like `adb shell reboot`). If `sys.boot_completed` is not 1 within `ro.starbite.bootguard`
   seconds (1200; 0 = off), it saves `logcat -b all` and the properties to `/data/local/tmp/starbite-bootguard.*`
   and restarts into recovery. A hang on the boot animation therefore always comes back without the buttons
   (`androidboot.init_fatal_reboot_target=recovery` in the cmdline covers init and critical-service crashes).
5. **First boot straight to the home screen, natively** (2026-11-07): `/system/etc/init/starbite-firstboot.rc`
   starts `/system/etc/starbite/firstboot.sh` (shell user and domain, like the boot guard, which ran on the
   phone) at `sys.boot_completed=1`: provisioned + setup complete, Samsung's and Google's wizards and the OMC
   agent disabled for user 0, home started; it does nothing once setup is complete. For an already built
   work_dir: `tools/android/oneui/postfix-system.sh` (VM) applies the mod's missing parts and rebuilds
   system.img.

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

## 4. UFS layout (`starbite-oneui`, on S9 #1 since 2026-11-07)

USERDATA (iOS slot + OS store) sits right after SYSTEM .. CP_DEBUG and is never moved. One UI's EROFS images
(build of 2026-11-07): **system 6419857408 B (5.98 GiB)**, vendor 697573376 B (665 MiB, after
postfix-vendor.sh), odm 36864 B. System does not fit the S9's SYSTEM (4.75 GiB), so:

| # | Name | Blocks (4 KiB) | Size | Holds |
|---|---|---|---|---|
| 18 | SYSTEM | 68736..1678719 | 6.14 GiB | One UI system (EROFS); grew over the old VENDOR + ODM |
| 19 | VENDOR | 12512000..12774143 | 1 GiB | One UI vendor (EROFS), behind WINDOWS |
| 20 | ODM | 12774144..12790527 | 64 MiB | One UI odm (EROFS) |
| 21-25 | CACHE .. USERDATA | unchanged | | CACHE = One UI /cache (ext4, 12 MiB) |
| 26-27 | WINESP, WINDOWS | 6613760..12511999 | 0.5 + 22 GiB | NTFS 23622320128 B (17.2 GB used, 6.4 GB free) |
| 28 | A16DATA | 12790528..15615994 | 10.78 GiB | One UI /data (ext4, 2825462 blocks: 5 short of the partition, `length=-20480`) |

VENDOR and ODM keep their names (the first-stage fstab in `android.dtb` finds them by name) and their entry
numbers, so no other partition is renumbered: the GPT lists them out of block order, which
`ufslayout.validate` now allows (overlaps are still refused). Written with `build/oneui-gpt.py starbite-oneui
--write` (the installer's gpt.py; both original copies in backup/20261106-oneui/gpt/), then a TWRP restart.

Images: `tools/android/oneui/sparse2raw.py IMAGE | tools/android/pixel/part-write.py NAME - --size N`
(64 MiB chunks read back on the phone + a whole-range SHA-256). System sha256 7662390c..., vendor 5572b8b7...
on the phone. The rebuild with the firstboot service (postfix-system.sh, 2026-11-07 08:45) is 6419861504 B,
sha256 dedb499c...; it is for fresh installs (the installer) and is not on S9 #1, whose setup was finished by hand.

**Windows at 18 GiB was too small (2026-11-07 morning):** the shrink left it 1.4 GB free; after two chkdsk runs it
hung at the Welcome screen. WINDOWS went to 22 GiB (VENDOR/ODM rewritten 4 GiB further on, A16DATA re-created at
10.78 GiB) and `ntfsresize` grew the NTFS to fill it (it resets the journal and schedules one more chkdsk).

## 5. Status (2026-11-07, 05:20): One UI 8 boots on S9 #1

- Build: UN1CA 3.2.1 (`ro.unica.version` 1.5 stable), S22 base `S901BXXSOGZH3`, DS-ACK 4.9.337 KSU kernel,
  SELinux enforcing. Three build fixes on the arm64 VM (patchelf, the signapk stand-in, CERT.SF naming) and
  the vendor post-fix (section 2).
- First boot (`tools/store/boot-next.sh android`): boot completed in about 2.5 min, the setup wizard started,
  then One UI restarted once on its own (normal reboot, no wipe: Samsung's first-boot restart; bootstat records
  that boot as `reboot,factory_reset`). A normal reboot lands in the loader menu (no default OS), from where
  `sbfb.py boot android` (or `sbfb.py reboot recovery` for TWRP) works over USB.
- Second boot: home in about 50 s; `first-boot.sh` disabled both setup wizards and OMC agent, provisioned=1:
  One UI Home. Later boots go to the lock screen (swipe), no setup.
- Checked: /data = A16DATA ext4 (14 GB, 13 GB free); vendor/odm EROFS from sda19/sda20; USERDATA not mounted;
  system_server started once, empty crash buffer; Wi-Fi scans (10 networks); sensor hub sensors listed; boot
  guard ran (`/data/local/tmp/starbite-boot-ok`).
- Open: `ro.adb.secure=0` (UN1CA's vendor default: adb without authorization); setup skip is over adb, not yet
  native; One UI is not the menu's default OS; not tested yet: SIM/calls (never emergency), camera, audio,
  fingerprint, NFC, battery life; the installer does not yet install One UI on a stock phone.
