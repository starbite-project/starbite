# sbwddm-ng-0169: the row "Windows 11 (new GPU)" - the 0167 driver as its own entry, the other rows untouched

The user asked for the driver with the GPU fixes as a separate entry on the Starbite OS list (docs/WIN11-GPU.md
6.108). Both entries start the same Windows install; the new row differs only by `wdt = 80` (other Windows rows:
82). Windows' sbwdog.sys records that value at boot, and the adapter's user-mode driver becomes a small selector:

| file | what | sha256 |
|---|---|---|
| `sbsel.dll` | the selector (starbite/win/sbsel): `WtDat` = 80 s -> loads `sbumd-ng.dll`, anything else -> `sbumd.dll` | see SHA256SUMS |
| `sbumd-ng.dll` | sbumd 0167 (flip-model windows, GD at its 60 cap; measured T71, T79) | d39b83ab |
| `catalog-row.toml` | the row for the live catalog | |

`System32\sbumd.dll` (the user's T61 driver, 6d759c44) is never written; the standard rows load it through the
selector, byte for byte the file they load today. Kernel drivers are shared and unchanged.

## Install (phone in TWRP, SYSTEM hive clean)
```
S=~/project-starbite/build/win/stage; R=~/project-starbite/build/win/results-t83
tools/win/sbsel-install.sh show $R                         # before: UserModeDriverName = sbumd.dll
tools/win/sbsel-install.sh install $S/sbwddm-ng-0169 $R    # two files, then the hive value; sbumd.dll checked unchanged
```
The row: add `catalog-row.toml`'s entry to build/store/catalog-live.toml after `win11-big` and update the store
(tools/store/store-update.sh ... --catalog build/store/catalog-live.toml). **The catalog is full (32 of 32, the
loader's CAT_MAX): one existing row has to be dropped in the same update** (`--drop KEY`); which one is the main
session's / the user's choice. No new store file: `windows-big3i.uefi` is already there.

## Way back
```
tools/win/sbsel-install.sh restore $R     # UserModeDriverName = sbumd.dll: every row loads the driver directly again
```
and drop the `win11-ng` row at the next store update. The two files may stay in System32; nothing names them.

## What the new row is, honestly
- Games and flip-model windows: Geometry Dash at its 60 fps cap with GL kit 0169 in the game folder (the kit turns
  the fast present path on by itself on this row only); T71: flip, blt and DirectComposition windows all composed.
- Known problems of this driver (T78, open): from Microsoft Edge's start the desktop stalled (DWM GPU job timeouts),
  and Calculator, Notepad, Explorer came out black in screen shots. Hence the row's text
  "new GPU driver · games; some apps black". The standard rows do not have this driver and are not affected.
- The row signal depends on the store's watchdog setting being "per entry" (the default). With "off", or "one
  value for every entry" at anything but 80 s, the new row loads the standard driver: safe, but no different. With
  "one value for every entry" = 80 s EVERY Windows row with a watchdog would load the new driver: do not set that.
- If sbwdog.sys ever fails to start, its State key keeps the previous boot's value, and that boot follows the
  previous boot's row. sbwdog is boot-start and has started in every pulled boot.
- The runtime's unload of its user-mode driver now unloads only the selector; the real driver stays loaded in the
  process (it used to be unloaded and loaded fresh). T83b and every later test on the standard rows exercise that.
- A process that cannot read HKLM\SYSTEM\...\sbwdog\State (a tight sandbox) loads the standard driver on any row.
