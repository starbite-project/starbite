# UEFI Shell entry (Microsoft category, Lab)

Status 2026-11-03: built, not yet run on the phone.

## What it is

`uefishell.uefi` is a store image in the same format as `windows.uefi`: the woa bootshim plus a
7 MiB Star2LtePkg FD. The loader starts it through the `uefi` method (xnu_boot.c `boot_uefi`:
"ARMd" magic at +56, the FD is copied to 0x90000000, as with `windows.uefi`).

It is our source build of the woa UEFI, built the way the W2 builds were (those reached BDS and ran an
EFI Shell from UFS: NOTES.md 2026-10-25 23:40). The difference is that its BDS starts the **edk2 UEFI
Shell embedded in the FV** and never registers or starts Windows:

| Part | Source |
|---|---|
| SEC/PEI + DXE | `ref/exynos9810-woa/firmware` + `tools/woa/patches` 0001 (no TWRP divert), 0002 (SEC without `dc zva`), 0003 (DEBUG output to the pstore ring), 0005 (EndOfDxe), 0018 (no RamManager), 0022 (RAM stages, default 0), 0023 (PMIC-seeded RTC) |
| Shell entry | `tools/woa/patches-shell/0024-starbite-shell-entry.patch`: DSC/FDF switch `STAR2LTE_BOOT_SHELL` (default 0, so nothing changes for other builds), `Star2LteKeysDxe`, DevicePathDxe, the Shell binary under gUefiShellFileGuid `7C04A583-9E3E-4F1C-AD65-E05268D0B4D1`, BDS `Star2LteBootShell()` |
| Shell | edk2 2938b830 `ShellPkg/ShellPkg.dsc`, RELEASE CLANGPDB, the "all commands" variant (FILE_GUID EA4BB293..., `acpiview` included) |
| Keys | `Star2LteKeysDxe`: GPIO_ALIVE GPA0 bits 3/4/6 and GPA2 bit 4 (the same reads as `shim/starlte/menu.c keys()`), sampled every 15 ms, debounced. The ALIVE page is added to the GCD as uncached MMIO (it is not in the platform MMU map) |

The release modules (patches 0019-0021) are left out because this build needs our own BDS. Our DXE set
stalls winload (the "gmm freeze"), but that does not matter here because Windows is never started.

### Keys in the shell

There is no keyboard (no USB host stack in UEFI, touch is not an input device here), so the keys
drive a **command palette**:

- **Volume Down / Volume Up**: clear the line (ESC) and type the next / previous palette command
  (repeats while held). The position is kept across commands; only the first press of a boot
  starts from an unset position (Down: `ver`, Up: `reset`).
- **Power**: Enter (runs the command, continues at a `-b` page break or a `pause`).
- **Bixby**: `q` (stops a `-b` listing).

The palette has only read-only commands: `ver`, `map -b`, `memmap -b`, `acpiview -l`,
`acpiview -s APIC|GTDT|PPTT|FACP|DBG2`, `smbiosview -b`, `dmem 0x14050000 0x80`, `drivers -b`,
`devtree -b`, `dh -b`, `dmpstore -b`, `ls -b fs0:\`, `ls -b fs0:\EFI`, `vol fs0:`, `date`, `time`,
`help -b`, and `reset` last (back to S-Boot and the Starbite menu). A `startup.nsh` in
`FS0:\` or `FS0:\EFI\tools\` (FS0 should be WINESP, the only FAT volume) runs after the usual 5 s
countdown, so scripts can be added later without rebuilding.

## Build

```
tools/woa/build-shell-uefi.sh            # RAM stage 0 (bank 0, 711 MiB); "... 2" for 1.4 GiB
```

It runs in lima: edk2 ~/work/edk2 @2938b830 + patches-edk2, its own output directory
`Build/Star2LteShell` (so `build-uefi.sh` output is untouched). It refuses to run while another edk2
build is running there.

**Shared-tree hazard:** like `build-uefi.sh`, it resets lima's `~/work/edk2` with
`git checkout -- .` before applying `tools/woa/patches-edk2`. That throws away any uncommitted edk2
edits another workflow has there; the pgrep guard only catches builds that are running. The script
now saves the tracked diff to `~/work/edk2-discarded-<date>.diff` (in lima) before the reset, but
untracked files are not saved, and `build-uefi.sh` has no such safeguard: check with whoever else
uses `~/work/edk2` before building, and commit or stash edk2 work you want to keep. Outputs are in `build/woa/shell/`:

| File | sha256 |
|---|---|
| `uefishell.uefi` (store entry, 7340416 B) | `c29f534313c7436085c44c42213ebf7dd64378a12444a53335d20487dd7b801a` |
| `Star2LteShell.fd` | `7ff2f3bea83ea504b04c2c1d847f001093233d54adc5ebba7ece1a605987758c` |
| `Shell-full.efi` (edk2 Shell, 1139712 B) | `e9cc4a8e2d5363e07e7429e05771ba0a93f05f819b892ddd611e08ecc1c88aa6` |
| `bootshim.bin` | `46c61695ab8c5fb42f23594d35def338f14a8eb9cbe038469853edeca74530dd` |

The build checks the FD size, the ARMd magic, and that FVMAIN.inf lists the Shell file (GUID
7C04A583), `Star2LteKeysDxe` (the only input device) and `DevicePathDxe`. The Shell
binary is reproducible from edk2. The FD includes a build timestamp, so its sha256 changes with each build.

## Proposed catalog entry (not added: catalog.toml belongs to the menu work)

```toml
[[entry]]
key = "uefishell"
name = "UEFI Shell"
version = "EDK2 · Shell 2.2"
category = "microsoft"
method = "uefi"
tag = "lab"
files = ["uefishell.uefi"]
root = "-"
```

## Test on the phone (when it is free)

Key combos (no keyboard): **forced reset** = hold Volume Down + Power about 7 s until the screen
goes off; **TWRP** = from a forced reset (or from off), let go of Volume Down the moment the screen
goes off and hold Volume Up + Bixby + Power until the TWRP logo. A plain reset (forced, `reset`,
or the shell's 10 s auto-reset) goes through S-Boot back to the Starbite menu, not to TWRP.

1. Store, built from a **fresh pull of the phone's store**, never from the Mac copy
   `build/store/ufs-store.img` (other workflows flash the store; store-flash.sh replaces the whole
   store area, so a stale import silently rolls their entries back). In TWRP, in one sitting:
   ```
   tools/store/store-pull.sh build/store/ufs-store-live.img     # read-only; lists, CRC-checks, compares the catalog
   cp tools/store/catalog.toml build/store/catalog-shell.toml    # only if store-pull says the live catalog == catalog.toml
   cat tools/woa/catalog-uefishell.toml >> build/store/catalog-shell.toml
   python3 tools/store/sbstore2.py build build/store/ufs-store-shell.img --medium ufs \
       --catalog build/store/catalog-shell.toml --import build/store/ufs-store-live.img \
       uefishell.uefi=build/woa/shell/uefishell.uefi
   diff <(python3 tools/store/sbstore2.py list build/store/ufs-store-live.img) \
        <(python3 tools/store/sbstore2.py list build/store/ufs-store-shell.img)
   diff <(python3 tools/store/sbstore2.py catalog-keys build/store/ufs-store-live.img) \
        <(python3 tools/store/sbstore2.py catalog-keys build/store/ufs-store-shell.img)
   tools/store/store-flash.sh build/store/ufs-store-shell.img
   ```
   (The entry is the committed file `tools/woa/catalog-uefishell.toml`, the same as the proposed
   entry above, so the block has no heredoc and pastes from the raw markdown too.)
   The first diff must show only the changed `catalog` line and the added `uefishell.uefi`; the
   second only `> uefishell`. Nothing else removed or changed. If store-pull.sh prints **STOP**
   (the live catalog differs from catalog.toml), do not go on: find the TOML the live catalog was
   built from (ask whoever flashed it) and append the entry to a copy of that instead.
   `sbstore2.py build` refuses a `--catalog` that drops a key the imported catalog has. store-flash.sh keeps the settings block and verifies by SHA-256. (A dry run with
   catalog.toml + this entry packs 10 entries, 82.4 MiB of 721024 blocks.)
2. `adb reboot` from TWRP; in the menu pick **UEFI Shell**. Expected: "LOADING UEFI", then the woa UEFI text, then a cleared
   screen with the Starbite key hint line, the Shell banner and the mapping table (all LU0 partitions
   as BLKx, WINESP as FS0), the 5 s startup countdown, and then `Shell>`.
   During the 5 s countdown any Volume key sends ESC first, which skips `startup.nsh`; that is
   harmless (none exists yet).
3. Press Volume Down: `ver` appears on the prompt line. Press Power to run it. Volume Down then
   steps `map -b`, `memmap -b`, `acpiview -l`, `acpiview -s APIC` (the MADT our source publishes);
   run each with Power. The palette position is one global index in Star2LteKeysDxe: it starts
   unset only before the very first Volume press of the boot, and after that every press moves ±1
   from the last command typed; running a command does not reset it. Mind **Volume Up** as the
   *first* press of the boot: from the unset position it jumps to the *last* entry, which is
   `reset`, so Volume Up first and then Power resets the phone. Volume Up later steps backwards
   through the list (wrapping from `ver` to `reset`). Always read the prompt line before pressing Power.
4. Run `reset` to get back to the menu: the position persists from step 3, so press Volume Up until
   the prompt line reads `reset` (from `acpiview -s APIC` that is 5 presses: `acpiview -l`,
   `memmap -b`, `map -b`, `ver`, then wrapping to `reset`), or Volume Down to the end of the list
   (17 presses from there). Check the line, then press Power.
5. Read the ring afterwards from TWRP: in the menu after the reset, do a forced reset and go to
   TWRP with the key combo above (the ring survives a warm reset, and TWRP's kernel reads it before
   reusing it, so do not boot anything else in between), then
   `adb exec-out cat /sys/fs/pstore/pmsg-ramoops-0`. Look for
   `=KEYCON n=01` (the keys console registered), `=SHELL run=00 load=S0000`, and on exit
   `=SHELLRET`. "Star2LteKeys: installed" shows in the DEBUG mirror.

If the shell never comes up, it prints "UEFI Shell could not be started" and resets to the menu
after 10 s. On a hang (frozen screen, no reset after about a minute) the only way out is the forced
reset, then TWRP for the ring as in step 5.

If the shell starts but the keys do nothing (so `reset` cannot be typed either): forced reset, then
TWRP. `=KEYCON n=00` means the driver failed. The DEBUG line
then gives the GCD status. An `X ... FAR 14050024` exception would mean the ALIVE page mapping did
not take. If nothing appears after the UEFI text: compare with W2's src12 ring. The only BDS change
is that `Star2LteBootShell` runs right after the UFS connect.

## Booting the Windows UEFI (src38) straight into a shell

This is not practical. The release DXE volume carries its own binary BdsDxe, which boots the first
filesystem holding `\efi\microsoft\boot\bootmgfw.efi` or `\efi\boot\bootaa64.efi`. That is WINESP, so
the Shell would have to replace the Windows boot manager there, and the release's boot watchdog
(armed for Windows, about 80 s) would reset the phone during the Shell session. A separate image is
cleaner.

## Blockers / open points

- Not run on hardware yet (the phone is busy with other work).
- The GraphicsConsole uses the 8x19 font on a 1440x2960 panel (about 180x155 characters), so the text
  is small. A bigger font needs a custom HII font package (later).
- There is no free text input: only palette commands and scripts on FS0.
