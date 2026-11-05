# Android 17, Pixel-style: Google's GSI on Starbite

Date: 2026-11-05. Written with no phone access. **[read]** = checked in files, images or tool output tonight;
**[web]** = checked online tonight; **[unconf]** = only the phone can show it.
Context: docs/ANDROID-PLAN.md (how Android boots here today), docs/STARBITE-MASTER-PLAN.md §2.2 / §2.7 /
§3.3 (UFS layout, re-layout couplings, Pure Android 17), docs/STARBITE-2-PLAN.md §3.2b.
Tools: `tools/android/pixel/` (every write tool has `--dry-run`; none touches a phone by itself).

## 0. Summary

| | |
|---|---|
| **Decision** | Google's official **`gsi_gms_arm64` DP11.261016.003** (Android 17 QPR3, security patch 2026-10-05, Google apps and Play) on the SYSTEM partition, over LineageOS 24's vendor, kernel and first-stage ramdisk, which stay. Fallbacks in §1.4 |
| **Data area** | **16 GiB** (16.001 GiB, 17.18 GB) on UFS. Phase 1 runs on today's 8 GiB A16DATA (no GPT change); Phase 2 grows it to 16 GiB by shrinking WINDOWS and editing GPT entries 27/28 only. 24 or 32 GB do not fit without moving iOS 16 off UFS (§2) |
| **What is deleted** | LineageOS 24 on SYSTEM (image kept), its /data on A16DATA (backed up to the card first), its /metadata on ODM (backed up). Phase 2 also shrinks Windows' free space from about 12 GiB to about 4 GiB (no Windows files deleted). Nothing else |
| **Offline checks passed** | SELinux: GSI + vendor policy compile with a current secilc at the 4.9 kernel's policy version 30. HALs: the GSI still speaks Keymaster 3.0, gralloc 2.0, gatekeeper 1.0, sensors 1.0, audio 7.x, OMX. 32-bit userspace present. Treble levels match (202604) |
| **Main risk** | The GSI's Google-signed network modules are four months newer than LineageOS's (374399999 vs 370399999) and may want BPF the 4.9 kernel lacks: a boot loop shows it within minutes, rollback is one SYSTEM write (§6) |

## 1. Option study

### 1.1 (a) Google's Android 17 GSI on LineageOS 24's vendor: **chosen**

Source: developer.android.com/topic/generic-system-image/releases **[web]**. Every candidate's `build.prop` was read
straight out of the zips over HTTP ranges, without downloading them (`tools/android/pixel/fetch-gsi.py --dry-run`
lists the members) **[read]**:

| Build | Channel | Patch | Zip sha256 (release page = what we got) | Size |
|---|---|---|---|---|
| **DP11.261016.003** (16708287) | **17 QPR3 "beta", codename REL, SDK 37.2** | **2026-10-05** | `3538dd75fa1887d197a7fb77c0bd093cdd3a6a329038df23a531a976405f53a2` | 2,110,507,651 B |
| CP41.260831.007.A3 | 17 QPR2 beta | 2026-09-05 (the page says October) | `9087c172e74524de3c619ddec36256e8d85bf4227145fb6e5abcd0780432a4e3` | 2,104,213,486 B |
| CP3A.260905.010 | 17 QPR1 stable | 2026-09-05 | `d55d64aaebc71b2a895d792c152cfc408841bfde38565b082f495b850a2ae3e1` | 2,076,347,889 B |
| aosp_arm64 DP11 (no Google apps) | 17 QPR3 | 2026-10-05 | `d735a3b11fe1b76a736f325f5500f7211167642fe2ed39dae0cf530d1cf51436` | — |

URL pattern: `https://dl.google.com/developers/android/cinnamonbun/images/gsi/<file name>`, chosen file
`gsi_gms_arm64-exp-DP11.261016.003-16708287-3538dd75.zip`.

**Why DP11:** it is the only GMS build whose patch level (2026-10-05) is not older than BOOT's header
(os_version 17.0.0, patch 2026-10). That header is the keymaster root of trust and must never change
(ANDROID-PLAN §1.1 invariant). A system older than it is the one keystore failure mode we can avoid for free.
Its codename is REL (not a preview SDK), so apps treat it as plain Android 17.

**Blockers checked, with the result:**

| Question | Finding | |
|---|---|---|
| Treble / VNDK | Vendor `ro.board.api_level` 202604, `plat_sepolicy_vers` 202604; GSI `ro.llndk.api_level` 202604 and ships `mapping/202604.cil` and `plat_sepolicy_genfs_202604.cil`. VNDK is gone in 17 (LLNDK only), nothing to match | **[read]** ok |
| VINTF | Vendor target-level 7; the GSI carries compatibility_matrix.7/8/202404/202504/202604. Lineage's device framework matrix (Samsung, `vendor.lineage.*` HALs) is not in the GSI, and kernel 4.9 is below matrix 7's kernels: expect the "internal problem with your device" dialog. It is a warning, not a stop | ok / **[unconf]** dialog |
| Kernel 4.9, GKI | The GSI does not bring a kernel; ours stays. krazey90's kernel answers `uname` with 5.15.178 to `bpfloader`, `netbpfload`, `uprobestatsbpfl` and `netd` (root only), by process name, so it works for the GSI's processes as it does for Lineage's (kernel/sys.c, lineage-24.0) **[read]** | ok; BPF features: risk 1 |
| vendor_boot, dynamic partitions | Not used. First-stage init and fstab come from our store's `android.ramdisk` (Lineage's, Android 17); SYSTEM is a plain ext4 partition mounted at `/system` with `first_stage_mount`, which is system-as-root (switch root). The GSI image is a system-as-root image (ext4, `/` with `system/`, `init`, `apex` ...) **[read]** | ok |
| AVB / vbmeta | VENDOR's fstab has no `avb` flags; S-Boot verifies nothing past BOOT. The zip's `vbmeta.img` is not used | ok |
| Size, format | system.img 4,020,527,104 B, raw ext4 (not sparse, not EROFS), `shared_blocks` (read-only, never modified); SYSTEM is 5,100,273,664 B | ok |
| 64-bit only? | No: `abilist32=armeabi-v7a,armeabi`, 995 entries in /system/lib, `init.zygote64_32.rc` present. Needed: the vendor's camera provider and other HALs are 32-bit | ok |
| SELinux (the classic GSI killer) | `sepolicy-check.sh` replays init's split-policy compile (plat + system_ext + product + their 202604 mappings + genfs + the vendor's pub/vendor CIL) with secilc 3.11 at policy version 30: **compiles** (921,937 B). Controls: LineageOS's own system compiles too; an injected unknown type fails as it must. Six vendor-used public types are unknown to the GSI (`iso9660`, `udf`, `rfkilldisabled_prop`, `usb_uvc_config_prop`, `vendor_persist_nfc_prop`, `xtra_control_prop`): their rules compile but match nothing (NFC persistence, GPS xtra, rfkill props) | **[read]** ok; runtime denials: risk 2 |
| Enforcing | The GSI is a `user` build: SELinux is always enforcing, whatever the bootargs say. LineageOS 24 also runs enforcing on this vendor (krazey's thread, android-research.md) | ok |
| Keystore | Vendor HAL is Keymaster 3.0 (`android.hardware.keymaster@3.0-service`); the GSI's libkm_compat/libkeymaster4support still name keymaster@3.0 | **[read]** ok |
| Legacy HALs | GSI libui: mapper 2.0/3.0/4.0, allocator 2.0; gatekeeperd: gatekeeper 1.0; sensorservice: sensors 1.0/2.1; audio HAL 6.0/7.0/7.1/AIDL; OMX 1.0. The vendor ships mapper 2.0-impl-2.1, allocator 2.0, gatekeeper 1.0, sensors 1.0, KM 3.0 | **[read]** ok |
| Recovery overwrite | The vendor's `install-recovery.sh` runs only with `persist.vendor.recovery_update=true` (a Lineage setting the GSI never sets). The GSI's own recovery writers are checked by `gsi-facts.sh` after the fetch (step H2) | ok / H2 |
| Licence | GMS GSIs are for app compatibility testing, no redistribution, no modification. We never modify system.img (all fixes live in VENDOR) and never put it in an installer; **daily use goes beyond the licence** (master plan decision 6). The user accepts Google's terms on the release page | user's call |

**What Pixel-like means here:** Google's own system (Pixel's framework, Settings, SystemUI, Material You
"Wallpaper & style" ThemePicker, Quickstep gestures) with Google Play, Play services, the Google setup wizard,
Phone, Messages, Contacts, Chrome, Gmail, Maps, Photos, YouTube, Drive, Calendar, Clock, Gboard. The home screen
is AOSP's Launcher3 Quickstep as Google builds it for GSIs; the Pixel Launcher, Google Sans and At a Glance are
Pixel-only parts that are not in any GSI and cannot be added without modifying the image. **[read: the image's app list]**

What it loses compared with LineageOS 24: VoLTE/VoWiFi (krazey's ImsStack lives in his system image), the
Lineage extras (LiveDisplay, touch gestures, charging control: their `vendor.lineage.*` services lose their
system-side contexts), the Aperture camera app (Google's Camera2 instead). Play Integrity will fail (unlocked
bootloader). Register the phone at google.com/android/uncertified; use a throwaway Google account.

### 1.2 (b) A Pixel-like ROM built for starlte: **none exists**
PixelOS lists starlte as discontinued (last build Android 12, August 2022); PixelExperience is gone; crDroid's
and Evolution X's Android 17 device lists do not include the S9; the newest starlte builds of any of them are
Android 14 (crDroid, July 2024). The only maintained Android 17 for this phone is krazey90's LineageOS 24.
XDA lists unofficial "Pixel 10 Pro port" / "A37 port" threads for the S9: not official device support, not from a
trustworthy source, rejected. **[web]**
([pixelos.net/download/starlte](https://www.pixelos.net/download/starlte),
[thecustomrom.com Android 17 list](https://thecustomrom.com/guides/android-17-custom-roms),
[thecustomrom.com starlte](https://thecustomrom.com/devices/samsung/starlte))

### 1.3 (c) LineageOS 24 + Google apps + Pixel-style launcher: **fallback**
Known to boot (it is what runs today). krazey90 names MindTheGapps-17.0.0-arm64-20260914 as the optional Google
apps package for this build (android-research.md). Less Pixel-like (Lineage's Settings/SystemUI, Trebuchet), and
MindTheGapps is unlicensed redistribution of Google apps. It is the fallback if the GSI cannot be made to boot.

### 1.4 Fallback order
1. Same plan, `gsi_gms_arm64` CP3A.260905.010 (17 QPR1 stable) if something DP11-specific breaks. Its patch
   2026-09-05 is older than BOOT's 2026-10: watch keystore/vold in the logs.
2. `aosp_arm64` DP11 if a Google app or GMS itself is the problem (same framework, no Google apps).
3. LineageOS 24 back (§6) + MindTheGapps (1.3).

## 2. Space plan

### 2.1 What is on the UFS today (S9 #1's live GPT, backup/20261102-185306/sda-head-4M.img, parsed) **[read]**
UFS LU0 = 15,616,000 blocks of 4 KiB (59.57 GiB). Entries 1-24 are Samsung's and end at block 1,698,431.

| # | Name | 4 KiB blocks | Size | Holds |
|---|---|---|---|---|
| 18 / 19 / 20 | SYSTEM / VENDOR / ODM | 68736.. / 1313920.. / 1499520..1678719 | 4.75 GiB / 725 MiB / 700 MiB | Android 17 system, vendor, /metadata |
| 25 | USERDATA | 1698432..6613759 | 18.75 GiB | 0-16 GiB: **iOS 16.5 (an earlier port) slot**, the active Apple OS (APFS system + HFSX /var); 16 GiB-end: the **OS store** (capacity 721,024 blocks, about 0.2 GiB used) and the iOS herolog ring in the last 1025 blocks |
| 26 | WINESP | 6613760..6744831 | 512 MiB | Windows boot files |
| 27 | WINDOWS | 6744832..13518591 | 25.84 GiB | Windows 11, 13.86 GiB used (2026-10-25; re-measured by `phase2.sh check`) |
| 28 | A16DATA | 13518592..15615994 | 8.00 GiB | Android /data (f2fs) |

### 2.2 Where 16-32 GB could come from
- **Without a GPT change:** only A16DATA itself (8 GiB). USERDATA is fully used (iOS 16 + store), the store's free
  2.5 GiB is shared by every OS and far too small, and Windows' 12 GiB free space is inside its NTFS volume. A data
  image file inside NTFS or exFAT is not an option for Android's /data (the 4.9 kernel's NTFS is not a /data
  filesystem, FBE needs a real block device, and the card runs at 22.6 MB/s). A dm-linear concatenation of
  scattered pieces would hide Android data inside partitions other OSes think they own: rejected.
- **24 GB (22.35 GiB) or more:** needs WINDOWS at ≤ 11.5 GiB, below its 13.86 GiB of data, or the iOS 16 slot off
  UFS. iOS 16 runs an earlier port's kernel, which has no microSD driver, so its root cannot move to the card now
  (layout B of the master plan still waits for that). **Not possible today without deleting an OS.**
- **16 GiB:** shrink WINDOWS by exactly 8 GiB (25.84 → 17.84 GiB; Windows keeps about 4 GiB free) and let
  A16DATA start 8 GiB earlier. Only entries 27 (last LBA) and 28 (first LBA, type) change.

### 2.3 The proposal: Phase 1 at 8 GiB, Phase 2 at 16 GiB
**Phase 1 (no GPT change):** the GSI boots with a fresh 8 GiB A16DATA. It proves the system works and costs
nothing that cannot be rolled back by rewriting partitions.

**Phase 2 (GPT change, only after Phase 1 boots and with the user's yes to the Windows shrink):**

| # | Name | Before | After |
|---|---|---|---|
| 1-26 | Samsung's 1-24, USERDATA, WINESP | — | **byte-identical** (offsets, sizes, GUIDs, names, attributes) |
| 27 | WINDOWS | 6744832..13518591 (25.84 GiB) | **6744832..11421439 (17.840 GiB, 19,155,386,368 B)**; GUID kept |
| 28 | A16DATA | 13518592..15615994 (8.001 GiB) | **11421440..15615994 (16.001 GiB, 4,194,555 blocks, 17,180,897,280 B)**, 1 MiB aligned; GUID and name kept; type Microsoft basic data → **Linux filesystem data** (Windows then gives it no drive letter) |
| — | protective MBR, disk GUID 52444e41-…, header LBAs, backup GPT at 15615995..15615999 | — | unchanged; only the two header CRCs and the entry-array CRC change |

Generated from the 2026-11-02 backup into build/android/pixel/gpt-a16-16g/ by `gpt-a16grow.py gen` (refuses unless
the input is exactly layout A; checks byte by byte that nothing else changes), cross-checked with sgdisk
(`gpt-crosscheck.sh`: "No problems found" for the new and the restore set). **It is regenerated from a fresh backup
on the day** (B1); the 2026-11-02 set is a rehearsal:
`primary.bin` e98ab978…, `backup.bin` 7c2e0ab6…, restore set `primary-orig.bin` 7960f336…, `backup-orig.bin` 5f676eaa….

**Phase 2 impacts, explicitly:**
- Windows keeps all its files; its volume is shrunk with `ntfsresize` (rehearsed on a test volume) after an
  `ntfsclone` image to the card. Free space in Windows drops to about 4 GiB (exact number from `phase2.sh check`,
  which refuses if less than 2 GiB would remain). The Windows 10 VHDX plan (W5) needs layout B anyway.
- **Windows UFS driver write fence:** hard-coded to blocks 6613760..13518591 (master plan §2.7). After Phase 2 it
  still allows writes to 11421440..13518591, the first 8 GiB of the new A16DATA. Windows only writes inside its
  NTFS volume and no longer sees A16DATA as a basic data partition, but the safety net is gone there until the
  fence is re-patched to **6613760..11421439** (Windows owner's work: driver offsets 0x3ef0..0x3f18, re-sign,
  DriverStore). Until then: do not run disk tools in Windows.
- A16DATA is formatted again (fresh, empty). Do the real setup (Google sign-in) only after Phase 2.
- iOS 16, the store, USERDATA, WINESP, the card: untouched.

## 3. What is prepared (host only)

| Item | Where | sha256 / result |
|---|---|---|
| GSI system.img (DP11, from the zip above, verified) | re-fetched on the day into `build/android/pixel/gsi-gms-dp11/` (deleted tonight after the checks: the Mac fell to 1.4 GB free while other builds ran) | system.img `bddf5ad5a664e2f5d90d6e8cbe630ddbee3fd41ddb0711ebb5a4125c7e32d71c` (4,020,527,104 B, crc32 cd2e84fe); vbmeta.img b3281c52…; build.prop 8a870c6b… |
| Fresh f2fs /data, 8 GiB layout | build/android/pixel/a16data-8g.img.gz (37 MB) | raw `c4369d4e24758096f9d3f82186f6085afbb00f6af8c4907063c33391436127ad` (8,590,962,688 B) |
| Fresh f2fs /data, 16 GiB layout | build/android/pixel/a16data-16g.img.gz (75 MB) | raw `53180412852f73273f86974dfc6d85ba9bb24ecfddc2aa24d2beb64f562d162a` (17,180,897,280 B) |
| GPT sets (rehearsal, from 2026-11-02) | build/android/pixel/gpt-a16-16g/ | MANIFEST.txt |
| Static ntfsresize / ntfsclone / ntfsinfo / ntfsfix 2026.9.28 (aarch64, for TWRP) | build/android/pixel/ntfsprogs/ (source github.com/tuxera/ntfs-3g tag 2026.9.28, tarball 8a75293f…) | ntfsresize 1d9eb3d7…, ntfsclone c53d3c0a…, SHA256SUMS |
| Catalog with the new android version line | build/android/pixel/catalog-pixel.toml (from tools/store/catalog.toml; menu keys unchanged) | made again on the day from the live TOML |
| android.cmdline (store entry, ANDROID-PLAN H1) | tools/android/pixel/android.cmdline: `androidboot.init_fatal_reboot_target=recovery printk.devkmsg=on` | 2ca7c65a… |
| secilc 3.11 (SELinux Project release, signature good) | lima ~/android/pixel/secilc-3.11/secilc | libsepol 79f3d2c8…, secilc 1fb7de00… |
| VENDOR for the GSI | built on the day from a fresh VENDOR pull by build-vendor.sh (tested tonight on the 2026-10-27 image: e2fsck clean, labels set) | — |

Tools (tools/android/pixel/):

| Tool | What | Writes to the phone? |
|---|---|---|
| fetch-gsi.py | stream download: zip sha256 + per-member CRC, never stores the zip; `--dry-run` lists members | no |
| gsi-facts.sh, vendor-facts.sh, hal-compat.sh, apex-versions.sh | read-only facts about the images | no |
| build-secilc.sh, sepolicy-check.sh, sepolicy-unmapped.py | the offline SELinux replay | no |
| build-vendor.sh | VENDOR copy: microSD fstab line off (vold would offer to "format" the Starbite card), GSI boot logger (`starbite-log-gsi.rc` + a permissive vendor domain `starbite-log.cil`; the GSI has no usable su domain), labels set explicitly; `--dry-run` | no (image) |
| make-a16data.sh | fresh f2fs with today's flags | no (image) |
| preflight.sh | read-only checkpoint: identity, by-name map, mounts, SYSTEM = Lineage image, VENDOR hash, card, Mac space | no |
| part-backup.sh | partition → Mac or card (gzip on the phone), verified by sha256; `restore-card` for rollback | restore only |
| part-write.py | the only partition writer: SYSTEM/VENDOR/ODM/A16DATA allowlist, by-name → sdaN check, size, not mounted, 64 MiB chunks with read-back, zero chunks from /dev/zero, resume, whole-image hash at the end; `--dry-run`. Rehearsed against a fake phone (`rehearse-part-write.sh` + `fake-adb`: refusals, dry run, interrupted write + resume, stale data cleared) | yes |
| metadata-reset.sh | fresh /metadata on ODM with TWRP's own mke2fs (krazey's command); `--dry-run` | yes |
| catalog-pixel.py | changes only the android entry's `version` and `root` lines; `--back` restores them | no |
| gpt-a16grow.py, gpt-crosscheck.sh | Phase 2 GPT set + restore set, byte-checked, sgdisk cross-check | no |
| build-ntfsprogs.sh, ntfs-rehearse.sh | static NTFS tools, shrink rehearsal on a scratch volume | no |
| phase2.sh | Phase 2 steps: check, ntfs-backup, ntfs-shrink, gpt, verify, gpt-restore; `--dry-run` each | yes |

**tools/store/catalog.toml is not edited on this branch:** the tracked copy must equal the live store's catalog.
It changes in the same sitting as the store flash (A5) and is committed then.

## 4. Phone procedure (for the coordinator)

Rules for every step: phone in TWRP (Volume Up + Bixby + Power) unless a step says otherwise; run from the repo
root on the Mac; **every write is run first with `--dry-run`**, read, then for real; a step that prints STOP,
REFUSED or MISMATCH ends the session at that point (rollback §6 if a write had started). One storage-writing step
at a time (master plan rule 1.2.7). The user only presses buttons.

### Host steps (before the phone session)
- **H1. Space.** `df -h /` must show ≥ 9 GB free (4.0 GB image + 0.8 GB VENDOR work + the 4 GB margin). If not:
  `limactl shell starbite -- sudo fstrim -v /` (memory: lima-fstrim), then ask before deleting anything.
- **H2. Fetch and check the GSI** (2-5 minutes):
  ```
  python3 -I tools/android/pixel/fetch-gsi.py \
    https://dl.google.com/developers/android/cinnamonbun/images/gsi/gsi_gms_arm64-exp-DP11.261016.003-16708287-3538dd75.zip \
    3538dd75fa1887d197a7fb77c0bd093cdd3a6a329038df23a531a976405f53a2 build/android/pixel/gsi-gms-dp11
  shasum -a 256 build/android/pixel/gsi-gms-dp11/system.img      # bddf5ad5a664e2f5...
  limactl shell starbite -- bash tools/android/pixel/gsi-facts.sh build/android/pixel/gsi-gms-dp11/system.img | tail -4
  ```
  Checkpoint: zip sha256 matches (the script deletes everything otherwise), system.img hash as above, the three
  recovery writers `missing`.

### Session A: Phase 1, the GSI on 8 GiB (about 90 minutes)
- **A1. Preflight.** `tools/android/pixel/preflight.sh`. Must end "no STOP". A WARN on VENDOR's hash is expected
  if ANDROID-PLAN P1.5 was applied; a WARN on SYSTEM means it is not the LineageOS image: back it up whole in A2.
- **A2. Backups** (TAG = today's date, e.g. `20261106`):
  ```
  tools/backup-starlte.sh                                   # EFS, CPEFS, PARAM, ..., PERSISTENT, STEADY: fresh
  d=backup/$TAG-pixel; mkdir -p $d
  adb exec-out 'dd if=/dev/block/sda bs=4096 count=1024 2>/dev/null' > $d/sda-head-4M.img
  adb exec-out 'dd if=/dev/block/sda bs=4096 skip=15615995 count=5 2>/dev/null' > $d/sda-tail-gpt.img
  python3 -I tools/android/pixel/gpt-a16grow.py show $d/sda-head-4M.img $d/sda-tail-gpt.img   # must say layout A
  tools/android/pixel/part-backup.sh mac VENDOR $d
  tools/android/pixel/part-backup.sh mac ODM $d
  tools/android/pixel/part-backup.sh card A16DATA $TAG --dry-run && tools/android/pixel/part-backup.sh card A16DATA $TAG
  ```
  Checkpoint: every backup line says the two hashes match and is appended to build/android/pixel/backups.txt.
  SYSTEM needs no copy when A1 said it equals LineageOS's system.img (318f76ad…, kept in lima ~/android/z/).
- **A3. VENDOR for the GSI** (host, lima):
  ```
  gzip -dc $d/VENDOR.img.gz > build/android/pixel/vendor-live.img
  limactl shell starbite -- bash tools/android/pixel/build-vendor.sh build/android/pixel/vendor-live.img build/android/pixel/vendor-pixel.img --dry-run
  limactl shell starbite -- bash tools/android/pixel/build-vendor.sh build/android/pixel/vendor-live.img build/android/pixel/vendor-pixel.img
  limactl shell starbite -- env SECILC=/home/dev.guest/android/pixel/secilc-3.11/secilc \
    bash tools/android/pixel/sepolicy-check.sh build/android/pixel/gsi-gms-dp11/system.img build/android/pixel/vendor-pixel.img
  rm build/android/pixel/vendor-live.img
  ```
  Checkpoint: the dry run shows only the dwmmc2 line commented, the CIL block and one file_contexts line; the
  build ends "e2fsck -fn: clean" with every file `0644/0755 uid 0` and a label; the policy check says COMPILES.
- **A4. Unmount in TWRP:** `adb shell 'umount /data; umount /system_root; umount /system; umount /vendor; true'`,
  then `tools/android/pixel/preflight.sh` again: sda18/19/20/28 "not mounted".
- **A5. Store: android.cmdline + catalog line** (the ANDROID-PLAN H1 sequence, one sitting, nobody else on the store):
  ```
  tools/store/store-pull.sh build/store/ufs-store-live.img          # must not print STOP
  python3 -I tools/android/pixel/catalog-pixel.py tools/store/catalog.toml build/store/catalog-pixel.toml
  python3 tools/store/sbstore2.py build build/store/ufs-store-pixel.img --medium ufs \
      --import build/store/ufs-store-live.img --catalog build/store/catalog-pixel.toml \
      android.cmdline=tools/android/pixel/android.cmdline:text
  diff <(python3 tools/store/sbstore2.py list build/store/ufs-store-live.img) <(python3 tools/store/sbstore2.py list build/store/ufs-store-pixel.img)
  diff <(python3 tools/store/sbstore2.py catalog-keys build/store/ufs-store-live.img) <(python3 tools/store/sbstore2.py catalog-keys build/store/ufs-store-pixel.img)
  tools/store/store-flash.sh build/store/ufs-store-pixel.img
  cp build/store/catalog-pixel.toml tools/store/catalog.toml        # commit: the tracked copy = live
  ```
  Checkpoint: the list diff shows only `+ android.cmdline` and the catalog's new crc; the keys diff is empty.
- **A6. Writes** (each: `--dry-run`, read it, then the real run; times are estimates):
  ```
  python3 -I tools/android/pixel/part-write.py SYSTEM build/android/pixel/gsi-gms-dp11/system.img      # ~10 min
  python3 -I tools/android/pixel/part-write.py VENDOR build/android/pixel/vendor-pixel.img              # ~2 min
  tools/android/pixel/metadata-reset.sh                                                                  # seconds
  python3 -I tools/android/pixel/part-write.py A16DATA build/android/pixel/a16data-8g.img.gz             # ~3 min
  ```
  Checkpoint: each ends "written and verified, sha256 …" (SYSTEM bddf5ad5…, A16DATA c4369d4e…); metadata-reset
  ends with `LABEL="metadata" TYPE="ext4"`.
- **A7. The boot.** `adb reboot` → Starbite menu → **Android** (its line now reads "Google GSI 17 QPR3 · Play ·
  Linux 4.9"). Stopwatch. Expected: "LOADING ANDROID" → the GSI's boot animation → after 5-15 minutes (APEX
  decompression and first-boot dexopt) Google's setup wizard. Same outcome table as ANDROID-PLAN §3 P2, plus:

  | What you see | Meaning | Next |
  |---|---|---|
  | TWRP by itself within 2 minutes | init fatal (now reboots to recovery thanks to android.cmdline): SELinux, first stage, or a crashed critical service | collect-logs (below); `last_kmsg.txt` `init:` lines |
  | Boot animation restarting every 30-90 s | a critical process loops: netbpfload/netd (risk 1), keystore2, vold, surfaceflinger | forced reboot (Vol Down + Power 7 s) → TWRP → collect-logs |
  | "There's an internal problem with your device" | VINTF mismatch dialog | expected; tap OK |
  | Setup wizard | **booted** | Phase 1 test only: skip Wi-Fi and Google sign-in, reach the home screen, open Settings → About phone, then reboot to TWRP |
- **A8. Logs and checks.** In TWRP: `tools/android/collect-logs.sh gsi-try1` (our logger writes
  /metadata/starbite/boot0/{cmdline,dmesg,logcat}.txt). Then `tools/backup-starlte.sh` again and compare
  PERSISTENT, STEADY, MISC, EFS, RECOVERY with A2 (ANDROID-PLAN §3 P4 rules: EFS and CACHE may differ; RECOVERY
  and STEADY must not; PERSISTENT may, after the setup wizard). Boot iOS 16, Ventura, Windows and Linux once each
  from the menu (only SYSTEM/VENDOR/ODM/A16DATA and the store changed).

### Session B: Phase 2, A16DATA to 16 GiB (about 2 hours; only with the user's yes to shrinking Windows)
- **B0. Windows clean shutdown:** boot Windows from the menu, Start → Shut down (ntfsresize refuses an unclean or
  hibernated volume). Then TWRP.
- **B1. Fresh GPT backup and set** (A2's two `dd` lines into `backup/$TAG-p2/`), then:
  ```
  python3 -I tools/android/pixel/gpt-a16grow.py gen backup/$TAG-p2/sda-head-4M.img backup/$TAG-p2/sda-tail-gpt.img build/android/pixel/gpt-a16-16g
  limactl shell starbite -- bash tools/android/pixel/gpt-crosscheck.sh build/android/pixel/gpt-a16-16g
  ```
  Checkpoint: gen prints the before/after tables of §2.3; crosscheck says "No problems found" twice.
- **B2.** `tools/android/pixel/phase2.sh check` → "check passed" (GPT = orig set, WINDOWS not mounted, NTFS clean,
  ≥ 2 GiB would stay free). Write down the minimum size it prints.
- **B3.** `tools/android/pixel/phase2.sh ntfs-backup --dry-run`, then for real (about 14 GiB of used clusters,
  gzip on the phone: 20-40 minutes [est]). Checkpoint: `gzip -t` passes, file size and sha256 printed (note them).
- **B4.** `phase2.sh ntfs-shrink --dry-run`, then for real. Checkpoint: "Current volume size: 19155386368 bytes"
  (or one cluster less).
- **B5.** `phase2.sh gpt --dry-run`, then for real → "read back: new". `adb reboot recovery`.
- **B6.** `phase2.sh verify`: sda27 18706432 KiB, sda28 16778220 KiB; by-name links unchanged; ntfsfix -n clean.
- **B7.** `python3 -I tools/android/pixel/part-write.py A16DATA build/android/pixel/a16data-16g.img.gz` (dry run
  first) → 53180412…. TWRP: `adb shell df -h /data` after `mount /data` shows about 16 GiB (then umount).
- **B8.** Boot Windows once: it reaches the desktop; Disk Management shows C: at 17.8 GiB; nothing else.
  Then boot Android: a fresh setup wizard. **Now the real setup** (the user's checklist, §5).

## 5. Test checklist for the user (after B8, or after A7 if Phase 2 is skipped)
Do not test calls or SMS, and **never dial emergency numbers**. Never toggle "OEM unlocking". If Android offers to
"format" or "set up" an SD card, cancel (the card holds the other OSes). Use a throwaway Google account.

1. Setup wizard: language, Wi-Fi (connects, a page loads), skip or accept the copy-apps step, screen lock (PIN),
   fingerprint enrolment.
2. Google sign-in in the wizard (if Google blocks an uncertified device: finish without it, register the
   device's GSF ID at google.com/android/uncertified, wait, sign in from Settings).
3. Play Store: install one app; Play services shows no error.
4. Launcher: home screen, app drawer (swipe up), search bar, gesture navigation (back, home, recents) and the
   3-button option in Settings → System → Gestures.
5. Settings: About phone says Android 17, security patch 5 October 2026; Wallpaper & style changes the colours
   (Material You); brightness slider and auto-rotate; dark theme.
6. Sound (media volume, a YouTube video), Bluetooth headphones if at hand, camera photo (Google's Camera2),
   GPS fix in Maps, battery percentage and charging, power key sleep/wake, reboot back to the Starbite menu.
7. Tell us what looks or feels wrong (screenshots via power + volume down).

## 6. Rollback

| Situation | Do |
|---|---|
| GSI does not boot (Phase 1) | TWRP → `collect-logs.sh` → decide: fix in VENDOR and retry, or go back: `limactl shell starbite -- gzip -dc /home/dev.guest/android/z/system.img.gz \| python3 -I tools/android/pixel/part-write.py SYSTEM - --size 4561305600` (→ 318f76ad…); `gzip -dc $d/VENDOR.img.gz > v.img && part-write.py VENDOR v.img`; `part-write.py ODM` from `$d/ODM.img.gz`; `part-backup.sh restore-card A16DATA $TAG` (sha checked before and after); `catalog-pixel.py --back` + the A5 store sequence. LineageOS 24 is then exactly as before, with its data |
| Phase 2 stops before the GPT write | nothing to undo but the NTFS shrink, which Windows tolerates (volume smaller than partition); optional regrow: `ntfsresize --size 27745320960 /dev/block/by-name/WINDOWS` (dry run first) |
| Phase 2 GPT written, something is wrong | `phase2.sh gpt-restore --dry-run`, then for real → "read back: orig"; `adb reboot recovery`; `part-write.py A16DATA build/android/pixel/a16data-8g.img.gz`; optional NTFS regrow as above |
| Windows no longer boots after the shrink | TWRP: `gzip -dc /external_sd/starbite-backup/$TAG/WINDOWS.ntfsclone.gz \| /tmp/ntfsclone --restore-image --overwrite /dev/block/by-name/WINDOWS -` (with the GPT restored first, so the partition is its old size) |
| Anything touched a protected area | stop; compare with A2's backup-starlte.sh set; restore only that phone's own backup, from TWRP |

Never in any rollback: Odin/Heimdall by name (the PIT no longer matches past SYSTEM), BOOT, RECOVERY, DTB/DTBO,
EFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA, the boot LUNs.

## 7. Risks, ranked
1. **Network BPF** **[unconf]**: the GSI's Google-signed Tethering/Connectivity, Resolv and uprobestats modules are
   374399999 (October 2026); LineageOS 24's are 370399999 (June), the newest proven on this 4.9 kernel. A newer
   BPF program the backport cannot load makes netbpfload/netd fail and the boot loop. Logs: logcat `netbpfload`,
   `bpfloader`, `NetBpfLoad`. Fallback 1.4.1 (QPR1 stable, older modules) first.
2. **Runtime SELinux denials** **[unconf]**: the policy compiles, but vendor services labelled by LineageOS's
   system_ext contexts (service/property/hwservice) lose their labels under the GSI. Expected harmless for
   `vendor.lineage.*`; anything Samsung (RIL, camera, fingerprint) shows up as `avc: denied` in logcat. Fix in VENDOR
   (our own CIL and contexts files), never in system.img.
3. **Keystore / FBE** **[unconf]**: Keymaster 3.0 through km_compat on a 2023-02 vendor patch with a 2026-10 boot
   header; LineageOS runs the same framework code on it, so expected fine. If vold cannot create the CE key, the
   data is fresh: retry costs nothing.
4. **First-stage init with an old ramdisk:** Lineage's first-stage init (Android 17 CP2A) hands over to the GSI's
   second stage (17 QPR3). Same major version; a mismatch would show in last_kmsg before SELinux.
5. **VINTF dialog** at every boot (cosmetic). **No VoLTE** (2G/3G voice only, untested anyway).
6. **Phase 2:** ntfsresize on a real 26 GiB NTFS (rehearsed only on a scratch volume; ntfsclone backup first); the
   Windows driver fence gap (§2.3); a Windows that was not shut down cleanly (B0).
7. **Mac disk:** other builds took the Mac from 9.7 to 1.4 GB free tonight; H1 must hold or the GSI cannot be staged.
8. **Licence:** testing-only GMS GSI used daily (decision 6); never redistributed, never modified.
9. **Play Integrity / uncertified device:** some Google features and banking apps refuse; register the device.

## 8. Not done here
No phone access, no adb, no flashing, no edit of tools/store/catalog.toml (A5 does it with the flash), no change
to the loader, BOOT, RECOVERY, the kernel or the ramdisk (all unchanged: `android.kernel`, `android.ramdisk` stay).

## 9. In-place patches on the GSI's SYSTEM (tools/android/pixel/block-patch.py)
TWRP's 4.9 kernel cannot mount the GSI's ext4 read-write, so these are same-size binary patches written block
by block (each original 4 KiB block must match exactly once on the partition; written, read back). Re-apply
after any fresh GSI flash. (Decision 6 said "never modified": these two patches are the exception, local only.)

| File | Where (file offset) | Change | Why |
|---|---|---|---|
| `/system/lib64/libui.so` | 0x35890, 0x6159c | `b.gt` → `nop` | libui skips Gralloc2/3 when api level > 35; the vendor has mapper 2.1/3.0 only ("gralloc-mapper is missing") |
| `/system/lib/libui.so` | 0x373a2, 0x37330 | `bgt` (T1) → `nop` | same, 32-bit |
| `/system/bin/cameraserver` (libcameraservice is static) | 0x1202bc, block 288 → partition block 75522 | `cbz w27` (9b040034) → `b` (24000014) | `CameraService::addListenerHelper` fails when `IBinder::addFrozenStateChangeCallback` returns INVALID_OPERATION (-38): the 4.9 binder driver has no freeze notification. system_server's BiometricsCallbackHandler then dies ("Failed to register a camera service listener") and the boot animation never ends (attempt 5). Listeners now register without the frozen-state callback (only used to hold back callbacks to frozen apps). |

Originals: `build/android/pixel/libui-cmp/`, `build/android/pixel/cam/cameraserver-gsi` (sha256 4d812091…). The
attempt-4 workaround (`stop cameraserver` in starbite-log.rc) is removed: it never held (init restarted the service).

## 10. Decision 2026-11-05: back to LineageOS 24, made Pixel-style (live on S9 #1)
The GSI kept hitting 4.9-kernel incompatibilities (binder freeze notifications, ION, gralloc 2/3); the user chose
LineageOS 24 "as pure Android as possible, like the Pixel UI". Built by `tools/android/lpix/build-lpix.sh`
(the 2026-11-05 image was made by hand with exactly those steps; the script itself has not been re-run yet):
LineageOS 24's system.img + Google's own Android 17 GMS apps taken from the GSI (Play Services, Play Store,
Google setup wizard, Google app, Phone, Messages, Contacts, Photos, Chrome + Trichrome, Digital Wellbeing,
Speech Services, Calendar sync), LineageOS's duplicates and LineageSetupWizard removed, AOSP boot animation.
Gmail, Maps, YouTube, Drive come from Play (the 8 GiB /data would hold their updates twice otherwise).

| Partition | Content | sha256 |
|---|---|---|
| SYSTEM | Pixel-style LineageOS (VM: ~/android/lpix/system-pixel.img) | 74fc930e... |
| VENDOR | LineageOS 24's own (backup/20261105-pixel/VENDOR.img.gz) | 2693de7d... |
| ODM (/metadata) | fresh (metadata-reset.sh) | - |
| A16DATA | fresh f2fs (a16data-8g.img.gz) | c4369d4e... |

Fixes found on the phone: (1) GoogleConfigOverlay names com.google.android.ext.services (Google's mainline
ExtServices; LineageOS has com.android.extservices): system_server died in PackageManagerService, removed.
(2) Google WebView 149 + GoogleWebViewOverlay: "No WebView-library manifest flag" and the overlay hid
LineageOS's WebView 156: both removed, com.android.webview 156 is the provider. Result: boot_completed in ~30 s,
Google's setup wizard ("Hi there"), 0 system_server crashes. Dev: adb on from first boot, only this Mac's key
pre-authorized (DEV=0 in the script for an image without it). Not reverted: the cameraserver/libui GSI
patches (section 9) are gone with the GSI. Removed files are kept in the VM at ~/android/lpix/removed/, the GMS
source tree at ~/gsi-gms (the GSI itself is no longer on the phone).
