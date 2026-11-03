# Android on Starbite: first boot of Android 17, then GSIs from the card

Date: 2026-11-03. Written with no phone access: everything below comes from the loader source
(shim/starlte/xnu_boot.c, shim/fdt.c, sd.c, store.c), the store image (build/store/ufs-store.img),
the LineageOS 24 boot pieces (build/android/lineage/), the patched VENDOR image in the VM
(~/android/z/vendor.img), the kernel's embedded config (IKCONFIG), NOTES.md and
build/android-research.md. Facts are marked **[read]** (checked in those files), **[web]** (checked
online tonight) or **[unconf]** (needs the phone).

New tools for this plan (tools/android/, none touches a device by itself):

| File | sha256 | What |
|---|---|---|
| ramdisk-fstab.py | 7a826f8c… | list / cat / replace (`put`) / add files (`add`) and directories (`mkdir`) in a first-stage ramdisk (gzip newc cpio), byte-exact elsewhere (round trip of android.ramdisk is identical) |
| starbite-log.rc | 540951f5… | vendor init: kernel log + logcat of every boot to /metadata/starbite/boot0..4 |
| starbite-log.sh | 40f926b3… | the script those services run; both stop at `sys.boot_completed` (dmesg capped at 4 MiB / 10 min, logcat at 3 x 2 MiB, ends 60 s after boot_completed; oldest boots pruned while /metadata has < 32 MiB free) |
| collect-logs.sh | 040a9475… | host: pulls last_kmsg, pstore, sec_log, BCB, /metadata/starbite, CACHE recovery log, blkid from TWRP; ODM and CACHE mounted `-o ro,noload` and unmounted again |
| ../store/store-pull.sh | 83a7620d… | host: reads the phone's live store back as one image (read-only), lists it, compares its catalog with tools/store/catalog.toml |
| fstab.card-a16 | cec8ae38… | the fstab for card slot 1 (section 5) |

---

## 1. What is on the phone now (Android 17 = LineageOS 24.0, krazey90 20261025)

| Piece | Where | State |
|---|---|---|
| Kernel | UFS store `android.kernel` 38,334,560 B, crc32 be72b6c2, sha256 5d571d14… | Linux 4.9.337, clang 22, text_offset 0x80000, image_size 0x2f6a000 (49.6 MB in RAM) **[read]** |
| First-stage ramdisk | UFS store `android.ramdisk` 1,604,571 B, crc32 ca2ef41d, sha256 ba113e05… | gzip newc cpio: `init` + fstab (only the /data line changed: A16DATA) **[read]** |
| Command line | `android.cmdline` is listed in the catalog but **not in the store** | so the kernel gets S-Boot's bootargs only |
| Device tree | S-Boot's FDT = LineageOS's DTBH (BOOT is packed on Lineage's header + dt) | bootargs template: `console=ram root=/dev/ram0 … androidboot.hardware=samsungexynos9810 androidboot.selinux=permissive androidboot.debug_level=18760 … firmware_class.path=/vendor/firmware`; no `/firmware/android` node **[read]** |
| SYSTEM sda18 / VENDOR sda19 | raw ext4 from the zip | VENDOR: /data = A16DATA in fstab, A16DATA labelled userdata_block_device **[read]** |
| ODM sda20 | ext4 "metadata" = Android 17's /metadata | |
| A16DATA sda28 | 8 GiB f2fs from mkfs.f2fs in lima | |
| RECOVERY | TWRP 3.7.0 with /data = A16DATA | |

### 1.1 The boot path, read from the code
`boot_android()` (xnu_boot.c:727): kernel to 0x92080000, ramdisk to 0xA8000000, S-Boot's FDT copied
to 0xAD000000 (bootargs appended only if a cmdline exists), `/chosen linux,initrd-start/end` set
(4- or 8-byte cells both handled, fdt.c chosen_prop), DECON trigger restored, then the common
handoff (xnu_boot.S): D-cache cleaned by set/way, MMU/caches off, x0 = FDT, x1-x3 = 0, EL1.

Checked and **fine** (not a reason for a failure):
- Memory: the kernel (0x92080000..0x94FEA000), ramdisk and FDT sit outside every fixed
  reserved-memory region of the Lineage DT (lowest fixed ones: zswap_debug 0x91300000,
  rkp_region 0xAF800000, tima 0xB8000000). Dynamic CMA regions (camera, video_stream, crypto)
  are allocated after arm64_memblock_init has reserved kernel, initrd and FDT. **[read]**
- RKP/uH: the kernel has `# CONFIG_UH is not set`, so no hypervisor calls; KASLR is built in but
  needs a kaslr-seed, so placement is deterministic. **[read]**
- Ramdisk format: gzip, `CONFIG_RD_GZIP=y`. f2fs features the lima mkfs may have used (casefold,
  encrypt, extra_attr, project_quota, quota_ino, sb_checksum, compression…) are all in the kernel;
  `CONFIG_UNICODE=y`, `CONFIG_F2FS_FS_ENCRYPTION=y`, `CONFIG_FS_ENCRYPTION_INLINE_CRYPT=y`. **[read]**
- Keymaster root of trust: BOOT's header still carries Lineage's os_version 17.0.0 / patch 2026-10
  (checked on out/xnu-boot-uc3.img and backup/20261102-185306/BOOT.img), so FBE keys made on the
  first boot stay valid. **Invariant: every future BOOT must keep that header** (a lower
  patch level makes keymaster refuse the keys and Android asks to wipe /data). **[read]**
- **Open conflict with the card slots (section 4/5):** BOOT is shared, so every Android the loader
  starts (LineageOS 23.2 on the card, later GSIs) sees BOOT's os_version 17.0.0 / patch 2026-10
  through S-Boot's keymaster root of trust, while its own system reports Android 16 and an older
  `ro.build.version.security_patch`. Keymaster/keystore compares the two: a system older than the
  boot header can make key generation or the version check fail, so FBE on p9 may refuse to set up
  or ask for a wipe **[unconf]**. Options, in order: (a) first boot G1 with a fresh p9 and read
  `keystore`/`vold` lines in logcat (a fresh /data costs nothing there); (b) give the card system a
  `ro.build.version.security_patch` (and vendor patch) not older than BOOT's on the host, which is
  cosmetic but may satisfy the check; (c) run the card data without FBE (fstab `fileencryption`
  removed) as a test-only fallback. Changing BOOT's header is **not** an option: it would break
  Android 17's keys (the invariant above).
- Same handoff as Linux (works), with the MMU off and caches cleaned.

### 1.2 Why it may still not boot (ranked)

1. **S-Boot's real bootargs are unknown.** The DTB template has the right `androidboot.hardware`,
   but S-Boot rewrites bootargs. Watch for: `skip_initramfs` (the kernel has the Samsung
   `skip_initramfs` param: it would ignore our ramdisk and try `root=` instead → panic "VFS: unable
   to mount root"), a missing `androidboot.hardware=samsungexynos9810` (no fstab → first stage
   fatal), `androidboot.selinux` gone (enforcing with unlabeled things → denials). Engineer's
   "Kernel command lines" screen shows them: step P0 below.
2. **Which DT does S-Boot pass?** NOTES say BOOT carries Lineage's DTBH and other OSes still boot,
   but nobody has looked at the FDT the loader receives. If S-Boot took the DTB partition (stock
   Samsung DT) there is a `/firmware/android/fstab` node (SYSTEM/VENDOR/ODM→/odm ro): first-stage
   init would then use it instead of the ramdisk fstab and mount ODM at /odm, never /metadata.
   Signature in last_kmsg: `init: [libfs_mgr] … /odm` and no `/metadata`. **[unconf]**
3. **No kernel output at all** (frozen "LOADING ANDROID" or black). The vendor DECON driver takes
   over the panel from S-Boot's state; the loader drew the menu and restores only the trigger
   register. If the screen stays black but last_kmsg shows a boot, the display handover is the
   issue, not the boot.
4. **/data mount**: lima's mkfs.f2fs is newer than the 4.9 backport; `check` runs the vendor
   fsck.f2fs, and a refused mount → `fs_mgr_mount_all` returns NEEDS_RECOVERY → init reboots to
   recovery with `--wipe_data`. TWRP 3.7.0 would then **wipe A16DATA** (its /data). Harmless for a
   first boot, but the cause must be read from last_kmsg before retrying.
5. **CACHE (sda21, 12 MiB)**: `wait,check` ext4 with no `nofail`. It was used for woa ESP tests
   earlier (restored from backup) **[unconf: current filesystem]**. A non-ext4 CACHE fails the
   early mount_all (non-fatal for boot, but recovery commands and logs then fail).
6. **vold and the 128 GB card**: VENDOR's fstab still has
   `/devices/platform/11500000.dwmmc2/mmc_host* auto auto defaults voldmanaged=sdcard:auto`.
   If the kernel has no filesystem called `exfat` (it has `CONFIG_SDFAT_FS`, `# exfat alias`
   **[unconf]**), Android shows "Unsupported SD card" with a **Format** button that would erase
   the card (Linux distros, store, backups). Remove that line before the first boot (P1.5).
7. **PERSISTENT and STEADY**: init.samsungexynos9810.rc symlinks both (/dev/block/persistent is
   the FRP block). Android's PersistentDataBlockService writes PERSISTENT during setup and when
   "OEM unlocking" is toggled. Both are on Starbite's never-touch list. Backups exist
   (backup/20261102-185306); compare after the first boot; **never toggle OEM unlocking**.
8. **Recovery side effects**: a wipe request (BCB in MISC) is executed by TWRP, which now only
   reaches A16DATA. The loader cannot intercept it (S-Boot boots RECOVERY directly).
9. Smaller: the first boot (dex2oat, FBE key generation) takes 5-10 minutes; the boot animation
   alone is not a hang before 10 minutes.

---

## 2. Before the phone session (host only)

H1. Optional but recommended: add `android.cmdline` to the UFS store (the catalog already names it):
```
androidboot.init_fatal_reboot_target=recovery printk.devkmsg=on
```
`init_fatal_reboot_target=recovery` turns an init fatal (default: reboot to bootloader = Download
mode on Samsung, no logs) into a reboot straight into TWRP with last_kmsg intact.
`printk.devkmsg=on` stops init's /dev/kmsg lines from being rate-limited.
Build it from a **fresh pull of the phone's store**, never from the Mac copy
`build/store/ufs-store.img`: other workflows flash the store (iOS overlay, linux.*, windows.uefi,
menu), and store-flash.sh replaces the whole store area, so a rebuild from a stale image silently
rolls their entries back. No `--catalog`: the live catalog already names android.cmdline and is
kept as it is.
```
printf 'androidboot.init_fatal_reboot_target=recovery printk.devkmsg=on' > build/android/android.cmdline
tools/store/store-pull.sh build/store/ufs-store-live.img          # TWRP, read-only; lists + CRC-checks
python3 tools/store/sbstore2.py build build/store/ufs-store-a17.img --medium ufs \
    --import build/store/ufs-store-live.img \
    android.cmdline=build/android/android.cmdline:text
diff <(python3 tools/store/sbstore2.py list build/store/ufs-store-live.img) \
     <(python3 tools/store/sbstore2.py list build/store/ufs-store-a17.img)   # only + android.cmdline
tools/store/store-flash.sh build/store/ufs-store-a17.img                 # TWRP; USERDATA+16 GiB, keeps settings
```
The diff must show exactly one added line (android.cmdline) and nothing removed or changed; any
other difference means the pull and the flash are not back to back, so pull again. Do the pull,
build and flash in one sitting, and tell whoever else uses the store that night (it is shared by
every OS). (tools/overlay/sbstore-pull.py reads only the v1 view of the store; store-pull.sh reads
the whole version 2 store.)

H2. Note the reference hashes: kernel 5d571d14…, ramdisk ba113e05… (store crc32 be72b6c2 /
ca2ef41d), VENDOR original `~/android/z/vendor.orig.img`, patched `~/android/z/vendor.img`.

---

## 3. First-boot test plan for Android 17 (one session, about 45 minutes)

The user only presses buttons; every command is run from the Mac.
Key combos: TWRP = hold Volume Up + Bixby + Power from off/reboot; forced reboot = hold
Volume Down + Power 7 s; leaving Upload mode = Volume Down + Power 7 s.

**P0. Look before booting (menu, no writes).** Boot to the menu, open Engineer:
- "Kernel command lines": photograph S-Boot's bootargs. Must contain
  `androidboot.hardware=samsungexynos9810`; must **not** contain `skip_initramfs`. Note
  `androidboot.debug_level` (0x4948 "IH" = panics go to Upload mode with the reason on screen;
  0x4f4c "OL" = silent reboot), `androidboot.selinux`, `androidboot.mode`/`bootmode`.
- "Android 17": store rows android.kernel + android.ramdisk (and .cmdline after H1); partition
  probe SYSTEM ext4, VENDOR ext4, ODM ext4 label metadata, A16DATA f2fs.
- Stop here if skip_initramfs is present or hardware is missing: the loader must append/override
  bootargs first (section 6, L2).

**P1. TWRP preparation** (`adb` in TWRP):
1. Baseline logs: `tools/android/collect-logs.sh baseline` (this is TWRP's own previous boot: the
   reference for what an empty result looks like).
2. Backups to the Mac (Android may write these): `adb exec-out 'dd if=/dev/block/sda19 bs=4M'` >
   VENDOR, and PERSISTENT (sda14), STEADY (sda16), MISC (sda15), CACHE (sda21), and a **fresh EFS**
   (`/dev/block/by-name/EFS`, plus SEC_EFS if present: `ls -l /dev/block/by-name`), taken now, not
   the 20261102 one (that predates any Android boot); sha256 each on both sides. EFS holds the
   IMEI and Android mounts it rw (`errors=panic`).
3. `blkid /dev/block/sda21` — if CACHE is not ext4: `mke2fs -t ext4 -b 4096 -L cache /dev/block/sda21`.
   Windows now uses WINESP, not CACHE, but tools/woa/ufs-test.sh still targets CACHE: after this
   mke2fs its saved `build/woa/cache-backup.img` holds the older filesystem, so a later
   `ufs-test.sh restore` would undo this step. Run ufs-test.sh only after re-taking its backup.
4. `fsck.f2fs --dry-run /dev/block/sda28` (really read-only; it must report no errors). TWRP
   3.7.0's fsck.f2fs is probably older than lima's mkfs.f2fs, which used casefold, compression and
   similar features: ignore "unknown feature" / unsupported-feature reports from it and never run
   it without `--dry-run` (a repair by the older tool could damage the newer filesystem).
5. VENDOR edits (no verity on VENDOR):
   ```
   adb shell 'mkdir -p /tmp/v && mount -t ext4 -o rw /dev/block/sda19 /tmp/v'
   adb push tools/android/starbite-log.rc /tmp/v/etc/init/starbite-log.rc
   adb push tools/android/starbite-log.sh /tmp/v/bin/starbite-log.sh
   adb shell 'cd /tmp/v && chmod 0644 etc/init/starbite-log.rc && chmod 0755 bin/starbite-log.sh &&
      chown 0:2000 bin/starbite-log.sh &&
      chcon u:object_r:vendor_configs_file:s0 etc/init/starbite-log.rc &&
      chcon u:object_r:vendor_file:s0 bin/starbite-log.sh &&
      ls -lZ etc/fstab.samsungexynos9810 &&
      sed -i "s|^/devices/platform/11500000.dwmmc2|#&|" etc/fstab.samsungexynos9810 &&
      chmod 0644 etc/fstab.samsungexynos9810 && chown 0:0 etc/fstab.samsungexynos9810 &&
      chcon u:object_r:vendor_configs_file:s0 etc/fstab.samsungexynos9810 &&
      ls -lZ etc/init/starbite-log.rc bin/starbite-log.sh etc/fstab.samsungexynos9810 &&
      grep -n dwmmc2 etc/fstab.samsungexynos9810'
   adb shell 'umount /tmp/v && sha256sum /dev/block/sda19'
   ```
   `sed -i` rewrites the file through a temp file and **has dropped the security.selinux xattr on
   this very file before** (NOTES.md 2026-10-27), so the label is set again explicitly with
   `chcon` and checked with `ls -lZ`: the fstab must show `0644 root root
   u:object_r:vendor_configs_file:s0` (the first `ls -lZ` shows what it was before; owner and label
   must match). An unlabeled fstab makes second-stage `mount_all` fail under SELinux.
6. Optionally pull ODM's `/metadata` listing: it should be empty except lost+found.

**P2. The boot.** `adb reboot` (TWRP → S-Boot → menu). Pick **Android** (Volume to move, Power to
select). Start a stopwatch. Watch for, in order: "LOADING ANDROID" + "KERNEL AT 92080000" on the
loader screen → screen change (kernel took DECON) → LineageOS boot animation → setup wizard.

| What you see | Meaning | Next |
|---|---|---|
| Menu again with a red line (-51..-59) | loader refused (store, FDT, initrd props) | photograph; the message names the step |
| Frozen on "LOADING ANDROID" > 2 min | kernel dead before the console, or DECON never taken over | forced reboot → straight to TWRP → collect-logs: last_kmsg empty = dead early; full = display handover |
| "UPLOAD MODE" screen | kernel panic (debug level high) | photograph the reason lines; Vol Down + Power 7 s, then TWRP |
| Download mode | init fatal without H1 (reboot to bootloader) | Vol Down + Power 7 s, then TWRP |
| TWRP by itself | init fatal with H1, or Android asked recovery (BCB) | do **not** touch TWRP's screen; collect-logs (BCB shows a wipe request) |
| Boot animation > 10 min | framework loop | forced reboot → TWRP → collect-logs (logcat in /metadata/starbite) |
| Setup wizard | **booted** | skip Wi-Fi, no Google; then reboot to TWRP for logs |

**P3. Logs:** `tools/android/collect-logs.sh try1` → build/android/logs/<date>-try1/. Read in this order:
`last_kmsg.txt` (first stage: `init: First stage mount`, `switch_root`, `fs_mgr`, `SELinux`), then
`pstore-console-ramoops*`, then `metadata-starbite/boot0/{cmdline.txt,dmesg.txt,logcat.txt}`,
`misc-bcb.txt`. `cmdline.txt` is the authoritative final command line.

**P4. After a successful boot:** compare PERSISTENT, STEADY, MISC, CACHE, EFS with the P1.2 backups;
then boot iOS, Windows and Linux once each from the menu (Android shares EFS/MISC/CACHE with them).
EFS is **expected** to differ: the Samsung RIL writes it during normal operation (nv data, logs,
time stamps), so an EFS diff alone is not an alarm. Alarm only if the IMEI is gone (`*#06#`),
files under EFS are missing, or another OS loses its modem afterwards; then restore the P1.2 EFS
backup from TWRP. PERSISTENT and STEADY should be unchanged unless setup wrote the FRP block.

---

## 4. GSIs: how they can boot here

### 4.1 Constraints found tonight
- Layout: non-A/B, **no super / dynamic partitions**, but every Android 10+ system image is a
  root image and Lineage's first-stage init already does system-as-root (mount `/system`,
  switch_root). An A/B-style ("b", system-as-root) GSI image can be mounted by the same first
  stage. **[read]** (ramdisk = `init` + fstab only)
- **Treble direction: system must be the same Android version as the vendor or newer.** VENDOR is
  built on Android 17 (`ro.board.api_level=202604`, VINTF target-level 7, Lineage-built HALs
  linked against Android 17's LLNDK) **[read]**. An Android 14/15/16 GSI on this VENDOR is the
  unsupported direction. So each Android version gets **its own vendor + kernel + first-stage
  ramdisk** from the matching LineageOS build (vendor blobs, Mali userspace, camera and kernel
  are a matched set).
- **Kernel 4.9**: Android 15+ bpf/netd and other mainline modules want 4.19+; LineageOS carries
  4.9 workarounds, stock AOSP/Google GSIs do not. → the first GSI must be **LineageOS-based**.
- The vendor is **zygote64_32** with 32-bit vendor services (camera provider `_32`) **[read]**:
  the GSI must carry 32-bit libraries (no "64-bit only" GSIs).
- **ext4, not EROFS**: the 4.9 backport has EROFS, but the GSI builders state EROFS needs 5.4+ and
  our fstab is ext4. **[web]**
- **/data cannot be shared**: the second-stage `mount_all /vendor/etc/fstab.${ro.hardware}` reads
  /data's device from VENDOR, so each Android needs its own vendor copy whose fstab names its own
  data partition. That is why each "card slot" carries vendor + odm + metadata + data.
- The card is **MBR** (p1 exFAT 8192..182886399, p2 0xDA raw store 182886400..249999359, the whole
  card) **[read: NOTES 1962, sd.c sd_store_area]**. MBR partitions have no names, so no
  `/dev/block/by-name` links: the fstab uses plain nodes `/dev/block/mmcblk0pN` (first-stage init
  creates any block node it waits for by device name; only the by-name symlinks are limited to
  `androidboot.boot_devices`). **[unconf: test in slot 1]**. Fallback: hybrid GPT (named
  partitions + the 0xDA MBR entry the loader reads) and `androidboot.boot_devices=11120000.ufs,11500000.dwmmc2`.
- `CONFIG_MMC_BLOCK_MINORS=8`: p8 and up use extended dev_t (mmc sets GENHD_FL_EXT_DEVT) — fine,
  but worth seeing in /proc/partitions once.
- DSU (Dynamic System Updates) needs /metadata (we have it) and installs the GSI into /data
  (A16DATA, 8 GiB: too small for system + userdata today) and, as far as known, dynamic partitions
  **[unconf]**. Not the main path; a cheap experiment once Android 17 boots.

### 4.2 The first GSI: LineageOS 23.2 GSI (Android 16), VANILLA, ext4, on LineageOS 23.2's vendor
- GSI: MisterZtr/LineageOS_gsi `v2026.05.24-lineage23.2`, `LineageOS-23.2-20260524-VANILLA-EXT4-GSI.7z`
  (0.9 GB packed, about 2.5 GB raw, ext4, rw) **[web]**. LineageOS-based, so it carries Lineage's
  legacy-kernel patches; the release notes ask for A/B (= system-as-root) devices, which we are
  for first-stage purposes.
- Vendor / kernel / ramdisk: krazey90's `lineage-23.2-20260912-unofficial-starlte.zip`
  (1,090,122,438 B, md5 1438efb6…, Android 16, the same 4.9.337 tree, header v0 boot.img; ODM
  image mounted at /odm, no /metadata) **[read: android-research.md]**.
- Why not an Android 17 GSI: none found tonight (AndyYan's newest is lineage-21; MisterZtr stops at
  23.2) **[web]**. Why not Google's Android 16 GSI: no 4.9 support, likely 64-bit only.
- Stage it in two steps on the same card slot, so only one thing changes at a time:
  **G1 = the whole LineageOS 23.2 (its own system) from the card** (proves the card boot path with a
  matched system), then **G2 = swap p5 for the GSI** (and format p9).

### 4.3 Later slots
| Slot | System | Vendor/kernel | Note |
|---|---|---|---|
| 2 | LineageOS 22.2 GSI (Android 15, MisterZtr 2026.01.05) | krazey 22.x if still on images.krazey.de **[unconf]**, else official LineageOS 20 (Android 13, last official starlte) | GSI newer than vendor = supported direction |
| 3 | Android 14 GSI (LineageOS 21 AndyYan, 2026-04-11, vN) | LineageOS 20 official vendor + kernel | |
| 4 | microG | not a separate GSI: a VANILLA GSI with microG's system apps added on the host (needs signature spoofing; LineageOS 23.2 has the restricted microG spoofing) **[unconf]** | |

Each slot is about 17.5 GiB (system 4.5, vendor 0.7, odm 0.16, metadata 0.06, data 12). The card
holds one slot plus spare after section 5; more slots need the 87 GiB exFAT reorganised (backups
moved to the Mac first) — a later decision.

---

## 5. Exact steps for card slot 1

### 5.1 Card re-layout (shrink the store partition; exFAT untouched)
The loader takes the store's size from MBR entry 2 and refuses a header capacity above it
(store.c:71, sd.c:363); store-flash-sd.sh requires capacity = partition size. So the store is
rebuilt at 8 GiB **before** the MBR changes.

| # | Type | Sectors (512 B) | Size | Use |
|---|---|---|---|---|
| p1 | 0x07 | 8192..182886399 | 87.2 GiB | unchanged exFAT |
| p2 | 0xDA | 182886400..199663615 | 8 GiB | store, capacity 2097152 blocks |
| p3 | 0x05 extended | 199663616..249999359 | 24 GiB | container |
| p5 | 0x83 | 199665664 + 9437184 | 4.5 GiB | system (Lineage 23.2, later the GSI) |
| p6 | 0x83 | 209104896 + 1433600 | 700 MiB | vendor copy (23.2 + fstab.card-a16) |
| p7 | 0x83 | 210540544 + 327680 | 160 MiB | odm (23.2's odm image) |
| p8 | 0x83 | 210870272 + 131072 | 64 MiB | metadata (ext4, logs) |
| p9 | 0x83 | 211003392 + 25165824 | 12 GiB | data (f2fs) |
| — | | 236169216..249999359 | 6.6 GiB | free in p3 |

Type 0x83 keeps vold away (vold only mounts FAT/NTFS/exFAT MBR types). Steps (TWRP, card store
backed up first with `dd if=/dev/block/mmcblk0p2 bs=4M count=<used>`):
1. `tools/store/store-pull.sh build/store/card-store-live.img sd`, then `sbstore2.py list` it: all
   entries must end below 8 GiB.
2. Host: `sbstore2.py build card-store-8g.img --capacity 2097152 --import build/store/card-store-live.img`.
   From now on **every** card store build must pass `--capacity 2097152`: `sbstore2.py --medium sd`
   hard-codes the old p2 size (8389120 blocks), and store-flash-sd.sh would refuse such a build
   (capacity must equal the partition). Update `MEDIA['sd']` in sbstore2.py to 2097152 once the new
   MBR is on the card (not done here, the old layout is still live).
3. TWRP: write the new MBR with `sfdisk` (TWRP has it **[unconf]**; else write the 512-byte MBR +
   EBRs built on the host with Python and `dd`), `blockdev --rereadpt`, check `/proc/partitions`.
4. `tools/store/store-flash-sd.sh card-store-8g.img`; menu → Engineer → OS store: card store opens.

### 5.2 Images (lima; keep each step under ~5 GB new disk, delete as you go)
1. Download the 23.2 zip (1.09 GB) into its own empty dir, check md5 1438efb6….
2. `brotli -d` + `tools/android/sdat2img.py` → system.img (4,561,305,600 B), vendor.img (681,574,400 B),
   odm.img (157,286,400 B); `boot.img` → kernel, ramdisk (unpack like build/android/lineage).
3. Vendor copy for the card: replace `/etc/fstab.samsungexynos9810` with tools/android/fstab.card-a16
   and add to `/etc/selinux/vendor_file_contexts`:
   ```
   /dev/block/mmcblk0p5  u:object_r:system_block_device:s0
   /dev/block/mmcblk0p6  u:object_r:system_block_device:s0
   /dev/block/mmcblk0p7  u:object_r:system_block_device:s0
   /dev/block/mmcblk0p8  u:object_r:metadata_block_device:s0
   /dev/block/mmcblk0p9  u:object_r:userdata_block_device:s0
   ```
   Do it with `debugfs -w` (`rm` + `write` + `ea_set security.selinux`), then compare every other
   file's xattr with the original (the sed lesson from NOTES 2026-10-27). Add starbite-log.rc/.sh
   too (logs land on p8).
4. Ramdisk: list the 23.2 first-stage ramdisk first, because 23.2 has no /metadata (its ODM goes
   to /odm) and fstab.card-a16 has a `first_stage_mount` /metadata line, which needs the mount
   point in the ramdisk:
   ```
   python3 tools/android/ramdisk-fstab.py list ramdisk-23.2        # is there a 'metadata' directory?
   python3 tools/android/ramdisk-fstab.py put ramdisk-23.2 a16card-1.ramdisk fstab.samsungexynos9810=tools/android/fstab.card-a16
   python3 tools/android/ramdisk-fstab.py mkdir a16card-1.ramdisk a16card.ramdisk metadata   # only if it was missing
   ```
   (`put` replaces existing members only; `mkdir`/`add` append new ones.) If the fstab name in
   23.2's ramdisk differs, `put` refuses with the missing name: use the name `list` shows.
5. Store (UFS): add `a16.kernel` (23.2 kernel), `a16card.ramdisk`, `a16card.cmdline`
   (`androidboot.init_fatal_reboot_target=recovery printk.devkmsg=on`), plus the catalog entry below,
   the same way as H1: `store-pull.sh` a fresh live store, `cp tools/store/catalog.toml
   build/store/catalog-a16.toml`, append the a16-card `[[entry]]`, build with `--import` the pull
   and `--catalog build/store/catalog-a16.toml`, `diff` the two listings, then store-flash.sh.
6. Card writes from TWRP with sha256 read-back per partition: p5 ← system.img, p6 ← vendor copy,
   p7 ← odm.img, `mke2fs -t ext4 -b 4096 -L metadata /dev/block/mmcblk0p8`,
   `mkfs.f2fs -g android -O encrypt,quota,project_quota,extra_attr,casefold -C utf8 -l data /dev/block/mmcblk0p9`
   (from lima, written raw, as for A16DATA). **No UFS partition is written by name.**

### 5.3 Test G1, then G2
- G1: menu → Engineer → Start an OS → "Android 16 (card)". Same outcome table and
  `collect-logs.sh` as section 3, plus `mount -t ext4 -o ro,noload /dev/block/mmcblk0p8` for its
  /metadata logs (p8 is 64 MiB: starbite-log.sh prunes old boots below 32 MiB free).
  First boot from a 22.6 MB/s card: allow 15 minutes.
- G2: 7z-extract the GSI in lima, `simg2img` if sparse, `e2fsck -f` + `resize2fs` to 4.5 GiB, write
  p5, re-make p9 (mkfs.f2fs), boot the same entry. Expected trouble spots: VINTF/HAL mismatches
  (logcat `hwservicemanager`, `vintf`), Samsung RIL (`vendor.samsung.hardware.radio`), camera.

### 5.4 Proposed catalog entries (for tools/store/catalog.toml; not edited here)
```toml
[[entry]]
key = "android"            # unchanged; only add android.cmdline to the store (section 2, H1)
name = "Android 17"
version = "LineageOS 24.0 · Linux 4.9"
category = "android"
method = "android"
tag = "experimental"
files = ["android.kernel", "android.ramdisk", "android.cmdline"]
root = "ufs:SYSTEM + A16DATA"

[[entry]]
key = "a16-card"
name = "Android 16"
version = "card slot 1 · LineageOS 23.2"
category = "android"
method = "android"
tag = "experimental"
files = ["a16.kernel", "a16card.ramdisk", "a16card.cmdline"]
root = "sd:p5 system, p6 vendor, p9 data"
flags = ["hidden"]
```
For G2 only `version` changes ("card slot 1 · LineageOS 23.2 GSI"); the files stay the same.

---

## 6. Loader changes (proposals for the shim owner; shim/starlte is not edited here)
- **L1. Optional DTB per Android entry** (4th file, like Linux): today every Android gets S-Boot's
  FDT, i.e. LineageOS 24's DT, also for the 23.2 kernel. Load the entry's DTB to EDIT_FDT, then
  copy S-Boot's /memory, /chosen bootargs and serials over (or the reverse: S-Boot's FDT with the
  entry's nodes). Not needed for G1 if 23.2's and 24's DTs match (compare dt.img of both zips first).
- **L2. Bootargs filter for Android**: drop `skip_initramfs` and force
  `androidboot.hardware=samsungexynos9810` if P0 shows S-Boot misses them.
- **L3. Delete `/firmware/android`** from the FDT copy when present (cheap insurance for risk 2).
- **L4. Android result on screen next boot**: the loader already dumps latches; add the exynos-ss
  header magic and the last reset reason (INFORM registers) to Engineer, so a failed Android try
  is visible in the menu without TWRP.
- No loader change is needed for card slots: kernel, ramdisk and cmdline are ordinary UFS-store files.

## 7. Blockers and open questions
- No phone tonight: P0 (real bootargs, which DT) decides whether L2/L3 are needed before try 1.
- Disk: the 23.2 zip + GSI + unpacked images are ~7 GB inside the VM (whose disk never shrinks;
  the Mac has 13 GiB free). Not downloaded tonight; do it with streaming (brotli | sdat2img to the
  card writer) and delete per step.
- Card re-layout touches the shared card store: needs the other agents to be off the card store.
- [unconf] first-stage init with `/dev/block/mmcblk0pN` paths (fallback: hybrid GPT + boot_devices),
  TWRP's sfdisk, exfat alias in the 4.9 kernel, DSU without dynamic partitions.
