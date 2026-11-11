# The OS store on the microSD card (2026-11-11)

**Why:** the full iOS 27 root (about 18.5 GiB, untrimmed) needs **all of USERDATA** (sda25, 4,915,328 blocks of 4 KiB =
18.75 GiB, byte 0 to the end). Until now the Starbite OS store lived at USERDATA +16 GiB (blocks 4194304..), and the
loader kept its settings record and crash log there. After this migration the loader needs **nothing on USERDATA**.
No Samsung partition (PARAM, EFS, ...) is used for anything.

## 1. Layout on the card (sd128, S9 #1's 128 GB card)

Card p2 (MBR type 0xDA, sector 182886400, 32 GiB) is unchanged. The store's 1 MiB store area at p2's start is too small
for the live store (53 entries, 478 MiB), so the store is split, **inside p2, without moving anything**:

| p2 blocks (4 KiB) | card sectors | what |
|---|---|---|
| 0..6 | 182886400.. | v1 view (empty), **SBSTORE2 header with the home extension**, settings record, directory |
| 7..255 | | unused (the old card store's catalog sat at 7; overwritten by nothing) |
| 256.. | 182888448.. | iOS 26 root (parked), macOS Ventura root: untouched |
| **8126720..8372735** | **247900160..249868287** | **the store's data: the layout's "storedata" area, 961 MiB** (was "free") |
| 8372736.. | 249868288.. | tail (XNU test range, sdlog ring, SBTIME01 clock): untouched |

`tools/store/cardlayout.py` names the 961 MiB area `storedata` (kind store). The store format already allowed entries
anywhere below the capacity; the only new thing is where the data starts. Room: 961 MiB - 478 MiB = **483 MiB to grow**.
More room later means giving the store another area (for example the parked iOS 26 root's 16 GiB): the user's call.

### The home extension (header block 1, bytes 64..91)

`"SBSTHOME"`, u32 1, u32 flags (bit 0 **HOME**: the loader's settings record is this store's block 2, its crash log is
this store's `crashlog` entry), u64 the first data block (7 for an ordinary store, 8126720 on the sd128 card), u32 CRC32
of bytes 64..87. The SBSTORE2 header CRC covers bytes 0..47 only, so older loaders and tools read such a store as before
(and ignore the extension). `sbstore2.py build --home --data-at layout` writes it.

**Compact images.** A store whose data starts past block 7 is kept on the Mac as a *compact* image: file blocks 0..6,
then the data (store block 8126720 at file block 7). `sbstore2.py list/header/build --import`, `store-pull.sh OUT sd`
and `card-store.sh` understand it; `store-flash-sd.sh` refuses it (and refuses to write over a home card store), because
writing it from block 0 would put the data over the iOS 26 root.

## 2. The loader (shim/starlte/store.c, sd.c)

**The home store** = the store holding the settings record and the crash log. `store_home()` decides it once per boot,
before any other store access:

| mode | how it is set | the card | the UFS store at USERDATA +16 GiB |
|---|---|---|---|
| **card only** | `sbstore=card` on BOOT's command line (payload cmdline), or a build with `-DSTORE_CARD_ONLY` (chain tests) | always brought up; its version 2 store is home | **never opened: no read, no write, no listing** |
| default (today's BOOT) | nothing | touched only if the UFS settings say "card store at boot" (`SET_SD_STORE`); a card store marked HOME is then home | closed for the boot as soon as the card is home; otherwise home, exactly as before |

- **Settings** (`settings_load/save`) and the **crash log** (`store_crashlog`, `store_crashlog_write`, crash.c's reads
  via the new `store_read`) go to the home store's medium. Every write is read back.
- **Card writes** (new, `sd_write4k` in sd.c: CMD24/CMD25 with auto stop, busy line, CMD13 until tran; the write path of
  starbite/libsb/sd.c) are refused (-9, nothing touched) outside a **fence** that only `store_home()` opens: the home
  card store's settings block (1 block) and its `crashlog` entry (16 blocks). Nothing else on the card can be written by
  the loader. `sd_rep.writes / write_errors / fenced` count them.
- **Fallback** (card only, no usable card store): the loader says why (`store_home_why()`: "NO OS STORE: NO MICROSD
  CARD", "NO OS STORE: THE MICROSD CARD DID NOT START", "NO OS STORE ON THE CARD"), in POST (STORE check: FAIL with that
  text) and pmsg, then opens **Starbite Recovery** with it. Its BOOT MENU (built-in catalog), loader fastboot over USB
  (`fastboot boot`, `oem log`), REBOOT TO TWRP and REBOOT TO DOWNLOAD MODE all work without a store. Rescue and
  Maintenance need their store entries and say "NO RESCUE SYSTEM: NOT FOUND". The UFS store is **not** used as a
  fallback: after the release it is iOS 27's data.
- Catalog alternatives (`"ufs:x|sd:x"`) already fall through to the card; `"ufs:"` finds nothing once the card is home.
- POST: STORE shows `CARD V2 n CRASHLOG`; MICROSD shows `CARD STORE V2 n HOME`.

Host test: `tools/loader-test/store_test.c` (in `tools/loader-test/run.sh`) runs the real store.c over fake media:
legacy unchanged, card home (UFS closed and never written, settings and crash log on the card, the fence exactly the
17 blocks), card only (zero UFS calls, also with no card or a blank card), the command-line parse.

## 3. Host tools

- `tools/store/card-store.sh migrate WORKDIR` (TWRP): pulls the live UFS store (store-pull.sh, every CRC) and its
  settings record; builds the home card store with **the same entries** (name, type, size, CRC32, flags compared one by
  one; volatile entries `crashlog` and `sbos-state` keep their bytes); the UFS settings record becomes the card's (card
  store flag on); writes the data pieces (16 MiB each, `conv=fsync`, SHA-256 read back on the phone), then the 7 head
  blocks last (until then the card's old header stays valid); verifies every piece again and pulls the store back
  (`cmp` with the image). **Writes only card p2; USERDATA is read only.**
- `card-store.sh flash IMG`: later updates. Build IMG with `--import` of a fresh `store-pull.sh OUT sd` (so volatile
  entries keep the phone's bytes): `sbstore2.py build NEW.img NAME=FILE ... --medium sd --home --data-at layout
  --import OUT.img`. Keeps the card's settings record; writes only the 16 MiB pieces that differ, head last.
- `card-store.sh verify IMG`: read-only comparison.
- `card-store.sh release --i-mean-it`: refuses unless the card holds a home store whose entries all pass their CRCs
  and BOOT contains `sbstore=card`; saves the old UFS store's blocks 0..6 to `build/store/ufs-store-head-*.bin`, then
  zeroes them. Every writer of the old store (old loaders such as the BOOT backup, Engineer OS and Starbite OS through
  libsb, sbstore-flash-linux.sh, the iOS overlay installer) needs a valid header there, so after this none of them can
  write into iOS 27's root.
- `tools/store/card-store-rehearsal.sh [LIVE.img]` (Lima VM): all of the above on the installer's fake phone, including
  "USERDATA unchanged by migrate", "p2 outside the head and the data area untouched", "flash keeps the card's settings".

## 4. Not covered here (follow-ups for their owners)

- **libsb** (Engineer OS, Starbite OS): `starbite/libsb/store.c` still looks only at the UFS store for settings,
  `store_append`, and Starbite OS's `sbos-state` slot (`sbos/kern/slot.c`, `sb_write_volatile(SB_MED_UFS, ...)`). Before
  the release they write the old UFS copy (the card copy goes stale); after the release they find no store (settings
  editor: no store; Starbite OS: "settings are not saved"). They should take the same home rule (the card store's
  SBSTHOME flag; libsb already has fenced card writes).
- `tools/linux/sbstore-flash*.sh`, `tools/overlay/sbstore-pull.py` and the XNU overlay installer read the UFS store.
- The card is read at 25 MHz, 4 bit (about 10 MB/s): a 38 MiB Android kernel takes about 4 s more than from UFS. The
  50 MHz high-speed mode (`sdhs`, master plan SD3) would halve that.
- sd64 cards: their store area is 64 MiB and they have no store data area yet.

## 5. Test plan (the main session runs it; the user present for the card-out step)

1. **Add the test row** to the live UFS store (the usual store-flash flow): entry `store-to-card-1.img` (the chain-test
   loader, card only) and the `store2card` row (bare, hidden, `no_launch`, `wdt_feed = "own"`). Done before step 2, so
   the card copy has the row too.
2. **Migrate** (TWRP): `tools/store/card-store.sh migrate build/store/card-migrate`. Expect `entries: all 54 equal`, every
   piece read back, "USERDATA untouched". Keep `build/store/card-migrate/card-store.img(.sha256)`.
3. **Boot the chain test** `store2card` from today's BOOT loader. Its stub arms a 75 s watchdog, fed by the menu's key
   loop and stopped before an OS starts. Expect POST `STORE: CARD V2 54 CRASHLOG`, `MICROSD: CARD STORE V2 54 HOME`,
   `SETTINGS OK`, and the menu with the same rows and default/last as before (settings copied from UFS).
4. **One boot of each kind** from the chained loader (power-cycle back to the BOOT loader, start `store2card` again
   between them): **bare** `utility`, **linux** `linux-shell` (or `linux-pmos`), **uefi** `uefishell`, **xnu** `mac13`. Each
   start loads its files from the card (CRC checked), writes the launch record to the card's crash log and the menu's
   last choice to the card's settings: the first loader card writes on the phone. Avoid `starbite-os*` and `engineer`
   until libsb follows the home store (section 4); no `android` at night.
5. After each boot, in the next chained loader's fastboot: `fastboot oem log crashraw` + `fastboot get_staged`, then
   `sbstore2.py crashlog FILE`: the previous start's launch record is there (it lives on the card now). `oem log pmsg`
   shows `STARBITE: launch KEY #n watchdog ...` without "(launch record NOT written)".
6. **Card out** (user present, optional): start `store2card` without the card. Expect Starbite Recovery with
   "NO OS STORE: NO MICROSD CARD"; BOOT MENU gives the built-in menu; the dead-man resets to the BOOT loader within 75 s.
   Card back in.
7. **Check the card** (TWRP): `tools/store/store-pull.sh build/store/card-after.img sd` (every CRC), `sbstore2.py
   settings-show` of p2 block 2, and `card-store.sh verify build/store/card-migrate/card-store.img`. Expect differences
   only in block 2 (settings) and in the crash log blocks.
8. **Only then the release**, with the user present: (a) BOOT with the card-only loader (`sbstore=card` on the payload
   command line), built from the verified BOOT backup and flashed only after this chain test passed; (b) in TWRP,
   `card-store.sh release --i-mean-it`; (c) USERDATA belongs to iOS 27 from then on.
