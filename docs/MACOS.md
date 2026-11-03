# macOS on the Galaxy S9: MAC0 (host groundwork for a first boot)

Status 2026-11-03. Nothing here was written to the phone. Every image and payload is on the Mac,
in `build/macos13/` and `out/` (both gitignored, Apple software stays local).

**Done means "the kernel starts macOS's launchd from a read-only root on the card".** The
payload for that test is `out/xnu-mac13-t0b.xnu`, with `build/macos13/root-t0.img` already in
the card's 64 MiB test range (section 8.1). Its first version (t0) hung with no log; the
cause of the missing log is found, and the fix (patch 0349) is written but not built. A second root with the full translated shared cache
(`build/macos13/root-full.img`, section 6.2) is the step after it, where launchd actually runs.

Contents: 1 version · 2 Ventura or 26.5 · 3 partial download · 4 instruction census ·
5 device identity · 6 the roots · 7 kernels · 8 first boot · 9 blockers · 10 sources.

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

- **Fix (written, not built):** `xnu/patches/pending-mac13/for-12377/0349-starlte-sweetreboot-timer-no-ufs.patch`.
  A STARTUP(EARLY_BOOT) thread waits N s; unless the UFS log thread took over, it puts the
  console tail into pmsg and resets to TWRP (`starlte_panic_to_recovery()`). It applies on
  `build/xnu-12377`. There is no existing boot-arg that panics if IOFindBSDRoot is not reached;
  with 0349, any hang after EARLY_BOOT becomes a log.
- **t0's cmdline was the other suspect** (coordinator): it dropped most of the daily set
  (sweetfb canvas, `cpus=8`, memory limits). **t0b** keeps it.

| | |
|---|---|
| Payload (run this one) | **`out/xnu-mac13-t0b.xnu`**, sha256 `355683503903001b050689ad484cb88c5097982cdfa4aedb4d4e9f3f1d66c399` (12377 kernel with 0345-0348; `out/starlte-iphone11-1792.adt`; `tools/cmdline/macos13-t0b.txt`). Store entry `mac13-t0b.xnu`, catalog key `mac13t0b` |
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
| Payload | `out/xnu-mac13-t1.xnu`, sha256 `2e61dd47a5168b2e45bd14679e967232efe1ce72aa7a8e83e8440dbf93f3d4b8` (12377 kernel, **`out/starlte-mac-j313.adt`**). Run it only after t0b passes, and preferably with 0349 built in |

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

0. **Card-root boots have no timed reset.** 12377's sweetreboot timer needs the UFS log thread,
   so a hang on `rd=sdroot0` stays hung (t0). Patch 0349 is written, not built (section 8.1).
   Until it is in, every hang costs a button press and gives no log.
1. **Disk for macOS 26.5 (the drawing path).** Its root is about 8 GB against the 6 GB budget.
   Needs the external drive or the master drive (master plan decision 1). Until then, 26.5 can
   only be censused and staged in pieces.
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
