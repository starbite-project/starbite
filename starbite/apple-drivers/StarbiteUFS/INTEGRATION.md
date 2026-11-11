# StarbiteUFS: integration (for the integrating session)

Nothing here has been applied; this driver's session did not touch `build/`, `tools/board`, `tools/store` or `shim/`.

## 1. Prerequisites

- **IOStorageFamily built from source** into the kernel (tag `IOStorageFamily-337.100.1`, docs/APPLE-S9-DRIVERS.md
  2.4): `IOBlockStorageDevice`, `IOBlockStorageDriver`, `IOMedia`, `IOMediaBSDClient`, `IOGUIDPartitionScheme`
  and their personalities. Until then the headers come from the SDK (the kcheck `-I` below).
- An `/arm-io/ufs` nub of class `IOPlatformDevice` (or a subclass whose `compareName` honours `compatible`), with
  its four `reg` ranges in board order (HCI, vendor HCI, UniPro, UFSP) as `IODeviceMemory`, and `interrupts`
  resolvable through the interrupt controller (StarbiteGICv2). Without an interrupt the driver polls.
- No system `IOMapper` (SysMMU in bypass): `IODMACommand` must hand out physical addresses.

## 2. Sources and build

| File | conf/files entry (suggested) |
|---|---|
| `starbite/apple-drivers/StarbiteUFS/StarbiteUFS.cpp` | `iokit/conf/files.arm64`: `...StarbiteUFS.cpp optional iokitcpp` (STARLTE only) |
| `starbite/apple-drivers/StarbiteUFS/hw/sbufs_fence.c` | `bsd/conf/files` (or `iokit/conf/files`): `standard` on STARLTE |
| `starbite/apple-drivers/StarbiteUFS/hw/sbufs_proto.c` | same |
| `starbite/apple-drivers/StarbiteUFS/hw/sbufs_hc.c` | same |

Include path: the folder `starbite/apple-drivers/StarbiteUFS` (the .cpp includes `"hw/sbufs_hc.h"`; the core
includes its siblings). The core needs only `<stdint.h> <stddef.h> <stdbool.h> <string.h>` and, under `KERNEL`,
`<libkern/libkern.h>` for `snprintf`. Compile-checked clean with the kernel's own flags:

```
python3 tools/apple/kcheck.py starbite/apple-drivers/StarbiteUFS/StarbiteUFS.cpp \
  -I /Applications/Xcode_16.4.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk/System/Library/Frameworks/Kernel.framework/Headers
python3 tools/apple/kcheck.py starbite/apple-drivers/StarbiteUFS/hw/sbufs_{fence,proto,hc}.c
```
(the .cpp's 8 remaining warnings are all inside the SDK's storage headers: `-Wzero-as-null-pointer-constant`,
`-Wshadow-field`.) Host tests: `sh starbite/apple-drivers/StarbiteUFS/test/run.sh`.

`STARBITEUFS_SBSTOP` (default 1) uses `starlte_sbstop_at()` / `starlte_sbstop_now()` from
`bsd/dev/herolte_ufs.c` (0423) for the stages `ufs` and `ufsroot`. Define it 0 for a Boot KC build (those
symbols are not exported).

## 3. Personality (KernelConfigTables.cpp, STARLTE)

```
"   {"
"     'IOClass'         = StarbiteUFS;"
"     'IOProviderClass' = IOPlatformDevice;"
"     'IONameMatch'     = 'starbite,ufs';"
"     'IOProbeScore'    = 1000:32;"
"   },"
```
(`Info.plist` carries the same for a later KC; OSBundleRequired Root.)

## 4. ADT: the fence properties on /arm-io/ufs (done in this session, uncommitted)

The driver refuses to start without a filled `starbite,allowed-ranges`. Format and allow-list rule: NOTES.md.
- `tools/board/starlte.board`: the `ufs` block gets `fence = { "starbite,allowed-ranges" = 8,
  "starbite,denied-ranges" = 48 }`; `tools/board/mkadt-board.py` emits them as placeholders (header, count 0,
  zero records) and `--check` verifies them. The ADTs in `out/` from before this change fail that check until
  they are regenerated.
- `shim/starlte/ufs_fence.c/.h` (new) + one hook in `xnu_enter` (`shim/starlte/xnu_boot.c`) + `ufs_fence.c` in
  `shim/starlte/build.sh`: fills them in the ADT copy at every XNU start from the live GPT.
- `starbite,writes-allowed` is never emitted (M4 and on: read-only).
Host test of the loader side: `test/loader/` (run by `test/run.sh`).

## 5. Coexistence with herolte_ufs.c

`herolte_ufs.c` must stay compiled (it carries the boot-health code: sbwdt, sbcrash, sbstop, pmsg), but its UFS
part drives the same host. It only starts for `rd=ufsroot0` or `ufsprobe=1`; StarbiteUFS refuses to start when
either is on the command line. Never use both in one boot.

## 6. Root by IOMedia (IOKitBSDInit.cpp)

Works today without changes: `rd=disk0` (BSD-name matching) while StarbiteUFS is the only disk driver.
Proposed for when StarbiteSD joins (a few lines in `IOFindBSDRoot`, next to the `sdroot0`/`ufsroot0` cases):

```
rd=sbrange:<UUID>  ->  matching = { IOProviderClass = IOMedia; IOPropertyMatch = { Whole = Yes; };
                                    IOParentMatch = { IOPropertyMatch = { "Starbite Range UUID" = "<UUID>"; }; }; }
```
(`IOParentMatch` must match an ancestor, not only the direct provider: check `IOService::matchInternal` in 12377
before relying on it; otherwise walk the parents in a notification handler.) A range holding an inner GPT is
matched by the partition IOMedia's own `UUID` instead (the stock `IOUUIDMatching` path needs
`boot-uuid-media`, which only Apple's closed filesystems publish).

Properties published on each StarbiteUFSRange: `Starbite Range Name`, `Starbite Range UUID`, `Starbite Range LUN`,
`Starbite Range First LBA`, `Starbite Range Blocks`, `Starbite Range Root`, `Starbite Range Writable`, the
`Device/Protocol Characteristics` dictionaries, and the `IOMaximum*` transfer limits (256 blocks, 4 KiB segments,
32 address bits).

## 7. Boot-args

| Arg | Default | Meaning |
|---|---|---|
| `sbufsrw` | 0 | 1 = writes, if the ADT says `starbite,writes-allowed` = 1 and the range is WRITABLE |
| `sbufsirq` | 1 | 0 = polled only |
| `sbufsmulti` | 1 | 0 = one 4 KiB block per command (no self test, vendor registers untouched) |
| `sbufsqd` | 4 | slots 1..8 |
| `sbufsbounce` | 0 | 1 = every transfer through the bounce buffers |
| `sbufs64` | 0 | 1 = DMA above 4 GiB if CAP.64AS |
| `sbufscached` | 0 | 1 = cacheable descriptors, one slot (herolte's mode) |
| `sbufsreport` | 0 | S = full report (tree, root, fence) S s after start |
| `sbstop=ufs` / `sbstop=ufsroot` | - | test-boot stops (0423), see PHONE-TEST.md |
