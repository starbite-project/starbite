# macOS on the Galaxy S9: MAC0 (host groundwork for a first boot)

Status 2026-11-03. Nothing here was written to the phone. Every image and payload is on the Mac,
in `build/macos13/` and `out/` (both gitignored, Apple software stays local).

**Done means "the kernel starts macOS's launchd from a read-only root on the card".** The
payload for that test is `out/xnu-mac13-t0b.xnu`, with `build/macos13/root-t0.img` already in
the card's 64 MiB test range (section 8.1). Its first version (t0) hung with no log. The cause
of the missing log was found, and the fix is now kernel patch 0349 (in `out/` since 20:17). A second root with the full translated shared cache
(`build/macos13/root-full.img`, section 6.2) is the step after it, where launchd actually runs.

Contents: 1 version · 2 Ventura or 26.5 · 3 partial download · 4 instruction census ·
5 device identity · 6 the roots · 7 kernels · 8 first boot · 9 blockers · 10 sources ·
11 kernel side for 26.5 (emulation 0350-0352, census corrections, what macOS expects) ·
12 **the macOS 26.5 root and payloads** (`root-26t1.img`, `xnu-mac26-t1/m1.xnu`, the 26.5 census).

---

## 1. The version

The brief changed during the work (coordinator, 2026-11-03): from macOS 26.x to "any modern
macOS, preferably Ventura 13.6.x on the 8796 kernel we ported for iOS 16".

**Pinned: macOS 13.5 (22G74), the UniversalMac restore IPSW.**

```
https://updates.cdn-apple.com/2023SummerFCS/fullrestores/032-69606/D3E05CDF-E105-434C-A4A1-4E3DC7668DD0/UniversalMac_13.5_22G74_Restore.ipsw
12,893,195,726 bytes; listed by api.ipsw.me for Macmini9,1 (and every Apple-silicon Mac of 2020-2023)
```

The kernel strings, read from each IPSW's `kernelcache.release.mac13g` (fetched remotely with
`ipsw extract --kernel --remote`, about 25 MB each):

| Build | Kernel version string | Matches our source? |
|---|---|---|
| 13.5 22G74 | `Darwin Kernel Version 22.6.0: Wed Jul  5 22:22:52 PDT 2023; root:xnu-8796.141.3~6/RELEASE_ARM64_T8103` | **yes** |
| 13.5.2 22G91 | the same string, the same build stamp | yes (same kernel binary) |
| 13.6 22G120 | `Darwin Kernel Version 22.6.0: Fri Sep 15 13:41:30 PDT 2023; root:xnu-8796.141.3.700.8~1/...` | no: a security branch (CVE-2023-41992 kernel fix) whose source Apple never published |
| our 8796 build | `Darwin Kernel Version 22.6.0: ... dev:xnu-8796.141.3` (`out/kernel.development.starlte`) | — |

Why 13.5 and not "13.6.x":
- Apple's open-source releases stop at **macos-135** for Ventura (`distribution-macOS` tags
  macos-130 ... macos-135; no 136/137). macos-135's release.json lists **xnu-8796.141.3,
  dyld-1066.10, hfs-627.100.6, libpthread-514.100.2**. The 13.5 IPSW's `/usr/lib/dyld` says
  `PROJECT:dyld-1066.10`, so kernel, dyld and HFS sources all match one release.
- 13.6 has a different kernel (table above). 13.6.1 and later have **no restore IPSW** at all
  (api.ipsw.me lists 13.6 22G120 as the last Ventura IPSW); they exist only as OTA updates.
- Our KDK is `KDK_13.5_22G74` (`/Library/Developer/KDKs`, used by `tools/build-xnu-local.sh`).
- 13.5.2 (22G91) runs the same kernel with two later userland security fixes (ImageIO,
  BLASTPASS). It is a drop-in alternative (`IPSW_URL=` in the root builder); 13.5 is pinned
  because its dyld/HFS sources are the published ones.

Which IPSW members matter (BuildManifest, identity J313AP / J274AP "Customer Erase Install"):

| Manifest key | Member | Size | Zip storage |
|---|---|---|---|
| OS (the system volume) | `096-63007-081.dmg` | 7.48 GB (9.75 GB APFS) | stored (UDIF, LZFSE chunks) |
| Cryptex1,SystemOS (the shared caches) | `096-62994-081.dmg` | 4.43 GB raw APFS, 3.80 GB in the zip | **deflated** |
| Cryptex1,AppOS | `096-63034-082.dmg` | 18 MB | deflated |
| DeviceTree | `Firmware/all_flash/DeviceTree.j313ap.im4p` | 0.1 MB | — |
| KernelCache | `kernelcache.release.mac13g` (T8103) | 25 MB | — (not used: we boot our own kernel) |

None of the Ventura images are AEA-encrypted (macOS 26's are, section 2).

## 2. Ventura on 8796, or macOS 26.5 on 12377?

The coordinator asked to decide this on evidence, for the user's real goal: **"a modern macOS
draws the login screen"**. The hard part of drawing is WindowServer, which needs a Metal device
(§0.3 of the master plan).

| | macOS 13.5 on xnu-8796 (the iOS 16 series) | macOS 26.5 (25F71) on xnu-12377 (today's iOS 26 kernel) |
|---|---|---|
| Kernel source match | exact (13.5 = xnu-8796.141.3) | exact (26.5 = xnu-12377.121.6, `KDK_26.5_25F71` in `build/kdk`) |
| arm64e userland in the kernel | ported in this work, **not compiled** (`xnu/patches/pending-mac13/0311`) | done (0319, "STARLTE A64E: emulated ..."), proven by iOS 26 |
| Shared-cache slide of arm64e caches on 4 KiB pages | v3 stores plain pointers already; the 16 KiB-slide-page split (12377's 0321) still to check on 8796 | done (0320, 0321) |
| microSD root + pmsg log | ported, not compiled (0307-0310, 0312) | done and measured (0345-0348: 50 MHz, 23 MB/s) |
| M3 cores (LSE) | none (A55 only) | done (0342-0344) |
| **Metal on the Mali** | **nothing**: the 8796 series only has iOS 16's GLES path (sbca over Mesa); the StarbiteMetal accelerator (0336), GPU buffer/surface work (0326-0341) and the user-side metal2vulkan/PanVK plug-in exist only for 12377 + the iOS 26 Metal framework | **exists**: the same IOGPU/Metal generation as iOS 26 (both are 26.x); the plug-in needs a macOS build, not a new design |
| Shared cache size | **3.19 GiB** (2 files) | about **7 GB** [est: this Mac's macOS 27.2 cache is 7.2 GB in 84 files; 26.5's SystemOS cryptex is 6.17 GB still compressed] |
| Root image | 3.8 GiB: fits under 4 GB and the Mac's 6 GB budget | about 8 GB: **does not fit the Mac budget**. Needs the external drive (`/Volumes/Untitled`, 34 GB free) or the master drive |
| IPSW images | plain UDIF / raw APFS | AEA-encrypted (`094-56453-087.dmg.aea` 10.3 GB, `094-56679-089.dmg.aea` 6.2 GB): no random access; two streaming passes through `aea decrypt` (stdin/stdout) per image, about 33 GB of downloads |
| Instruction classes left after a64e-rewrite (section 4) | see section 4 | more (built for M1-M4, with SME/SVE kernels in Accelerate) |

**Decision: macOS 26.5 on the 12377 kernel is the faster road to a drawn login screen.**
Every kernel piece it needs is already in the kernel the phone boots daily, and the Metal stack
targets the same Metal generation. Ventura's road runs through a port of the whole GPU stack to
8796 *and* a rewrite of the Metal plug-in for Ventura's 2022 Metal framework. That is weeks of
work with no shortcut.

What I built is still Ventura, and that is not wasted:
- MAC0's test (the kernel starts macOS launchd from the card) is the same code path for both.
  It runs on the 12377 kernel today with the Ventura tier-0 root.
- Ventura's full root fits the Mac budget **now**: 3.8 GiB, with the whole translated cache. It
  shows how far a macOS userland gets on our kernel (launchd, its boot work, the first daemons)
  before the 8 GB 26.5 root can be built off the Mac.
- The tools are version-agnostic (`IPSW_URL=`, `OS_DMG=`, `SYS_DMG=`). For 26.5,
  `ipsw-apfs.py` needs one more input mode (an `aea decrypt` pipe in front of its deflated-member
  streaming), plus somewhere to put 8 GB.

**What the user must decide:** the external drive (or master drive) for the 26.5 root, about
8-9 GB. I did not write to `/Volumes/Untitled`: it holds the archived backups
(`build/ARCHIVED-TO-USB.txt`).

## 3. Partial download (no full IPSW)

`tools/macos/ipsw-apfs.py` reads single files out of an IPSW's disk images over HTTP range
requests (curl, Apple CDN hosts only):

- **zip**: the central directory from the tail (zip64), then the local header of one member.
- **stored members (the OS image, UDIF)**: the `koly` trailer and the `blkx` table of the
  `Apple_APFS` partition. Then only the LZFSE/zlib chunks under the APFS metadata and the wanted
  files are fetched, through `libcompression`.
- **APFS**: the container superblock (newest in the checkpoint area), the object maps and the fs
  B-tree. That includes sealed volumes: header-less nodes, and hashed index nodes whose child
  oids are stored **relative to the tree's root oid**. This was measured on the 13.5 cryptex,
  where child + 1838 = the real oid. Also the extent records, xattrs, symlinks, and **decmpfs**
  types 3/4 (zlib), 7/8 (LZVN, libcompression 0x900), 9/10 (stored), 11/12 (LZFSE) and 13/14
  (LZBITMAP). Inline, resource-fork and embedded-fork variants are all handled.
- **deflated members (the SystemOS cryptex)**: no random access. Pass 1 streams the member once
  and keeps the blocks that are APFS metadata (a valid Fletcher-64 object checksum, or the shape
  of a sealed volume's header-less node): 12,026 blocks, a 49 MB index
  (`build/macos13/systemos.idx`). Pass 2 streams again and decodes the wanted files as their
  blocks go by. A multi-GB compressed file is decoded 64 KiB chunk by chunk straight into the
  target file (`RsrcSink`), so it never sits in RAM or on disk twice.
- `--owners` writes Apple's uid/gid/mode per path, and `tools/macos/hfs-owners.py` applies them
  to the finished HFSX image offline. **No sudo anywhere.**
- `--thin` keeps the arm64e slice of universal binaries. Each slice keeps its own code signature
  (`codesign -v` passes on the extracted launchd and dyld).

What was fetched, and the disk it uses:

| What | Downloaded | On disk |
|---|---|---|
| BuildManifest, J313AP/J274AP device trees, 3 kernelcaches (kernelcaches deleted after reading) | about 100 MB | 9 MB + 0.6 MB |
| OS image: tier 0 (launchd, dyld, launchd.plist, Data template `private/`) | 30 MB of chunks | in `root-t0.img`, 64 MiB |
| OS image: full tier (`tools/macos/ventura-full.list`, 5,144 items, 508 MiB fat, thinned) | 1.53 GB of chunks (whole LZFSE chunks, runs with small gaps bridged: 253 requests) | in `root-full.img` |
| SystemOS cryptex: pass 1 + pass 2 (arm64e cache only, x86_64 cache skipped) | 2 x 3.63 GiB | 49 MB index + the cache inside `root-full.img` |
| **Total added on the Mac** | | **3.70 GiB (3.97 GB): `build/macos13` 3.6 GiB, `build/xnu-8796` 83 MiB, the three payloads 48 MiB, the ADT 12 KiB** (budget 6 GB) |

The Lima VM was trimmed once (`fstrim`: 914 MiB back to the host). Nothing went to the
external drive.

## 4. Instruction census

`tools/dsc-isa-census.py` now counts 50 instruction classes. For each class it records which S9
core lacks it and what handles it today: `rewrite` (tools/a64e/a64e-rewrite.py, ahead of time),
`trap` (the 12377 kernel's `sweet_a64e_emulate`, or the M3 LSE emulation), `none` (an
undefined-instruction crash if it runs), or `nop` (a hint). It counts only
`S_ATTR_PURE_INSTRUCTIONS` sections. **A static count is an upper bound on what runs**, because
much of this code sits behind runtime CPU-feature checks that the kernel answers for the A55.

Runs: Ventura 13.5's arm64e cache (`build/macos13/census-22G74.txt`, on Apple's bytes before the
rewrite) and this Mac's macOS 27.2 cache (`build/macos13/census-host-macos27.2-26B5112a.txt`,
the stand-in for 26.5 [est: 26.5 should be close to 27.2, with fewer M4/SME paths]). The iOS 26
column is the earlier census (`build/dsc-isa-census-ios26-n104.txt`), which had only the first
20 classes.

| Class | lacks | handled by | iOS 26.6.2 | **macOS 13.5** | macOS 27.2 |
|---|---|---|---|---|---|
| images / instructions | | | 4,305 / 783 M | **2,499 / 437 M (1,669 MiB)** | 4,113 / 921 M |
| RETAA/RETAB | both | rewrite | 5,562,251 | **3,591,127** | 6,823,444 |
| BRA*/BLRA* | both | rewrite | 10,694,728 | **4,960,509** | 8,673,261 |
| PAC/AUT/XPAC (register) | both | rewrite | 13,261,454 | **5,074,697** | 15,991,996 |
| LDRAA/LDRAB | both | rewrite (writeback: trap) | 1,650 | **7,753** (112 left to trap) | 22 |
| PACGA | both | trap | 687 | **598** | 767 |
| FJCVTZS | both | trap | 535 | **789** | 2,150 |
| FCADD / FCMLA | both | trap / **none** | 16,239 / 0 | **14 / 0** | 16,239 / 0 |
| LDAPR (RCpc) | M3 | rewrite to LDAR | 133,388 | **40,466** | 193,624 |
| LSE CAS / CASP / LD<op>,SWP | M3 | trap (M3) | 44,415 / 2,583 / 781,772 | **28,906 / 419 / 201,536** | 82,977 / 577 / 832,682 |
| DotProd, RDM | M3 | none (M3) | 994 | **354** | 23,000 |
| **LDAPUR/STLUR (LRCPC2, v8.4)** | both | **none** | 140 | **323** | 2,060 |
| **MRS/MSR DIT (register)** | both | **none** | — | **3,374** (3,117 in libcorecrypto: `mrs x8, DIT` / `msr DIT, x8` around every operation) | 97 |
| **MSR DIT/SSBS, #imm** | both | **none** | — | **0** | 2,834 |
| **SB** | both | **none** | — | **0** | 7 |
| RMIF (FlagM) / CFINV / SETF | both | none | — | **2** / 0 / 0 | 0 |
| **MRS/MSR S3_x_C15_x (Apple IMPDEF)** | both | none | — | **1,068** (e.g. libsystem_pthread `msr S3_6_C15_C1_5` = SPRR_PERM_EL0 and `S3_4_C15_C2_7` = APRR: the JIT write-protect toggles, behind the commpage APRR/SPRR flags) | 2,013 |
| **Apple AMX** (`0x00201000` space) | both | none (Apple only) | — | **185,041** (Accelerate: BLAS, BNNS, LAPACK, vDSP) | not counted |
| FMLAL/FMLSL (FHM) | both | none | — | **81,997** | 74,093 |
| BF16 (BFCVT/N, BFDOT, BFMLAL, BFMMLA) | both | none | — | **590** | 1,376 |
| I8MM (SMMLA/UMMLA/USMMLA, USDOT) | both | none | — | **2** | 32 |
| SHA3 (EOR3/BCAX/RAX1/XAR) / SHA512 | both | none | — | **912 / 1,154** (corecrypto 432) | 1,428 / 1,152 |
| SVE/SME encoding space | both | none | — | **5,151** | 161,054 |
| FRINT32/64, DC CVADP, WFET/WFIT, CNTxCTSS, SME SVCR | both | none | — | **0** | 0 |
| PAC hints (PACIBSP...), BTI | — | already NOPs | 11,671,309 | **6,103,208** | 13,724,408 |
| **sites in "none" classes (both cores)** | | | | **279,614** | 246,146 + AMX |

What a64e-rewrite did to the 13.5 cache in the image (2,496 images, libcorecrypto skipped):
`ret 3,587,909, br 2,206,857, blr 2,750,559, pac 5,073,061, ldra 7,641, ldapr 40,466`. Left for
the kernel: `ldra 112, pacga 598, fjcvtzs 789, fcadd 14` (all four already emulated by 0319).

Reading the table: **Ventura's cache is the smaller job.** It has half the PAC sites of 27.2,
almost no SVE, no SB and no `MSR DIT, #imm`. It is built for M1/M2, while 27.2 carries M4
SME/SVE kernels. Most "none" sites are in Accelerate (AMX, FHM, BF16), which picks its kernels by
CPU family and features. On our kernel those read as a non-Apple ARMv8.2 core, so these paths
should not run. **The ones that are probably unconditional are corecrypto's DIT save/restore**
(every Mac has DIT) **and LRCPC2**. Those two must be handled before a full macOS userland runs.

**What the translator (or the kernel) must add, by priority** (12377's `sweet_a64e.c`, or the
8796 copy in `pending-mac13/0311`):

| # | Class | Sites (13.5 / 27.2) | Why it matters | Fix | Effort |
|---|---|---|---|---|---|
| 1 | `MRS Xt, DIT` / `MSR DIT, Xt` (and `#imm`) | 3,374 reg (3,117 in corecrypto) / 2,931 | Every Apple-silicon Mac has DIT, so corecrypto may save/set/restore it unconditionally on macOS: a trap on the first hash or cipher. corecrypto is exactly the image a64e-rewrite must skip (its FIPS self-test MACs its own code) | trap: `MRS Xt, DIT` -> Xt = 0, `MSR DIT` -> NOP (DIT only guarantees data-independent timing) | **S** (1-2 h) |
| 2 | `SB` | 0 / 7 | speculation barrier | rewrite 1:1 to `ISB` (or `DSB SY`) | **S** |
| 3 | `LDAPUR*/STLUR*` (LRCPC2, v8.4) | 323 / 2,060 | compiler-generated acquire/release with an offset (atomics in structs); iOS 26's A13 cache had only 140 | rewrite `#0` offsets to LDAPR/STLR (then LDAR for the M3); trap-emulate the rest (copyin/copyout + DMB) | **S-M** (half a day) |
| 4 | `FMLAL/FMLSL` (FHM, v8.2 optional, M1 has it) | 81,997 / 74,093 | FP16 widening multiply-add: Accelerate, CoreML, CoreImage. May be baseline code on macOS | trap-emulate with FCVTL + FMLA in an asm helper (the A55 has FP16), or keep these libraries off the hot path | **M** (1 day) |
| 5 | `FCMLA` | 0 / 0 | complex multiply-add; FCADD is emulated already, FCMLA is not | extend the FCADD helper | **S** |
| 6 | `MRS/MSR S3_x_C15_*` (Apple IMPDEF: SPRR/APRR JIT toggles), Apple AMX | 1,068 / 2,013 / 185,041 | Apple-only registers and the AMX coprocessor, reached only when the code believes it runs on an Apple CPU (commpage CPU family, APRR/SPRR flags, `hw.optional.*`) | check that the commpage describes a non-Apple v8.2 core and no APRR/SPRR (NOTES: the features are the nibble-minimum of the A55/M3 ID registers). A trap is a crash by design | **S** to check, **R** if something uses them unconditionally |
| 7 | BF16, I8MM, SHA3, SHA512, SVE/SME space | see the table | M2+/M4 kernels in Accelerate and corecrypto, dispatched by CPU features | none, as long as the feature bits say "absent". Count again per image if a crash names one | **S** (watch) |
| 8 | M3-only: LSE, LDAPR, DotProd, RDM | 230,861 / 916,236 | as on iOS 26 | keep the userland on the A55s (`cpus=4`) first; 0342-0344 cover LSE on 12377 | — |

**Estimate:** items 1-3 and 5 are about one evening on 12377 (a64e-rewrite + `sweet_a64e.c`,
then the a64e test binary extended). Item 4 is one more. Item 6 is a check of the commpage bits
plus whatever the first crash logs show. That is about **2-3 sessions** before a macOS daemon
set can run without undefined-instruction crashes, versus iOS 26's 1. On the 26.5 cache
(proxy: 27.2), the same list with larger counts.

## 5. Device identity

**Chosen: MacBook Air (M1, 2020), J313AP / MacBookAir10,1, T8103, board-id 0x26.**
- It is in both IPSWs we may use (Ventura's and 26.5's UniversalMac lists Apple-silicon Macs
  from 2020 on).
- A battery-powered laptop with a built-in panel is what the phone is. CoreDisplay's
  "built-in display" path and powerd's battery logic fit better than a Mac mini's headless,
  external-display-only model.
- **J274AP (Macmini9,1, board-id 0x22) is the fallback**: no battery, lid, keyboard or trackpad
  expectations at all. It is one argument to the same tool, if laptop-only daemons misbehave in
  MAC2.

The kernel never sees a Mac platform expert. Our kernels match `SweetPlatformExpert` on
`samsung,a5xelte` in the root `compatible` and keep every S9 hardware node (CPUs, memory, GIC,
UFS, `/pram`, fbcon). There is no AppleARMPE, no kexts, no iBoot. So the identity is only what
userland reads: `hw.model`, `IOPlatformExpertDevice`, MobileGestalt, `/product`.

`tools/macos/mkadt-mac.py BASE.adt MAC.adt OUT.adt` does it the way the iPhone 11 ADT was made
(`mkadt_starlte.py --iphone11` takes `/product` from the real N104AP tree): start from the ADT
the kernel already boots with, change identity only.

| Node / property | iPhone 11 ADT today (`out/starlte-iphone11-1792.adt`) | Mac ADT (`out/starlte-mac-j313.adt`) | Read by |
|---|---|---|---|
| `/` model | `iPhone12,1` | `MacBookAir10,1` | `hw.model`, IOPlatformExpertDevice |
| `/` compatible | `N104AP iPhone12,1 samsung,a5xelte ... AppleARM` | `J313AP MacBookAir10,1 samsung,a5xelte samsung,herolte samsung,starlte samsung,exynos9810 AppleARM` | the PE match (`samsung,a5xelte`) + userland |
| `/` target-type / target-sub-type | `N104` / `N104AP` | `J313` / `J313AP` | MobileGestalt, sysdiagnose, firmware updaters |
| `/` manufacturer | (none) | `Apple Inc.` | System Information |
| `/` serial-number | (none) | `STARBITES9M1` (synthetic) | `IOPlatformSerialNumber` (`IOPlatformExpertDevice::configureDefaults`) |
| `/product` | N104AP's (artwork subtype 1792, phone idiom) | J313AP's: `product-name`/`product-description` "MacBook Air (M1, 2020)", `artwork-device-idiom` "mac", `graphics-featureset-class` "APPLE7", `compatible-app-variant` "MacFamily20,1", `product-soc-name` "Apple M1"; iBoot's `syscfg/...` placeholders dropped | MobileGestalt, CoreDisplay, Metal feature-set queries |
| `/chosen` chip-id / board-id | 0x8030 / none | **0x8103 / 0x26** (BuildManifest ApChipID/ApBoardID) | MobileGestalt, softwareupdated, RestoreOS checks |
| `/chosen` display-scale, random-seed | 2, 256 B | 2, a fresh 256 B (12377 wants 256; 8796 takes 64) | kernel |
| `/efi/platform` system-id | (none: **no IOPlatformUUID today**) | 16 bytes from the serial | `IOPlatformUUID`, `gethostuuid()` (`generatePlatformUUID`, IOPlatformExpert.cpp:2258). `--nvram` puts it in `/options` instead, which also brings up an IODTNVRAM |
| `disp0` canvas | 828x1792 (iPhone 11 points @2x) | unchanged for MAC0-MAC2. **MAC3:** landscape 1480x720 @1x with a 2x present (master plan §3.2) | SweetFramebuffer |
| memory, CPUs, GIC, UFS, `/pram`, fbcon, `/starbite-sd` | — | **unchanged** | kernel |

Not changed on purpose: `/chosen` `firmware-version` ("starlte-entry"), `secure-boot`,
`certificate-*` (absent = a development, unfused-looking Mac), and the `options` node (absent =
no NVRAM; `nvram(8)` will be empty).

## 6. The roots

`tools/macos/build-macos-root.sh t0|full`. It builds a raw HFSX volume with **no partition map,
not journaled, case-sensitive**, made by Apple's `newfs_hfs -s -b 4096` on a user-attached image.
A read-only mount never replays a journal, the volume starts at sector 0 of the `sdroot=` range,
and `rd=sdroot0` checks the `HX` signature at byte 1024.

- **Why HFSX and not APFS:** our XNU has no APFS (no apfs.kext, no IOMedia, no DiskArbitration).
  Both kernels mount HFS from the built-in hfs sources (fork patch 0026, `hfs-627` / `hfs-715`).
  APFS is required only by what we skip: sealed-volume checks, snapshots, the Preboot and Data
  volumes.
- **System and Data are one volume.** The Data volume template
  (`/System/Library/Templates/Data`, what the installer copies to a new Data volume) is moved to
  `/`. `/private/{etc,var,tmp}` come from it, and `/etc`, `/var`, `/tmp` are the system
  volume's own symlinks.
- **The cryptex is flattened:** the SystemOS cryptex's `System/Library/dyld/` goes to
  `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/`. That is the first place dyld-1066
  looks (its strings list `/System/Volumes/Preboot/Cryptexes/OS/` before `/System/Library/dyld/`),
  and the system volume's own `/System/Cryptexes/OS` symlink points there. No graft and no
  cryptexd.
- **Owners:** files are written by the user, then `hfs-owners.py` writes Apple's uid/gid/mode
  into the catalog records (default 0:0). `fsck_hfs -fn` passes on both images.
- **a64e:** as `build-ios-root.sh` does for iOS 26, `a64e-rewrite.py cache ... --skip
  libcorecrypto` rewrites the cache in place (corecrypto's FIPS self-test MACs its own code).
  launchd and dyld stay Apple's bytes and signatures; the kernel trap-emulates their PAC
  instructions, as it does for iOS 26's launchd.

### 6.1 Tier 0: `build/macos13/root-t0.img`

64 MiB (67,108,864 bytes), sha256
`be42769a4c77cc9068e18d632a3a1751dd2f0ce4ab531853481c7c33ec1c7c46`, 757 items, 26 MiB used:
`/sbin/launchd` (1,145,264 bytes, universal x86_64+arm64e, Apple signature valid),
`/usr/lib/dyld` (dyld-1066.10), `/System/Library/xpc/launchd.plist`, `/private` from the Data
template, an empty cryptex dyld directory, and `/.starbite-macos` (build tag). **No shared
cache.** It fits the 64 MiB test range that SD1 already read.

### 6.2 Full: `build/macos13/root-full.img`

3900 MiB (4,089,446,400 bytes; 3.5 GiB used, 335 MiB free), sha256
`7355e30042184c9d5c4302c0849fc24c9cf92c5e59b6e443a83659178e2f11f5`. fsck_hfs is clean, and
5,158 catalog records got Apple's owners.
- The arm64e shared cache: `dyld_shared_cache_arm64e` (1,592,393,728 bytes) and `.01`
  (1,833,435,136), plus the `.map`, at `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/`.
  They were decoded from the cryptex's LZFSE/LZBITMAP resource forks. The main header's subcache
  entry UUID `4caa7c9a...846b` equals `.01`'s header UUID. **Translated** by a64e-rewrite (counts
  in section 4).
- From the OS volume, arm64e slices only: `/sbin` (launchd 571,824 bytes thin), `/bin`,
  `/usr/sbin`, `/usr/lib` (dyld), `/usr/libexec` (xpcproxy and the daemons), all of
  `/System/Library/LaunchDaemons`, `LaunchAgents`, `xpc`, `SystemVersion.plist`,
  `loginwindow.app`, `SkyLight.framework` (WindowServer and its resources), `Metal.framework`
  and `CoreGraphics.framework` bundles (MAC3's needs), and the Data template.
  The list is `tools/macos/ventura-full.list`.
- Not included: `/usr/bin` (229 MiB fat: user tools), `/System/Library/CoreServices` beyond
  loginwindow (698 MiB), `/System/Library/Extensions` (381 MiB of kexts we cannot load),
  framework resources, `/Applications`. Add paths to the list as MAC2 needs them; 335 MiB are
  free in the image.
- Built in 2 runs (the second with `RESUME=1` after a decoder fix): OS part about 3 min, cryptex
  pass 2 about 2.5 min (it stops at the last needed block, 73 % into the stream), census + rewrite
  about 1 min.

## 7. Kernels

**For the first boots: `out/kernel.development.starlte-12377`** (with 0345-0348). It already has
everything a macOS launchd needs: the card root, arm64e grading + PAC emulation (0319), 16 KiB
dyld fixups and arm64e cache slides on 4 KiB pages (0320/0321), the AMFI stand-in answers
(0322), pmsg + sweetreboot (0309). It is also the kernel the 26.5 path uses.

**The 8796 port (the coordinator's item 3)**, source level, in `xnu/patches/pending-mac13/`
(made on `build/xnu-8796`, a worktree of `build/xnu-7195`'s `base-8796` branch = the iOS 16
tree; the six patches apply in order on it):

| Patch | From 12377 | Change for 8796 |
|---|---|---|
| 0307-starlte-microsd-driver | 0345 | applies unchanged; everything it calls exists in 8796 (`starlte_pmic_read/write` from 0302, `sweet_sample_start`, `crc32`, `ml_io_map`, devfs, buf KPIs) |
| 0308-starlte-microsd-root | 0346 | applies unchanged onto 0300's IOFindBSDRoot STARLTE block |
| 0309-starlte-microsd-cmu-div | 0347 | unchanged |
| 0310-starlte-microsd-high-speed | 0348 | unchanged |
| 0311-sweet-arm64e-userland | 0319 | the grading hunk was re-made: 8796 calls it `grade_binary()` (arm64e accepted at grade 8 with the ptrauth ABI); `sleh.c`, `sweet_a64e.c` and `sweet_a64e_fp.s` unchanged (`uNeon->ns_64` exists in 8796) |
| 0312-starlte-pmsg-sweetreboot | the pmsg part of 0309 | new file `bsd/dev/starlte_pmsg.c`: panic and timer -> ramoops pmsg -> reset to TWRP, without 12377's UFS log ring; `sweet_panic_hook` in `panic_spin_forever()` |

**Not compiled.** There is no 8796 build tree left (`build/dxb` is gone; a fresh
darwin-xnu-build tree is about 1 GB), so they are staged outside `xnu/patches/03*.patch`. That
way `tools/build-xnu-local.sh` keeps building the old iOS 16 kernel until they compile. To use
them: move them to `xnu/patches/`, run `FRESH=1 tools/build-xnu-local.sh`, fix what the compiler
says, and boot the same tier-0 root with `KERNEL=out/kernel.development.starlte
ADT=<64-byte-seed ADT>`.

Still missing on 8796 for a cache-backed (full) root:
- a check that the pager slides 16 KiB v3 slide pages correctly on 4 KiB pages. 8796's v3 path
  stores plain rebased pointers already (no `ptrauth_calls`), but 12377 needed
  `sweet_sr_slide_subpage` (0321) for this;
- whatever AMFI questions dyld-1066 and launchd ask that iOS 16 never asked;
- the whole GPU stack (section 2).

Porting the card driver alone was small (it applied unchanged). Making 8796 run an arm64e macOS
userland is **M** (2-4 sessions with compile and boot rounds). Making it draw is **XL**.

## 8. The first boot

The phone starts in TWRP. The coordinator runs every host and adb command
(`tools/store/boot-next.sh`), and the user only presses buttons. BOOT and the daily `@boot`
iOS 26 entry are never touched: everything is a **hidden lab entry in the UFS store** plus card
sectors **inside p2's free part or the test range**. The card store at p2 is valid (8 blocks used,
coordinator), so `sdroot=lba:` ranges inside p2 past block 8 are accepted
(`sd_range_vs_store`, 0345).

### 8.1 T0: launchd from the card (the MAC0 test)

**Result of the first try (t0, coordinator, 2026-11-03):** the root was written to the test range
and read back (`be42769a...` matched), and the store entry booted. Then the phone **hung**: no
return to TWRP after 10+ minutes, and no log.

Why no log: on 12377 the `sweetreboot=N` *timer* lives only in `herolte_log_thread`, which
`herolte_log_start()` starts from the **UFS** root path, and only with `ufsrw=1`
(`bsd/dev/herolte_ufs.c:1114-1138`). With `rd=sdroot0`, sweetreboot covers panics only (the
hook armed at STARTUP KMEM). So t0 did not panic: something hung or looped, and nothing could
reset it. The same holds for every card-root boot, the SD probe included (it ended in a
deliberate panic).

- **Fix: `xnu/series-12377/0349-starlte-sweetreboot-timer-no-ufs.patch`** (written here;
  committed and built by the main session as 1ebc653, `out/kernel.development.starlte-12377`
  from 20:17). A STARTUP(EARLY_BOOT) thread waits N s; unless the UFS log thread took over, it
  puts the console tail into pmsg and resets to TWRP (`starlte_panic_to_recovery()`). No
  existing boot-arg panics if IOFindBSDRoot is not reached. With 0349, any hang after
  EARLY_BOOT becomes a log.
- **t0's cmdline was the other suspect** (coordinator): it dropped most of the daily set
  (sweetfb canvas, `cpus=8`, memory limits). **t0b** keeps it.

| | |
|---|---|
| Payload (run this one) | **`out/xnu-mac13-t0b.xnu`**, sha256 `1f00aecb145e1645f582adb6ef54d8e04cf629c1e5953cac85682ebaf04f324a` (12377 kernel with 0345-0349, re-packed at 20:17; `out/starlte-iphone11-1792.adt`; `tools/cmdline/macos13-t0b.txt`, read back from the payload). Store entry `mac13-t0b.xnu`, catalog key `mac13t0b` |
| Root | `build/macos13/root-t0.img` -> card sectors **249868288 + 131072** (the 64 MiB test range; already on the card) |
| Command line (435 bytes) | `tools/cmdline/ios26-sdprobe.txt` minus `sdprobe`/`sdhold`/`sdbench`, `sweetinject`, `sbgl`/`sbmtl`/`sbprof`/`sbshot` and `a5fb_noconsole`, plus `sweettracename=launchd sweetreboot=120` (`rd=sdroot0 sdroot=lba:249868288,131072 herolog=0` kept) |
| First try | `out/xnu-mac13-t0.xnu` (sha `48787dec...`, `tools/cmdline/macos13-t0.txt`: a minimal 243-byte set with `cpus=4`): hung |

`sweettracename=launchd` logs launchd's failing syscalls with their paths, which shows where dyld
looks for the cache. The console stays on the panel (no `a5fb_noconsole`). If t0b hangs too, a
photo of the panel tells a kernel hang (before or after `BSD root: sdroot0`) from a userland spin
(after `exec launchd`).

Steps (TWRP, adb):
1. `shasum -a 256 build/macos13/root-t0.img out/xnu-mac13-t0.xnu` (the two values above).
2. Card: `python3 tools/macos/card-range-flash.py build/macos13/root-t0.img 249868288
   --check-only`, then without `--check-only`. It checks the card's size, the MBR, p1 and p2
   bounds and the store header, writes one 64 MiB chunk, and reads it back.
   By hand instead:
   `adb push build/macos13/root-t0.img /tmp/ && adb shell "dd if=/tmp/root-t0.img of=/dev/block/mmcblk0 bs=1048576 seek=122006 conv=fsync"`
   (122006 MiB = sector 249868288), then
   `adb shell "dd if=/dev/block/mmcblk0 bs=1048576 skip=122006 count=64 | sha256sum"` must print
   `be42769a...7c46`.
3. Store: pull, add, build, diff and flash as in `docs/XNU-MICROSD.md` §12.3 steps 2-5, with
   `mac13-t0.xnu=out/xnu-mac13-t0.xnu:xnu` and this catalog block (no `cmdline =` line):
   ```toml
   [[entry]]
   key = "mac13t0"
   name = "macOS 13 tier 0"
   version = "13.5 22G74 · XNU 12377 · card test range"
   category = "lab"
   method = "xnu"
   tag = "lab"
   files = ["mac13-t0.xnu"]
   root = "sd:lba 249868288+131072 (HFSX 64 MiB, read-only)"
   flags = ["hidden"]
   ```
   For t0b: `mac13-t0b.xnu=out/xnu-mac13-t0b.xnu:xnu`, `key = "mac13t0b"`,
   `name = "macOS 13 tier 0b"`, `files = ["mac13-t0b.xnu"]`.
4. Boot: `tools/store/boot-next.sh mac13t0b` (or Engineer > Start an OS).
5. Log: `build/test-boots/<time>-mac13t0b/pmsg.txt`.

Expected, in order:
`STARBITE SD: ... high speed ... 50375 kHz` -> `sdroot=lba:249868288,131072: ... volume header HX`
-> `BSD root: sdroot0` -> `hfs: mounted MACROOT on device ...` -> `load_init_program ...
/sbin/launchd` -> dyld (arm64e slice, grade 8) runs. With no cache it fails:
`dyld[1]: dyld cache '(null)' not loaded: ...`, then `Library not loaded:
/usr/lib/libSystem.B.dylib`. The sweettrace lines should show opens of
`/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e` failing with
ENOENT. Then launchd exits -> `panic: initproc exited -- exit reason namespace 6 (DYLD) ...` ->
pmsg -> TWRP.

**Pass:** the panic names dyld's missing cache (or libSystem). That proves the kernel mounted
the card root and started macOS's launchd, which ran its dyld. **Interesting failures:**
`exec of /sbin/launchd failed: 86` (EBADARCH: arm64e not graded) · `HFS did not mount` ·
AMFI or code-signing kills before dyld prints · a SIGILL at an instruction (check the census
classes).

### 8.2 T1: launchd with the shared cache (the step after MAC0)

| | |
|---|---|
| Root | `build/macos13/root-full.img` (3900 MiB = 7,987,200 sectors) -> card sectors **241348608 + 7987200**, inside p2 (182886400..249999359), past the store's used end (block 8), ending at 249335807: 64 MiB clear of the test range. The range also lies in the "free in p3" area of the quick Android re-layout (236169216..249999359, ANDROID-PLAN §5.1), so it survives that re-layout; after the full re-layout it moves to p6 |
| Command line | `tools/cmdline/macos13-t1.txt` (436 bytes): t0b's with `sdroot=lba:241348608,7987200` and `sweetreboot=300` |
| Payload | `out/xnu-mac13-t1.xnu`, sha256 `2e61dd47a5168b2e45bd14679e967232efe1ce72aa7a8e83e8440dbf93f3d4b8` (12377 kernel with 0345-0349, **`out/starlte-mac-j313.adt`**). Run it only after t0b passes |

Steps: `card-range-flash.py build/macos13/root-full.img 241348608` (about 61 chunks; all-zero
chunks are written from `/dev/zero` on the phone), the same store procedure with key
`mac13t1` / file `mac13-t1.xnu`, then `tools/store/boot-next.sh mac13t1 600`.

Expected: dyld maps the translated cache, and launchd runs (`launchd[1]` lines and its job
spawns). The first failures will be macOS boot work that wants APFS (Data volume mount,
`apfs_boot_util`), the launchd.plist signature, and missing daemons. That is MAC1/MAC2's list,
as iOS 16's milestone 5 was.

**Safety notes.** `sbstore2.py --medium sd` reserves only the last 64 MiB of p2. A card store
above 28.5 GiB would reach 241348608, so check `sbstore2.py header` before any large card store
build. A follow-up is to reserve this range in `sbstore2.py` too (not done here: not my file).
Both ranges are read-only for XNU (no `sdrw`).

## 9. Blockers (honest)

0. **Why t0 hung is still unknown.** The log gap is closed (0349: card-root boots now reset with
   the console tail), but the hang itself is not explained until t0b's log or a photo arrives.
1. ~~Disk for macOS 26.5~~: solved, the 26.5 root is built on the USB drive (section 12).
2. **Instruction classes the translator does not handle yet** (section 4). The likely
   unconditional ones on macOS are DIT, SB and LRCPC2. Small fixes, but each one is a crash until
   it is done, and 26.5 adds FHM-heavy and SME-gated code.
3. **WindowServer needs Metal and the display chain (MAC3).** Only the 12377 kernel has the
   Metal stack. The plug-in exists only as an iOS 26 build. A macOS build of
   StarbiteMetal/metal2vulkan/PanVK and the CoreDisplay mode/EDID path are unknown in size.
4. RAM: 1.8 GiB for macOS (master plan §0.4). Ventura is lighter than 26.5, but WindowServer +
   loginwindow + the daemon set will page heavily.
5. No APFS / IOMedia / DiskArbitration: launchd's macOS boot work expects a Data volume, sealed
   system checks and cryptex grafting. They must be disabled or answered (MAC1/MAC2).
6. The 8796 port is not compiled; the full-root path on 8796 still needs the slide check (section 7).
7. Card speed 23 MB/s (PIO, 0348): a cold launchd + cache start reads hundreds of MB.
8. Licence: personal experiment only (master plan §1.4). Apple's files never leave `build/`.

## 10. Sources

- Apple CDN IPSWs (URLs above) and their BuildManifest.plist; IPSW list from
  `https://api.ipsw.me/v4/device/Macmini9,1?type=ipsw`, `.../Mac17,5?type=ipsw`.
- Kernel strings: `kernelcache.release.mac13g` of 13.5 22G74, 13.5.2 22G91 and 13.6 22G120
  (blacktop `ipsw extract --kernel --remote`, build/tools/ipsw 3.1.739).
- Apple open source: `github.com/apple-oss-distributions/distribution-macOS` tags macos-130..135,
  `macos-135/release.json`; `github.com/apple-oss-distributions/xnu` tags (newest
  xnu-12377.121.6).
- APFS layout: Apple File System Reference (object map, B-tree node, fs records, sealed volumes).
  decmpfs type numbering as in apfs-fuse's `Decmpfs.h`.
- Repo: `docs/STARBITE-MASTER-PLAN.md` §0.3/0.4/3.2, `NOTES.md` (iOS 16 milestones 4-9, iOS 26
  bring-up, a64e), `docs/XNU-MICROSD.md`, `xnu/series-12377/0309, 0319-0322, 0345-0348`,
  `tools/mkadt_starlte.py`, `tools/build-ios-root.sh`, `tools/stage-ios-root.sh`,
  `tools/store/sbstore2.py`, `build/dsc-isa-census-*.txt`.
- Measured here: the cryptex listing (`build/macos13/cryptex-dyld-ls.txt`), the census reports
  (`build/macos13/census-*.txt`), the J313AP/J274AP device trees (`build/macos13/dt/`).

---

## 11. Kernel side for macOS 26.5 (kernel agent, 2026-11-03)

Scope: what macOS 26.5's userland needs from our 12377 kernel, from the first launchd up to
WindowServer, beyond what iOS 26 needed. The 26.5 binaries were not on the Mac yet, so the
userland evidence is this Mac's **macOS 27.2** (`26B5112a`, M4, read only), marked "27.2
proxy"; kernel evidence is the 12377 source and Apple's **26.5 kernels in the KDK**
(`build/kdk/KDK_26.5_25F71.kdk`).

### 11.1 Instruction emulation: patches 0350-0352

| Patch | Classes | Status | Proof |
|---|---|---|---|
| 0350 (main session) | MSR/MRS DIT, STLUR/LDAPUR/LDAPURS | in the series, booted with iOS 26 | — |
| `0351-sweet-a64e-fhm` | FMLAL, FMLAL2, FMLSL, FMLSL2, vector and by element | **in the series** (main session 5e2f2e3, builds clean) | exact FPMulAddH: halves widened in integer code (AHP 0, FZ16 flush without IDC), addend FZ flush with IDC, NaN pick as FPProcessNaNs3H + FPConvertNaN, then one single-precision FMADD under the user's FPCR (n*m of two halves is exact in single). `tools/a64e/difftest/run.sh` against this Mac's real FMLAL: 72 encodings, 3.6 M random cases (NaN/denormal/inf, FZ/FZ16/DN/AHP, all rounding modes): **0 mismatches** |
| `0352-sweet-a64e-sha3-sha512-sb` | EOR3, BCAX, RAX1, XAR, SHA512H/H2/SU0/SU1, SB; two FCADD-helper fixes | `xnu/patches/pending-macos26/`, commit f1e38734 in `build/xnu-12377`; single-file compile check with dxb's exact flags: clean, **not yet linked** | difftest: 2.2 M cases, **0 mismatches** |

Both new emulations use only integer code and ARMv8.0 FP, so they run on the M3 too. The FMADD
helper masks IRQ/FIQ while the user's FPCR and v0-v2 are live.

**0352 also fixes a latent panic that can hit iOS 26 with `cpus=8`:** FCADD with H elements ran
`fadd h0` (FP16 arithmetic) inside the kernel. The Mongoose M3 has no FP16 arithmetic, so on an
M3 that is an *undefined kernel instruction* = panic. 0352 refuses H-size FCADD on an M3
(MIDR implementer 0x53), and sleh.c pins the thread to an A55, where it is emulated. Also
fixed: both FP helpers zero-extend the 32-bit FPCR argument before `msr FPCR`.

Feature reporting is unchanged on purpose: FHM, DIT, LRCPC2, SHA3, SHA512 and SB stay
**absent** in `hw.optional.arm.*` and the commpage, so dispatching code keeps its non-FHM/SHA3
paths and only compiler-generated or unconditional sites trap.

### 11.2 Census corrections (tools/dsc-isa-census.py, for the 26.5 table)

- **The FHM class was wrong.** Its mask `0x9F60DC00 / 0x0E20CC00` ignores U (bit 29), so it also
  counts **FMLA/FMLS .2S/.4S** (U=0, opcode 110011) and **FACGE/FACGT .S** (U=1, 111011). §4's
  81,997 (13.5) and 74,093 (27.2) "FMLAL" sites are almost all ordinary FMLA. Correct patterns
  (encodings checked with clang on this Mac):

  | class | mask | value |
  |---|---|---|
  | FMLAL/FMLSL vector | `0xBF60FC00` | `0x0E20EC00` |
  | FMLAL2/FMLSL2 vector | `0xBF60FC00` | `0x2E20CC00` |
  | FMLAL/FMLSL by element | `0xBFC0B400` | `0x0F800000` |
  | FMLAL2/FMLSL2 by element | `0xBFC0B400` | `0x2F808000` |

  True FHM count in the whole 27.2 cache: **180** (76 + 76 + 14 + 14, libBNNS and a few ML
  libraries). The "FMLAL" hits in SkyLight, CoreGraphics, QuartzCore, CoreImage are FMLA.
- **BCAX was never counted:** `EOR3/BCAX 0xFFA08000 / 0xCE000000` matches EOR3 only. Use
  `0xFFE08000 / 0xCE000000` (EOR3) and `0xFFE08000 / 0xCE200000` (BCAX). 27.2: EOR3 977, BCAX 491.
- Handled-by labels: FHM -> `trap (0351)`; SB, EOR3, BCAX, RAX1, XAR, SHA512* -> `trap (0352)`.
- The tool was being edited by the root agent (uncommitted MOPS/SSBS/SME/CSSC classes), so I
  did not touch it: **root agent, please apply the masks above before the 26.5 run.**

Corrected per-image census, 27.2 proxy, classes that trap on both S9 cores (after 0350-0352
everything listed here except BF16 is emulated):

| Image (boot or WindowServer path) | sites |
|---|---|
| libsystem_m | EOR3 13, BCAX 4, XAR 1 (compiler output with +sha3), BFDOT/BFMLAL idx 2 (M2+, gated), SVE space 134 (`__sme_*`) |
| libsystem_platform | SB 1, DIT 3, SVE space 82 (`__sme_memchr` etc., chosen only with kHasFeatSME) |
| libcorecrypto | SB 1, DIT 62, LDAPUR 4, EOR3 20, BCAX 50, RAX1 10, XAR 48, SHA512 144 |
| dyld (in the cache) | SHA512 144 (its own corecrypto), DIT 3, SB 1, LDAPUR 3 |
| libsystem_malloc | LDAPUR/STLUR 66 |
| Foundation, Metal, JavaScriptCore, vImage, Espresso | EOR3 1 each (JSC also BCAX 12, LDAPUR 1,951) |
| CoreGraphics | **BCAX 48** |
| libboringssl | EOR3 2, XAR 6 |
| libBNNS | the only real FHM (180 in total), EOR3 4 |

### 11.3 SVE, SME, AMX and Apple IMPDEF registers: absent for every process (checked, no change)

- **Commpage / hw.optional:** ISAR0/ISAR1/MMFR2/PFR0 features are the nibble-minimum of the A55
  and the M3 (`commpage.c`, STARLTE). SME comes only from `arm_sme_version()`, i.e.
  ID_AA64PFR1.SME = 0 on both cores, so `kHasFeatSME/SME2/SME2p1` = 0, every
  `hw.optional.arm.SME_*` = 0, `sme_max_svl_b` = 0; SMFR0 is not even read. There is no SVE
  flag; `FEAT_SVE_B16B16` reads ID_AA64ZFR0 (RAZ on v8.0/v8.2) = 0. Same commpage for iOS and
  macOS processes.
- **AMX:** not a sysctl. Apple's 26.5 kernels carry `arm_amx_version <= (kAMXVersionMask >>
  kAMXVersionShift)`: the AMX version lives in the commpage capability word. This Mac's
  `caps64` = `0x9fffef97af0a7fc8`; the bits that are *not* in the public header are 27, 29, 36,
  39 and 63 (bits 27-29 = 0b101 [inference: the AMX version field]). Our kernel sets only
  public-header bits, so they are 0, and `_COMM_PAGE_CPUFAMILY` = 0 (A55 part 0xd05 ->
  CPUFAMILY_UNKNOWN).
- **IMPDEF registers (S3_x_C15), disassembled on the 27.2 cache:** dyld's TPRO toggles
  (`S3_6_C15_C1_5`, about 660 MRS/MSR sites) run only if commpage byte **+0x10C** is 2..4; libsystem_pthread's
  APRR/SPRR JIT toggles only for a commpage mode byte 1..4; libsystem_platform's ktrace writes
  only if `_COMM_PAGE_APT_ACTIVE` (+0x341) is set (our `apt_msg_policy()` stub returns 0). Our
  commpage leaves all of these 0.
- **Rule for later patches:** never write commpage bytes or capability bits outside the public
  `cpu_capabilities.h` for a macOS boot; that is what keeps AMX, TPRO, SPRR and SME code off.

### 11.4 Shared region: the cache must fit 6 GiB

Our kernel's arm64 shared region is `0x180000000 + 0x180000000` (6 GiB). Apple's **26.5**
T8103 release kernel uses the same (disassembled `vm_shared_region_lookup`: `mov x8,
#0x180000000` for both), so a real 26.5 cache fits. This Mac's **27.2 cache spans 7.18 GiB**
(`sharedRegionSize 0x1cb930000`): our kernel cannot map it, so 27.2 is a census stand-in only,
never a root. Root agent: check the 26.5 header's `sharedRegionSize <= 0x180000000`.

### 11.5 What macOS 26 expects at boot, and the plan

| Need | Who reads it | State on our kernel | Plan |
|---|---|---|---|
| `hw.machine` = `arm64`, `hw.model` = `MacBookAir10,1` | uname -m, model identifier everywhere | **already right for macOS.** The STARLTE build compiles the macOS branch of `sysctl_hw_generic` (its SWEET exception tests `ARM64_BOARD_CONFIG_SWEET`; STARLTE defines A5XELTE; checked by preprocessing kern_mib.c with dxb's flags) | none. Side finding for iOS: iOS processes get `hw.machine` = `iPad8,6` (Apple's iOS-on-Mac kludge), likely NOTES' "some UI says iPad". Do **not** add A5XELTE to that `#if` (macOS would get `uname -m` = MacBookAir10,1); if iOS needs DT names, switch at run time on `proc_platform(initproc)` |
| `hw.target` J313AP, `hw.product` MacBookAir10,1 | MobileGestalt, softwareupdate | right (first `compatible`, `model`) | none |
| IOPlatformUUID, `gethostuuid()` | configd, opendirectoryd, CFPreferences ByHost, loginwindow | present: on arm64 `generatePlatformUUID()` hashes `/chosen` unique-chip-id + chip-id (both in the Mac ADT). §5's `/efi/platform` system-id is the x86_64 input | none (mkadt-mac comment corrected) |
| IOPlatformSerialNumber | System Information, loginwindow | `STARBITES9M1` from root `serial-number` (`configureDefaults`) | none |
| Other root properties (platform-name, secure-root-prefix, model-number, region-info, mlb-serial-number) | System Information, MobileGestalt | absent | ignore; ADT if a daemon asks |
| `/chosen` boot-uuid, apfs-preboot-uuid, boot-objects-path (`kern.bootuuid`...) | launchd | ENOENT; 27.2 launchd only builds cookie paths under `/System/Volumes/Preboot/<uuid>/` and gives up ("no cookie") | ignore |
| NVRAM (`/options`, IODTNVRAM) | nvram(8), some daemons' settings | absent (no `/options`) | ignore for MAC1-2. `mkadt-mac --nvram` would give an in-memory IODTNVRAM (no `nvram-bank-size` -> empty CHRP image); untested |
| IOPMrootDomain, RootDomainUserClient | powerd, loginwindow | present (PMInstantiatePowerDomains) | none |
| **System sleep** | powerd idle sleep (laptop identity, no user input on the phone) | the port has no sleep path | **ADT: `/defaults sleep-disabled = 1`** (read by IOPMrootDomain at boot-complete; Apple's VMs do the same). Done in `tools/macos/mkadt-mac.py` (48b3066); regenerate `out/starlte-mac-j313.adt` at the next pack |
| Battery (IOPMPowerSource) | powerd, menu extras | SweetBattery (built for HEROLTE boards): 100 %, AC | none |
| AppleSMC | CoreBrightness, thermalmonitord, SampleAnalysis | absent; callers log "Failed to find the AppleSMC service" | ignore; a key-less `AppleSMC` IOService stub only if one blocks |
| IOHIDSystem (`IOService:/IOResources/IOHIDSystem`) | IOKit.framework NX* calls, CoreDisplay, OSServices | absent. CoreDisplay's `get_hid_service_connection()` returns 0 without aborting (disassembled) | ignore for drawing. MAC3+ input: a macOS-only SweetHID pointer service (GenericDesktop Mouse pair, absolute X/Y + button 1 from the digitizer) when initproc is a macOS binary; IOHIDSystem parameter stub only if loginwindow/WindowServer stall on it. SweetHIDButtons' consumer keys go through the same IOHIDEventService user client to macOS's hidd. No text input: use auto-login |
| kernelmanagerd check-in | IOKit registry quiet, deferred (dext) matching | `iokitDaemonAvailable()` is true on the macOS config: the service root stays busy +1 until kernelmanagerd sends personalities. iOS 26 logs show "KextLog: kernelmanagerd is active" | keep `com.apple.kernelmanagerd` enabled. If it fails, IOKitWaitQuiet callers (diskarbitrationd...) hang; kernel fallback (boot-arg that skips the busy count) not written |
| AMFI/Sandbox | dyld, launchd, every daemon | permissive stand-ins as for iOS. macOS-only sysctls missing (ENOENT): `security.mac.amfi.developer_mode_status`, `.allow_only_platform_code`, `security.mac.asp.willshutdown`, `security.mac.sandbox.sandcastle.enabled`, `security.mac.img4.ignition_blob` (iOS 26 runs without them too) | read the first boot's "SWEET AMFI: <proc> op N" / "SWEET Sandbox:" lines for macOS-only ops |
| Data volume (APFS) | launchd boot mode | 27.2 launchd: `fsctl(APFSIOC_GET_DEV_BY_ROLE)` on `/System/Volumes/macOS` and `APFSIOC_IS_VOL_LOCKED` on `/` fail on HFS -> data status 0 -> **default boot mode** (not fvunlock, not "pivot root"). The kernel's ROSV/firmlink/mount-by-role code runs only for APFS roles | root side: **do not** set `launchd_force_data_status=1` (1 = "unlocked [pivot root]" mode) |
| Boot tasks | launchd (27.2): fsck, mount-phase-1 (`/sbin/mount -P 1`), mount-phase-2, init-with-data-volume, `/sbin/mount -uw /` ("Root device is mounted read-only"), `/sbin/mount -vat nonfs` | the card root has no write path (`sdrw` prints "no write path"): `mount -uw /` fails, and a failed boot task was fatal on iOS 16 (milestone 5) | **MAC1 needs a writable root**: (a) a card write path in `starlte_sd.c` bounded to the `sdroot=` range (main session's driver), or (b) a macOS root on UFS (`ufsrw=1` works, but `ufsroot=` takes a whole GPT partition), or (c) for a first look only: no-op wrappers for these tasks and a read-only `/private/var` |
| `kern.osproductversion`, `osreleasetype`, `osproductversioncompat` | everything | RW sysctls, set by launchd from SystemVersion(Compat).plist | none |
| SIP | launchd, AMFI | `/chosen/asmb lp-sip0` = 0x6f from the base ADT (relaxed) | none |
| Shared cache size | dyld | 6 GiB region (11.4) | root: check the 26.5 header |
| Metal / IOGPU / IOSurface / IOMobileFramebuffer | WindowServer | Metal agent | — |

**Net result: no further kernel patch is required for the first macOS 26.5 launchd boots
beyond 0350-0352.** The blockers that remain are root-side (writable root, boot tasks) and one
ADT property (sleep-disabled, done in the tool).

### 11.6 For the root agent

1. Apply the census masks of 11.2 before the 26.5 table.
2. Check the 26.5 cache's `sharedRegionSize <= 0x180000000` (11.4).
3. Writable root or boot-task wrappers (11.5, "Boot tasks"); keep kernelmanagerd; leave
   `launchd_force_data_status` unset.
4. Re-run `tools/macos/mkadt-mac.py` for the Mac ADT (now adds `/defaults sleep-disabled`).

### 11.7 For the Metal/WindowServer agent

- CoreGraphics has 48 BCAX sites: **0352 must be in the kernel before WindowServer runs.**
  The FMLAL hits counted earlier in SkyLight/CoreGraphics/QuartzCore are ordinary FMLA.
- `IOHIDSystem` absence is tolerated by CoreDisplay; input for macOS is MAC3+ (11.5).
- macOS processes see `hw.model` MacBookAir10,1, `/product graphics-featureset-class` APPLE7.

## 12. The macOS 26.5 (25F71) root and payloads (root agent, 2026-11-03)

**State: built, not booted.** One HFSX root on the USB drive, two store payloads that boot it.
Everything Apple's is in `/Volumes/Untitled/starbite-macos26/` (the only folder this work uses on
that drive); the Mac keeps the payloads (`out/`, 2 x 17 MB), the ADT and the census text.

### 12.1 The IPSW, read without downloading it

```
https://updates.cdn-apple.com/2026SpringFCS/fullrestores/122-58869/DFB1CEEF-5619-4591-9924-E20DB2C8FED0/UniversalMac_26.5_25F71_Restore.ipsw
19,769,944,180 bytes (api.ipsw.me, MacBookAir10,1); J313AP is still in the BuildManifest (chip 0x8103, board 0x26)
```

| Manifest key (J313AP Erase Install) | Member | Zip size | Payload |
|---|---|---|---|
| OS | `094-56453-087.dmg.aea` | 10.33 GB, stored | AEA -> 13.07 GB **raw APFS** container, volume "Macintosh HD" (not sealed in the IPSW) |
| Cryptex1,SystemOS | `094-56679-089.dmg.aea` | 6.17 GB, stored | AEA -> 7.10 GB raw APFS, "CheerF25F71.arm64eSystemCryptex", sealed |
| BaseSystem / Cryptex1,AppOS | `022-21678-092.dmg.aea` / `094-56385-096.dmg` | 1.14 GB / 52 MB | not used |
| DeviceTree | `Firmware/all_flash/DeviceTree.j313ap.im4p` | 43 KB | `dt/j313ap-25F71.adt` (pyimg4) |

**AEA at random** (the doc's earlier "two streaming passes" is not needed): `ipsw-apfs.py`
opens a stored `.dmg.aea` member with libAppleArchive's `AEADecryptionRandomAccessInputStream`
over a custom byte stream whose pread is a ranged GET (`RangeCache`: windows from 256 KiB,
doubling to 16 MiB while reads are sequential). Opening costs one pass over the cluster
headers (48 MB for the cryptex, 135 MB for the OS image). The symmetric key comes from Apple's
key service through `ipsw fw aea --key` on the member's first 256 KiB, and is kept in
`keys.txt` beside the image. Then the raw APFS goes through the same `Apfs`/`Plan` code as
before; small files are written as soon as their extents are in, big ones stream to disk.
Measured: OS files 2.69 GB fetched (for 1.6 GB of fat files), cache 1.81 GB fetched (for
5.54 GB of files), at 5-18 MB/s.

Fixed on the way (also affects Ventura): `Plan` skipped the contents of a directory that had
first been added as the parent of an earlier path. **Ventura's `root-full.img` has only launchd
in `/sbin` and only dyld in `/usr/lib`** (the list names `/sbin/launchd` before `/sbin`); the
26.5 root got its 687 missing items back the same day. `Plan` also follows symlinks inside
paths now (kept as symlinks) and skips missing paths with a warning.

### 12.2 The root: `/Volumes/Untitled/starbite-macos26/root-26t1.img`

| | |
|---|---|
| Image | 10,737,418,240 bytes (10 GiB, **20,971,520 sectors**), raw HFSX, case-sensitive, **not journaled**, volume MACROOT; 6.4 GiB used, 3.6 GiB free |
| sha256 | **`4a1dceb6a4b2bbb48e04ed376fce15be89bfaf749bd7272c43f0c2c622f53c7d`** |
| Checks | `fsck_hfs -fn` clean, `HX` at byte 1024, 9,129 catalog records with Apple's owners (`hfs-owners.py`) |
| Build | `MACOS=26 tools/macos/build-macos-root.sh t1` (about 50 min on the 15-30 MB/s USB stick: 13 min of it zero-filling the 10 GiB file on exFAT) |
| Card | **LBA 182888448 + 20971520** (ends at 203859967), inside p2 past the card store's used end, 37.5 M sectors below the Ventura root at 241348608 |

Contents:
- From the OS volume, arm64e slices only (`tools/macos/macos26-t1.list` +
  `macos26-jobs.list`, 9,074 items, 1.6 GB fat): `/bin`, `/sbin`, `/usr/{lib,libexec,sbin}`,
  `/usr/bin/true`, every LaunchDaemons/LaunchAgents plist and `xpc/launchd.plist`, the
  executable (+ bundle Info.plist) of **every launchd job** outside those trees (generated from
  the job plists: 871 paths), the Data template's `private/`, and the bundles WindowServer and
  loginwindow need later: SkyLight (WindowServer), loginwindow.app, WindowManager.app,
  CoreDisplay, Metal, CoreGraphics, IOSurface, QuartzCore (`default.metallib`), IOGPU,
  GPUSupport, IOKit, Security, Keychains, Sandbox, Preferences/Logging, FeatureFlags.
- The arm64e shared cache, all 12 subcaches + `.map` + `.atlas` (5.54 GB), at
  `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/`, **translated** by a64e-rewrite
  (3,643 images, `--skip libcorecrypto`): ret 5,401,739, br 3,584,011, blr 6,067,095, pac
  11,798,611, ldapr 133,885; left for the kernel: pacga 724, fjcvtzs 932, fcadd 16,239, ldra 18.
  `sharedRegionSize` **0x165114000 (5.58 GiB) + maxSlide 256 MiB: fits the 6 GiB region** (11.4).
- dyld-1378 looks in `/System/Volumes/Preboot/Cryptexes/OS/`, then `/private/preboot/Cryptexes/OS/`,
  then `/System/Library/dyld/`. All three work: the files, `/private/preboot ->
  ../System/Volumes/Preboot`, and `/System/Library/dyld -> ../Volumes/Preboot/Cryptexes/OS/System/Library/dyld`
  (b8ce21d). `/System/Cryptexes/OS` is the OS volume's own link to the Preboot path.
- Not included: x86_64 and Rosetta AOT caches, the cryptex's WebKit/Safari parts, `/usr/bin`
  (except `true`), CoreServices apps beyond the job executables, `/System/Library/Extensions`
  (kexts), fonts, `/Applications`.

### 12.3 Instruction census of the 26.5 cache (Apple's bytes, before the rewrite)

`build/venv/bin/python tools/dsc-isa-census.py .../dyld_shared_cache_arm64e` (copy in
`build/macos26/census-25F71.txt`). 3,646 images, 735,036,712 instructions (2,804 MiB of code).
The tool's masks were corrected first (11.2 plus: FCMLA's and FP16's values had bits outside
their masks and never matched; the DIT/SSBS `#imm` row matched SMSTART/SMSTOP instead of SSBS).

| Class | Sites | Lacks | Handled by |
|---|---|---|---|
| RETAA/RETAB · BRA*/BLRA* · PAC/AUT/XPAC reg | 5,406,023 · 9,655,104 · 11,800,450 | both | rewrite |
| LDRAA/LDRAB | 22 | both | rewrite (18 writeback: trap) |
| PACGA · FJCVTZS · FCADD | 724 · 932 · 16,239 | both | trap (0319) |
| LDAPR | 133,885 | M3 | rewrite -> LDAR |
| LDAPUR/STLUR (LRCPC2) | 127 | both | trap (0350) |
| MSR DIT #imm · MRS/MSR DIT | 101 · 91 | both | trap (0350) |
| FMLAL/FMLSL(2) vector + element | 8 + 8 + 56 + 56 = 128 | both | trap (0351) |
| EOR3 · BCAX · RAX1 · XAR | 5,400 · 325 · 45 · 9,037 | both | trap (0352) |
| SHA512H/H2/SU1 · SHA512SU0 · SB | 896 · 256 · 7 | both | trap (0352) |
| LSE CAS · CASP · LD<op>/SWP | 70,260 · 450 · 717,019 | M3 | trap (M3, 0342-0344) |
| DotProd (+U, idx) · RDM | 6,247 · 1 | M3 | **none** (M3) |
| FP16 scalar (counts FCVT from H too, so an upper bound) | 29,771 | M3 | **none** (M3) |
| **FCMLA vector · by element** | **3,164 · 5,643** | both | **none**: all in libBLAS (complex BLAS) except 1 in libsystem_m |
| **MOPS CPY/SET (v8.8)** | **18** | both | **none**: all in libsystem_m |
| **CSSC min/max** (v8.9) | 505 | both | **none** |
| SMSTART/SMSTOP · MRS/MSR SVCR · TPIDR2 | 2,469 · 1 · 1,422 | both | none: SME paths (SME absent in the commpage, 11.3) |
| SVE space · SME space | 132,978 · 136,508 | both | none: `__sme_*` / Accelerate kernels behind feature checks (11.3) |
| AMX (Apple) | 359,075 | both | none: Accelerate, behind the CPU family (11.3) |
| MRS/MSR S3_x_C15 (IMPDEF) | 1,660 | both | none: TPRO/SPRR toggles behind commpage bytes (11.3) |
| BF16 (BFCVT/N, BFDOT idx) | 1,181 | both | none |
| ERETA*, FlagM, SSBS, CNTxCTSS, DPB2, WFxT, FRINT32/64, BFMMLA, I8MM, CSSC CTZ/CNT/ABS | 0 | | |
| **total in "none, both cores" classes** | **644,624** | | 510,277 in Accelerate (BLAS/BNNS/LAPACK/vDSP/vImage), 132,109 in CorePhotogrammetry/CMImaging/LearnedFeatures |

Boot-path images (per image, classes that are not rewritten; `none` classes in bold):

| Image | What is left |
|---|---|
| dyld (cache copy) | LDAPUR 3, SB 1, DIT 3, SHA512 144, LSE 65, **S3_x_C15 682** (TPRO, gated), SVE/SME space 6/3 |
| libcorecrypto (not translated) | LDAPUR 4, SB 1, DIT 58, EOR3 20, BCAX 50, RAX1 10, XAR 48, SHA512 144, SVE/SME space 17/7; plus all its PAC ops trap (it is skipped by the rewrite) |
| libsystem_platform | SB 1, DIT 3, LSE 84, **S3_x_C15 28**, SVE/SME space 85/15 (`__sme_*`) |
| libsystem_m | LDAPUR 16, EOR3 13, BCAX 4, XAR 1, **MOPS 18, FCMLA 1, CSSC 4, BFDOT 2**, RDM 1, SVE/SME 134/150 |
| libsystem_malloc · libdispatch · libxpc · libobjc · libsystem_pthread | LDAPUR 50 (malloc); LSE only; **S3_x_C15** 25 (objc), 24 (pthread) |
| CoreFoundation · Foundation · Security · IOKit | PACGA 106/330, LSE, EOR3 1 (Foundation), DotProd U 2 (CF) |
| SkyLight · CoreDisplay · IOSurface · IOGPU · Metal | LSE only (Metal: EOR3 1, FP16 7) |
| CoreGraphics · QuartzCore | BCAX 48 + FP16 3,497 (CG); FP16 235 (QC) |

Reading it: with 0350-0352 every class that a daemon or the WindowServer path is likely to run
unconditionally is emulated. What is left is gated code (SME/SVE/AMX/IMPDEF, 11.3), complex
BLAS (FCMLA), and **libsystem_m's 18 MOPS and 4 CSSC sites** (v8.8/v8.9, M4-class code, most
likely chosen at run time; the first SIGILL in libsystem_m would say otherwise). On the M3s
(`cpus=8`): DotProd (6,247, mostly libswiftCore/JavaScriptCore/sqlite3) and real FP16
arithmetic would trap there with no emulation; if one shows up, `cpus=4` keeps the userland on
the A55s.

### 12.4 Boot tasks and the read-write root

launchd 26.5 (3102.120.13) carries its boot tasks in a plist inside its own `__TEXT` (the
`Boot` dictionary; `/System/Library/xpc/launchd.plist` has none). 46 tasks; the ones marked
`RequireSuccess` end the boot when they fail. With **`sdrw=1`** (kernel 0353) the root mounts
read-only, then `mount -uw /` makes it read-write (the HFS update path has no dirty-volume check
for the root, so a reset without unmount does not block the next boot; the first mount at boot
stays read-only because the command line has no `rootrw=1`).
`tools/macos/macos26-boot-tasks.sh` puts Apple's `/usr/bin/true` in place of:

| Task (program) | Why a no-op |
|---|---|
| `fsck` / `fsck-safe` (`/sbin/fsck -qL -R 1`, RequireRun) | the kernel's fsck_hfs path is untested (coordinator) |
| `fips` (`/usr/libexec/cc_fips_test`, RequireRun) | corecrypto's self-test on trap-emulated PAC/DIT/SHA3/SHA512; one failure ends the boot (iOS 26 needed fips-ok too). Undo after a boot that gets far |
| `data-protection` (`/usr/libexec/init_data_protection`) | no AppleKeyStore in our kernel |
| `EndpointSecurity` (`/usr/libexec/endpointsecurityd --init`) | no EndpointSecurity kext (the daemon behind it is gone too) |

Originals stay as `<path>.starbite-orig` (list in `/.starbite-boot-tasks`); `--undo` restores
them, `--ro` also wraps the four tasks that write (dirs_cleaner, mount -P 1/2, tzinit,
dirhelper) for a root without `sdrw=1`. Left as they are: `mount -P 1/2` (prints "Not booted
from APFS, skipping apfs_boot_util"; no fstab), dirs_cleaner, tzinit, dirhelper, rc.netboot,
the internal Block tasks; the installer/demo/obliteration programs are not on the root.

The rest of the Starbite layer (`tools/macos/macos26-post.sh`, run by the builder):
- **WindowServer's job is out** (`macos26-disabled.list`, taken out of launchd.plist by
  `starlte-disable-jobs.py`, its LaunchDaemons plist renamed `.starbite-off`). Note that its job
  also owns `com.apple.iohideventsystem`.
- **Metal staged** (`tools/macos/metal/build.sh stage`, docs/MACOS-METAL.md §5): the bundle in
  `/System/Library/Extensions/StarbiteMetal.bundle`, `/usr/lib/sbmac.dylib`,
  `/usr/local/bin/sbmtltest`, `/private/var/local/{mtl2-clients,m2v-cache}`.
- **The M1 job is gated:** launchd 26.5's bootstrap paths do not include `/Library/LaunchDaemons`,
  so `com.starbite.sbmtltest` is in `/System/Library/LaunchDaemons` and in launchd.plist, and it
  runs `/bin/sh -c` that execs `sbmtltest --seconds 30` only when `sbmtltest=1` is on the command
  line (otherwise it prints `SBMTLTEST: off`). One root serves both payloads.

### 12.5 Payloads, store and catalog

Both: `out/kernel.development.starlte-12377` (22:33, 0345-0353 + 0360; sha `a4b9d1cb...`) +
`out/starlte-mac-j313.adt` (regenerated from **26.5's** J313AP tree with `/defaults
sleep-disabled`; sha `5a58af7c...`) + the command line, packed by `mkpayload-ios26.sh`.

| Payload | sha256 | Command line |
|---|---|---|
| `out/xnu-mac26-t1.xnu` (17,217,616 B) | `e1a08185f37c1389612cf6f8a70b7592fb8b0bf9214e865fab914954328bd2e4` | `tools/cmdline/macos26-t1.txt`, 444 bytes: macos13-t1's with `sdroot=lba:182888448,20971520`, `sweetreboot=420`, `sdrw=1` |
| `out/xnu-mac26-m1.xnu` (17,217,616 B) | `3dcb2a417b1abd03888ce1508feae6b1321f3353547b6f0d6743b8fb0c9df836` | `tools/cmdline/macos26-m1.txt`, 519 bytes: t1's + `sweetinject=/usr/lib/sbmac.dylib,WindowServer a5fb_noconsole=1 sbmtltest=1` |

Card: `python3 tools/macos/card-range-flash.py /Volumes/Untitled/starbite-macos26/root-26t1.img 182888448 --check-only`,
then without `--check-only` (160 chunks of 64 MiB; about 100 carry data, the rest are written from /dev/zero). Read-back of the whole range must
print `4a1dceb6...3c7d` **before the first boot**: with `sdrw=1` the range changes on every boot.

Store files and catalog (`docs/XNU-MICROSD.md` §12.3 procedure):
`mac26-t1.xnu=out/xnu-mac26-t1.xnu:xnu` and `mac26-m1.xnu=out/xnu-mac26-m1.xnu:xnu`

```toml
[[entry]]
key = "mac26t1"
name = "macOS 26 tier 1"
version = "26.5 25F71 · XNU 12377 · card p2"
category = "lab"
method = "xnu"
tag = "lab"
files = ["mac26-t1.xnu"]
root = "sd:lba 182888448+20971520 (HFSX 10 GiB, sdrw)"
flags = ["hidden"]

[[entry]]
key = "mac26m1"
name = "macOS 26 Metal M1"
version = "26.5 25F71 · XNU 12377 · sbmtltest"
category = "lab"
method = "xnu"
tag = "lab"
files = ["mac26-m1.xnu"]
root = "sd:lba 182888448+20971520 (HFSX 10 GiB, sdrw)"
flags = ["hidden"]
```

### 12.6 Expected first boot (mac26t1)

`STARBITE SD: ... high speed` -> `root sdroot=lba:182888448,20971520 ... HX, read-write (sdrw=1)`
-> `BSD root: sdroot0` -> HFS mounts MACROOT -> `/sbin/launchd` (arm64e, grade 8) -> dyld-1378
maps the translated cache (sweettrace shows the opens under `/System/Volumes/Preboot/Cryptexes/OS/`)
-> launchd's banner, **"Darwin Bootstrapper Version 7.0.0 ... 3102.120.13"**, and this time past
Ventura's "could not set up inheritance port" (launchd 26.5 and xnu-12377 are one release). Then
the boot tasks (four no-ops, `mount -uw /` via `mount -P 1`, dirs_cleaner, tzinit, dirhelper),
the boot mode (default: launchd's APFS role ioctls fail on HFS, 11.5), and the system domain's
~888 daemons. `SBMTLTEST: off` on the console shows LaunchDaemons started. sweetreboot resets
into TWRP at 420 s with the console tail in pmsg.

Likely first failures, in order of my guess: a `RequireSuccess` boot task that still fails
(`mount -P 1`, `dirhelper`, `tzinit`: rerun the wrapper with `--ro` on that task); libignition's
cryptex graft (no APFS, no graftdmg; it was not fatal on Ventura); kernelmanagerd's check-in
(11.5); daemons crashing on missing hardware or APFS; a SIGILL in a class from 12.3 (the class
names the fix). **mac26m1** expects MACOS-METAL.md §6.2's `SBMTLTEST:` lines (steps 1-7).

Notes: the root is not journaled and fsck is a no-op, so every reset leaves it dirty; it still
mounts (above), but re-flash it from the image (or run `fsck_hfs -fy` on a pulled copy) if HFS
errors appear. A card store build (`sbstore2.py --medium sd`) grows from p2's start and would
overwrite this range: check `sbstore2.py header` first, as for the Ventura range.
