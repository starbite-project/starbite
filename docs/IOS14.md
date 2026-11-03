# iOS 14 on the S9 (xnu-7195)

STARBITE-2-PLAN.md section 6: iOS 14 is meant to become the stable, daily Apple OS. The userland
of an A9/A10/A11 iPhone is plain arm64 (no PAC), so it runs natively on all 8 cores with no
translator and no instruction emulation, and the root is an ordinary volume (no sealed system
volume, no cryptexes).

Status (2026-11-03): **the kernel builds; nothing has run on the phone yet.**

## 1. Kernel: xnu-7195.141.2 + the STARLTE series

- Source: `apple-oss-distributions/xnu` tag `xnu-7195.141.2` (macOS 11.5, the iOS 14.7
  kernel generation). The series is the iOS 16 STARLTE one (xnu-8796) ported back:
  `xnu/series-7195/` (165 patches + 3 staging scripts, notes in its README, dropped pieces in
  `SKIPPED.txt`).
- Port tree `build/xnu-7195` (branches `base-8796`, `port-7195`): `tools/port-7195-next.sh`
  (sets the tree up and replays), `tools/export-7195.sh`, `tools/fix-7195.sh`.
- Build: `tools/build-xnu-7195.sh` (darwin-xnu-build taught macOS 11.5, Xcode 15.4, about 3
  minutes incremental, about 10 fresh; the build tree `build/dxb-7195` takes about 2 GB).
  `FRESH=1` re-clones xnu and applies the series from scratch.
- Output: `out/kernel.development.starlte-7195`, Mach-O arm64 (not arm64e), "Darwin Kernel
  Version 20.6.0", 4 KiB pages, link address 0xfffffff007004000, classic local relocations (no
  chained fixups: `mkpayload.py` reports `fixups: 0`, nothing to rewrite at slide 0).

What the 7195 port had to add beyond the 8796 series (patches 0400-0403): a MASTER config
rebuilt from 7195's MacOSX one, stubs for pieces Apple left out of the public 7195 arm64
source (amcc_rorgn.c, PMAP_CS declarations in pmap.c, pmap_cs_allow_invalid,
ml_enable_monitor), the 8796 allocator names, the SWEET PRNG on 7195's Fortuna-shaped
cckprng context, the HID user client on the classic IOUserClient dispatch, the AMFI glue
reduced to its MAC policies (7195 reads XML entitlements itself), and two bank-2 fixes from
the 11417 series (0314, 0315).

## 2. Identity: iPhone 7 (iPhone9,1 / D10AP), iOS 14.8 (18H17)

- `tools/mkadt_starlte.py OUT --iphone7 build/ipsw-14/d10ap.adt --big`: the real D10AP device
  tree's /product node (fetched alone, 29 KB: `DEVICE=iPhone9,1 VERSION=14.8 BOARD=d10ap
  OUT=build/ipsw-14 PARTS=devicetree tools/fetch-ios-parts.sh`, then `pyimg4 im4p extract`),
  model iPhone9,1, target D10/D10AP, chip-id 0x8010, display scale 2, artwork subtype 1334.
  Canvas 750x1334 (375x667 points @2x) scaled up by the kernel
  (`sweetfb_scale=1 sweetfb_lw=750 sweetfb_lh=1334`).
- Why the iPhone 7 and not the iPhone X identity iOS 16 used: iOS 14's arm64 shared cache is
  one ARMv8.0 build for A9..A11, so either works for code; the plan names the 6s/7, and the
  D10's 16:9 canvas is the smaller one for the software renderer. `--iphone7` can be swapped
  for the default D22AP (iPhone X, 1125x2436) identity later without kernel changes.
- The kernel tells userland only the features both core types have (0304: the A55's ID
  registers masked with the Mongoose M3's: no LSE atomics, no FP16 advertised), so all 8 cores
  can run iOS 14 code with no emulation.

## 3. Payloads built (none flashed)

| file | what | sha256 |
|---|---|---|
| `out/kernel.development.starlte-7195` | the kernel (FRESH build from the series, 13,869,328 bytes) | `a3cb846f...` |
| `out/starlte-iphone7.adt` | iPhone 7 ADT, 8 CPUs described (new random seed per build) | `2cc1df24...` |
| `out/payload-ios14-k1.bin` | SWEETXNU payload, **kernel test**: RAM-disk root = one static test init (mockfs), `cpus=1`, `tools/cmdline/ios14-kernel-test.txt` | `14497dc6...` |
| `out/payload-ios14-u1.bin` | SWEETXNU payload, **userland**: `rd=ufsroot0 ufsroot=IOS14 herolog=0`, `cpus=8`, `tools/cmdline/ios14-userland.txt` | `a2f66dc2...` |
| `out/xnu-boot-ios14-k1.img`, `-u1.img` | the same payloads behind the old uc3 loader (BOOT images, for a lab bench only) | `a2387b8b...`, `3c88fcf1...` |

`tools/mkimage-ios14.sh NAME CMDLINE` builds them (`MDROOT=build/ios14/sweet-init` adds the
test init: `clang -target arm64-apple-ios14.0 -c ref/ios-herolte/tools/sweet-init.S` +
`ref/ios-herolte/tools/mkinit.py`).

## 4. Testing on the phone (for when the phone is free)

The kernel test touches no partition: its root is the RAM disk inside the payload.

1. Store entry, not BOOT. In TWRP, in one sitting, pull the phone's **live** store (never rebuild
   from the Mac copy `build/store/ufs-store.img`, which may be behind the phone), make a catalog
   that is the live catalog plus the `ios14-k1` entry, build, compare, flash:
   ```
   tools/store/store-pull.sh build/store/ufs-store-live.img    # read-only; must say the live catalog == catalog.toml, not STOP
   cp tools/store/catalog.toml build/store/catalog-ios14.toml
   # append the ios14-k1 [[entry]] from section 6 (key ios14-k1, files = ["ios14k1.xnu"],
   # flags = ["hidden"]) to build/store/catalog-ios14.toml; catalog.toml has no ios14 rows on purpose
   python3 tools/store/sbstore2.py build build/store/ufs-store-ios14.img --medium ufs \
       --catalog build/store/catalog-ios14.toml --import build/store/ufs-store-live.img \
       ios14k1.xnu=out/payload-ios14-k1.bin:xnu
   diff <(python3 tools/store/sbstore2.py list build/store/ufs-store-live.img) \
        <(python3 tools/store/sbstore2.py list build/store/ufs-store-ios14.img)
   diff <(python3 tools/store/sbstore2.py catalog-keys build/store/ufs-store-live.img) \
        <(python3 tools/store/sbstore2.py catalog-keys build/store/ufs-store-ios14.img)
   tools/store/store-flash.sh build/store/ufs-store-ios14.img
   ```
   The first diff must show only the changed `catalog` line and the added `ios14k1.xnu`; the second
   only `> ios14-k1`. `sbstore2.py build` refuses a `--catalog` that drops a live key. Without the
   catalog copy the store would carry ios14k1.xnu with no catalog row, and the menu entry would not
   exist. (The catalog holds 16 of 32 entries, so it fits. If store-pull.sh prints **STOP** (the
   live catalog differs from catalog.toml), append the entry to a copy of the TOML the live one was
   built from, not to catalog.toml.)
2. Reboot, Engineer > Start an OS > iOS 14 (kernel test).
3. Expected on screen (framebuffer console): XNU 7195 boot log, `SWEET PRNG: HMAC-SHA256
   registered`, `SWEET AMFI: platform binary policy registered (0)`, `SWEET Sandbox: permissive
   policy registered (0)` (format `%s (%d)`, patches 0035/0050; a failure reads `... NOT
   registered (err)` with the error number), the md0 root mounted through mockfs, then the test
   init's PID 1 lines.
   **Film the screen for the whole boot: it is the only evidence of the kernel test.** The
   persistent console log (herolog, patch 0206, `bsd/dev/herolte_ufs.c`; STARLTE builds the
   HEROLTE UFS driver, so 0047's `sweetlog` is not in play) is written only by the UFS root driver, to the last
   4 MiB + 4 KiB of the partition named by `ufsroot=`, and only when the kernel booted with
   `rd=ufsroot0` and a writable root. The k1 payload boots `rd=md0` with no `ufsroot`/`ufsrw`, so
   it never brings up UFS and writes **no log at all** (and the log is never on /pram). Running
   `tools/starlte-log-pull.py` after a k1 hang would return the previous iOS 26 boot's log from the
   tail of USERDATA and look like an iOS 14 result: do not use it for k1. A hang or panic: film or
   photograph the last lines, then forced reset (Volume Down + Power 7 s).
   For the **userland** payload (u1, `ufsroot=IOS14 ufsrw=1`) the log exists only once
   `herolog=0` is taken out of its cmdline (section 5 item 4); then it is at the tail of
   **IOS14**, not USERDATA: `python3 tools/starlte-log-pull.py adb build/ios14-u1-log.txt --part IOS14`
   from TWRP (compare the flush number with the previous pull to be sure it is a new boot).
4. Then `cpus=8` (edit the cmdline file, rebuild the payload) to bring up the A55 + M3 cores.
5. Userland (`payload-ios14-u1.bin`) only once a root exists (section 5, blockers).

## 5. Userland steps (not done: the root image needs disk space this Mac does not have now)

1. IPSW, but only its root image: iPhone 7 (iPhone9,1) iOS 14.8 18H17,
   `https://updates.cdn-apple.com/2021FallFCS/fullrestores/071-98010/39CE5096-991E-4C25-87BF-7FF9181C07CA/iPhone_4.7_P3_14.8_18H17_Restore.ipsw`
   (5,042,962,813 bytes, SHA-1 c9f684a45241ce37ba74196b5740d60d08586b26). Members needed:
   `018-62062-016.dmg` (root OS, APFS, 4,681,242,228 bytes) and
   `Firmware/018-62062-016.dmg.trustcache` (48 KB), fetched alone with
   `DEVICE=iPhone9,1 VERSION=14.8 BOARD=d10ap OUT=build/ipsw-14 PARTS="os devicetree"
   tools/fetch-ios-parts.sh` (remote zip ranges; no full IPSW). The iPhone 6s alternative:
   iPhone8,1 14.8 18H17, `.../071-97932/2C2C8127-289D-44D6-93D6-2BA03D0D6E0D/iPhone_4.7_14.8_18H17_Restore.ipsw`.
2. Root volume: `tools/build-ios-root.sh` with `IPSW=build/ipsw-14 OUT=build/root-ios14.img
   SIZE_MB=8192`; needs one change first: iOS 14 has no SystemOS/AppOS cryptexes, so the
   script (and `stage-ios-root.sh`) must accept an OS image alone. The 8796-era tuning
   (`herolte-tune-ios-root.sh`: launchd plist edits, disabled daemons) is the starting point;
   iOS 14's daemon set is close to iOS 16's.
3. Graphics: iOS 14 still has the OpenGL ES CoreAnimation path; the Mesa/GLES work from
   iOS 16 / iPadOS 18 (`tools/mesa-ios`, `build/mesa-ios`) is the route, not Metal.
4. **Partition size vs the log ring.** With `ufsroot=IOS14 ufsrw=1` the kernel writes its
   persistent log (the herolog ring: patch 0206, `bsd/dev/herolte_ufs.c`, magic HEROLOG1, flushed
   every 10 s; the boot-arg is `herolog`, not `sweetlog`, which only the SWEET board reads) into the
   **last 4 MiB + 4 KiB (1025 blocks of 4 KiB)** of IOS14 on every boot. If the HFSX volume fills
   the partition, that overwrites the volume's tail (the alternate volume header and the last
   allocation blocks). The primary fix is the margin: make the IOS14 partition at least
   4 MiB + 4 KiB larger than the HFSX volume (for example an 8 GiB volume in an 8 GiB + 8 MiB
   partition, or `SIZE_MB=8184` in an 8 GiB partition). Until that partition exists and is
   checked, `tools/cmdline/ios14-userland.txt` carries **`herolog=0`** (no log, nothing written
   past the volume). When the partition is created, check from TWRP, with the root image on it:
   ```
   adb exec-out 'dd if=/dev/block/by-name/IOS14 bs=512 skip=2 count=1 2>/dev/null' | python3 -c '
   import struct, sys; b = sys.stdin.buffer.read()
   assert b[:2] in (b"H+", b"HX"), b[:2]; bs, n = struct.unpack(">II", b[40:48]); print(bs * n)'
   adb shell 'echo $(( $(cat /sys/class/block/$(basename $(readlink -f /dev/block/by-name/IOS14))/size) * 512 - 1025 * 4096 ))'
   ```
   The first number (volume end, block_size × total_blocks) must be <= the second (partition
   bytes minus the ring). Only then remove `herolog=0` and rebuild u1. iOS 26 is safe only
   because its 16 GiB root sits inside a much larger USERDATA.
5. Disk: the OS image (4.7 GB) plus an 8 GiB sparse root plus its raw copy does not fit the
   ~12 GB free now; build it in pieces (DELETE_SOURCES=1) or after a cleanup.

## 6. Proposed catalog entries (not written into tools/store/catalog.toml)

```toml
[[entry]]
key = "ios14-k1"
name = "iOS 14 (kernel test)"
version = "XNU 7195 · RAM-disk init"
category = "apple"
method = "xnu"
tag = "lab"
files = ["ios14k1.xnu"]
root = "RAM disk in the payload"
flags = ["hidden"]

[[entry]]
key = "ios14"
name = "iOS 14"
version = "14.8 · XNU 7195 · iPhone 7"
category = "apple"
method = "xnu"
tag = "experimental"
files = ["ios14.xnu"]
root = "ufs:IOS14 · not allocated · herolog ring at tail"
flags = ["hidden"]
```

## 7. Blockers

- **No place for the root.** UFS is fully allocated (USERDATA = iOS 26's 16 GiB root + the
  store). The kernel mounts a whole GPT partition by name (`ufsroot=`); iOS 14 needs either the
  planned UFS re-layout (an 8 GiB IOS14 partition), the planned XNU microSD driver (root image
  on the card), or a `ufsroot=NAME@offset` extension in `bsd/dev/herolte_ufs.c` for a window
  inside a partition.
- **Store vs iOS 26 log ring (already in the tree).** store-flash.sh gives the UFS store a
  capacity that runs to the very end of USERDATA (USERDATA / 4 KiB - 16 GiB = 721024 blocks), and
  the iOS 26 kernel's herolog ring (patch 0206, `bsd/dev/herolte_ufs.c`) is the last 1025 blocks
  of USERDATA. Today's store uses about 82 MiB, far from it. Enforced now: store-flash.sh refuses a
  store whose used blocks exceed capacity - 1025, and `sbstore2.py build --medium ufs` refuses the
  same (the capacity itself is unchanged, so store-flash.sh's capacity check still matches).
- Untested on hardware: the 7195-specific changes (serial console table, GIC file from 8796,
  PRNG, commpage masks, stubs) are compile-checked only.
- Dropped for now (SKIPPED.txt): the fast-core scheduler placement (0238), the 8796-only root
  media hunks, the kmem-claims log.
- The `lldbmacros` in the dSYM are Python 2 (their checker is a no-op), so kernel debugging
  with lldb macros needs a Python 2 lldb or a port of the macros.
