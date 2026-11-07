# Starbite Display: installing the virtual monitor on the phone's Windows

Starbite Display is the extra monitor of Starbite Link (the DeX-like experience, `starbite/link/PROTOCOL.md`):
a UMDF 2 IddCx indirect display driver that gives the phone's Windows 11 (22631, ARM64) a second monitor. Windows
extends the desktop onto it; the Starbite Link sender captures that output with Desktop Duplication and streams
it to the Mac app, over USB or TCP. The driver itself is transport-independent and moves no pixels: its swap-chain
thread takes each frame DWM presents and hands it straight back (no copy, no polling).

Nothing in this file has been run on the phone yet. What is verified on the Mac is listed at the end.

## Files

| Source (`starbite/link/win/idd/`) | What |
|---|---|
| `src/StarbiteDisplay.cpp` | the driver (adapter, one monitor, modes, swap-chain thread, status values) |
| `src/sbmodes.h` | EDID + mode table (plain C++, also compiled on the Mac by the test) |
| `StarbiteDisplay.inf` | INF: class Display, hardware id `Root\StarbiteDisplay`, WUDFRd + IndirectKmd, UMDF service `StarbiteDisplay`, `UmdfExtensions = IddCx0102` |
| `tools/sbdevnode.c` | `sbdevnode.exe`: creates the root devnode and installs the INF on it (what `devcon install` did; the ARM64 WDK has no devcon) |
| `install.cmd` | route B script (certificate trust + sbdevnode + log) |
| `idd-hive.py` | route A: the same install written straight into offline SYSTEM + SOFTWARE hive copies |
| `test/modes-test.cpp` | host check of the EDID and the modes |
| `build.sh` | builds, signs and checks everything into `build/link/idd/` |

`starbite/link/win/idd/build.sh` produces (git-ignored):

```
build/link/idd/pkg/StarbiteDisplay.{dll,inf,cat}    the driver package (DLL + catalog signed with the Starbite test cert)
build/link/idd/sbdevnode.exe                        signed
build/link/idd/payload/                             = C:\starbite\idd\ for route B (pkg\, sbdevnode.exe, install.cmd, starbite-test.cer)
build/link/idd/SHA256SUMS
```

The monitor: EDID 1.4, manufacturer `SBT`, product 0x5B01 (Windows names the monitor devnode `MONITOR\SBT5B01`),
display name **"Starbite"** (an EDID name descriptor holds 13 characters, so the full "Starbite Display" is the
adapter's device name and the IddCx endpoint name instead), 53 x 30 cm (24", 100 % scaling at 1080p). Modes:
1920x1080, 1600x900, 1280x720 at 30 and 60 Hz; **1920x1080 @ 30 Hz preferred** (the phone renders in software).
`edid-decode --check` says "EDID conformity: PASS". For the sender: find the output through
`QueryDisplayConfig` + `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_TARGET_DEVICE_NAME)` (monitor name "Starbite",
EDID product 0x5B01), or `SbOsTargetId` below; it lives on the render adapter (`SbRenderLuid`, on the phone the
Microsoft Basic Render Driver).

## Two routes

- **Route A, fully offline (preferred while nothing can be typed on Windows):** TWRP + hive edits on the Mac,
  exactly what a pnputil install leaves behind that PnP and UMDF read at boot. No DriverStore entry, no catalog
  check, no certificate import. Rehearsed on the Mac (below); unproven on the phone.
- **Route B, the regular install:** `install.cmd` run once as LocalSystem, armed offline as a one-shot service
  (or typed into an elevated prompt once input works). Uses Windows' own SetupAPI/PnP, so it is the reference
  that route A imitates.

Use one route. If route A misbehaves, undo it (`remove`, below) before trying route B.

### Preconditions (both routes)

Windows **shut down cleanly** (fast startup off: `hive-service.py ... hiberboot 0`, tools/win/README.md), TWRP
running, the WINDOWS partition mounted read-write as in tools/win/README.md step 2:

```sh
adb shell 'mkdir -p /mnt/win && mount.ntfs -o rw /dev/block/by-name/WINDOWS /mnt/win'
```

Testsigning stays on (the DLL carries an embedded signature from the Starbite test certificate, like the
re-signed Exynos9810Ufs.sys that already loads).

## Route A: offline, nothing run on Windows

1. **Mac: build.** `starbite/link/win/idd/build.sh` (exit 0; prints the checks and SHA256SUMS).
2. **Phone -> Mac: pull both hives and keep undo copies.**
   ```sh
   adb pull /mnt/win/Windows/System32/config/SYSTEM   build/link/idd/hive/SYSTEM.pulled
   adb pull /mnt/win/Windows/System32/config/SOFTWARE build/link/idd/hive/SOFTWARE.pulled
   cp build/link/idd/hive/SYSTEM.pulled   backup/<date>-win-idd/SYSTEM
   cp build/link/idd/hive/SOFTWARE.pulled backup/<date>-win-idd/SOFTWARE
   ```
3. **Mac: edit the copies** (each refuses a dirty hive, never touches IN, and dumps what it wrote):
   ```sh
   starbite/link/win/idd/idd-hive.py system   build/link/idd/hive/SYSTEM.pulled   build/link/idd/hive/SYSTEM.new   install
   starbite/link/win/idd/idd-hive.py software build/link/idd/hive/SOFTWARE.pulled build/link/idd/hive/SOFTWARE.new install
   ```
4. **Mac -> phone: the DLL, the INF (for reference only), the two hives; check hashes.**
   ```sh
   adb push build/link/idd/pkg/StarbiteDisplay.dll /mnt/win/Windows/System32/drivers/UMDF/StarbiteDisplay.dll
   adb push build/link/idd/pkg/StarbiteDisplay.inf /mnt/win/Windows/INF/StarbiteDisplay.inf
   adb push build/link/idd/hive/SYSTEM.new   /mnt/win/Windows/System32/config/SYSTEM
   adb push build/link/idd/hive/SOFTWARE.new /mnt/win/Windows/System32/config/SOFTWARE
   adb shell 'cd /mnt/win/Windows/System32 && sha256sum drivers/UMDF/StarbiteDisplay.dll config/SYSTEM config/SOFTWARE; sync; umount /mnt/win'
   shasum -a 256 build/link/idd/pkg/StarbiteDisplay.dll build/link/idd/hive/SYSTEM.new build/link/idd/hive/SOFTWARE.new
   ```
5. **Boot Windows.** Expected: the phone's own screen as before, plus Windows now has two displays (Settings >
   System > Display: a second display "Starbite", 1920x1080, extended to the right of the main one). Nothing is
   visible on the phone except that the mouse pointer can leave the right edge. A new display is normally
   extended (Windows' default for a display combination it has never seen); if it came up duplicated or as the
   only display, Win+P switches, and the undo below restores the old state.
6. **Read back** after a clean shutdown: pull SYSTEM again, then
   `starbite/link/win/idd/idd-hive.py status build/link/idd/hive/SYSTEM.after` (read only, no VM). It dumps the
   device key, everything PnP added under it (`Properties`, the monitor child's container, `LogConf`...) and the
   driver's own status values (below).

### Exactly what route A writes

SYSTEM hive (`ControlSet001` = the current control set, from `Select\Current`):

| Key | Value | Type | Data | Why |
|---|---|---|---|---|
| `Enum\Root\StarbiteDisplay\0000` | HardwareID | MULTI_SZ | `Root\StarbiteDisplay` | the root enumerator reports this device at every boot |
| | Service | SZ | `WUDFRd` | function driver = the UMDF reflector (`Services\WUDFRd` is in-box) |
| | UpperFilters | MULTI_SZ | `IndirectKmd` | the kernel half of IddCx (`Services\IndirectKmd` is in-box); from the INF's `.HW` AddReg |
| | ClassGUID / Class | SZ | `{4d36e968-e325-11ce-bfc1-08002be10318}` / `Display` | display adapter class |
| | Driver | SZ | `{4d36e968-...}\NNNN` | points at the driver key below; with ConfigFlags 0 it marks the device as installed |
| | ConfigFlags | DWORD | 0 | installed, no reinstall/finish-install pending |
| | DeviceDesc / Mfg | SZ | `Starbite Display` / `Starbite` | names |
| | Capabilities | DWORD | 0 | as on `Root\BasicDisplay\0000` |
| | ContainerID | SZ | `{00000000-0000-0000-FFFF-FFFFFFFFFFFF}` | as on the other root devices |
| `...\0000\Device Parameters\WUDF` | DriverList | MULTI_SZ | `StarbiteDisplay` | the INF's `UmdfServiceOrder`: which UMDF driver(s) WUDFRd loads for this device |
| | KernelModeClientPolicy | DWORD | 1 | `UmdfKernelModeClientPolicy = AllowKernelModeClients` (IndirectKmd talks to it) |
| | FileObjectPolicy / MethodNeitherAction / FsContextUsePolicy | DWORD | 1 / 1 / 1 | `UmdfFileObjectPolicy = AllowNullAndUnknownFileObjects`, `UmdfMethodNeitherAction = Copy`, `UmdfFsContextUsePolicy = CanUseFsContext2` (the IddCx INF set; values as hidtelephony's in the phone's DriverDatabase) |
| | HostProcessSharingEnabled | DWORD | 0 | `UmdfHostProcessSharing = ProcessSharingDisabled`: own WUDFHost process |
| `Control\Class\{4d36e968-...}\NNNN` (first free; 0000 on the hive of 2026-11-07, which has no display-class device yet) | DriverDesc, ProviderName, DriverVersion, DriverDate, DriverDateData, InfPath, InfSection, MatchingDeviceId | SZ (DateData QWORD FILETIME) | `Starbite Display`, `Starbite`, `0.1.0.0`, `11-7-2026`, 2026-11-07, `StarbiteDisplay.inf`, `StarbiteDisplay_Install`, `root\starbitedisplay` | the device's driver (software) key, as SetupAPI writes it |

SOFTWARE hive:

| Key | Value | Type | Data |
|---|---|---|---|
| `Microsoft\Windows NT\CurrentVersion\WUDF\Services\StarbiteDisplay` | WdfMajorVersion | DWORD | 2 |
| | WdfMinorVersion | DWORD | 33 (0x21; the DLL is built against UMDF 2.33) |
| | ImagePath | EXPAND_SZ | `%SystemRoot%\System32\drivers\UMDF\StarbiteDisplay.dll` |
| | WdfExtensions | MULTI_SZ | `IddCx0102` (the INF's `UmdfExtensions`: binds the IddCx class extension) |

File: `C:\Windows\System32\drivers\UMDF\StarbiteDisplay.dll`.

Where these come from (read from the phone's own hives of 2026-11-07, `build/win/results-combo1/`): the UMDF service
registration lives in the **SOFTWARE** hive, not under `SYSTEM\...\Services`: the in-box IddCx driver `RdpIdd` has
exactly `WUDF\Services\RdpIdd` {WdfMajorVersion 2, WdfMinorVersion, ImagePath `...\drivers\UMDF\RdpIdd.dll`,
WdfExtensions `IddCx0102`}, and the class extension itself is `WUDF\Services\IddCx0102` (`...\UMDF\IddCx.dll`).
The per-device UMDF settings (`Device Parameters\WUDF\DriverList`, `KernelModeClientPolicy`,
`HostProcessSharingEnabled`) are what the DriverDatabase records for the in-box UMDF 2 packages (hidtelephony,
hidlamparray, xinputhid's BLE configuration) under their `Device\WUDF` configuration key. There is no
`Services\<name>\Parameters\Wdf` for a UMDF driver (that is the KMDF layout) and no `Services\StarbiteDisplay`:
`WUDFRd` and `IndirectKmd` are the only kernel services involved, both already in-box (Start 3).

Not written, on purpose: the DriverStore (`DriverDatabase\DriverPackages`, `FileRepository`), the device's
`Properties` (PnP fills in the ones it needs), any `oemNN.inf`. A later route B install over a route A device
would be confused by these gaps, hence "one route".

### Undo route A (from TWRP)

```sh
starbite/link/win/idd/idd-hive.py system   <SYSTEM pulled now>   <SYSTEM.undo>   remove   # device key + its class key
starbite/link/win/idd/idd-hive.py software <SOFTWARE pulled now> <SOFTWARE.undo> remove   # the UMDF service
```
push both back and delete `Windows/System32/drivers/UMDF/StarbiteDisplay.dll` (rehearsed: after install +
remove, `Enum\Root`, `Control\Class\{4d36e968...}` and `WUDF` are identical to the original, value for value).
Quicker, if the desktop is unusable: delete only the DLL; the device then fails to start (a problem code in
Device Manager, no monitor), which cannot affect boot (IddCx devices are not boot-critical; WUDFRd ErrorControl 1
means log and continue). Last resort: push the backup hives from step 2.

## Route B: regular PnP install (one-shot service, or by hand)

1. **Mac: build** (as above). `build/link/idd/payload/` is the whole payload.
2. **Phone: copy the payload** to `C:\starbite\idd\`:
   `adb shell mkdir -p /mnt/win/starbite/idd && adb push build/link/idd/payload/. /mnt/win/starbite/idd/`
3. **Mac: arm a one-shot LocalSystem service** in a SYSTEM hive copy (tools/win/README.md "PnP drivers" plan):
   ```sh
   tools/win/hive-service.py SYSTEM.pulled SYSTEM.new add sbiddinst --type 16 --start 2 \
       --image '%SystemRoot%\System32\cmd.exe /c C:\starbite\idd\install.cmd' --display 'Starbite Display install (one shot)'
   ```
   (Type 0x10 = own-process service; the Service Control Manager starts cmd.exe as LocalSystem at boot, gives
   up waiting after ~30 s, but the script keeps running. `--oneshot` only adds an unused Parameters value.)
   Push SYSTEM.new back, boot Windows, wait ~2 minutes, shut down cleanly.
4. **Read back:** `C:\starbite\idd\install.log` (certutil, `sbdevnode: device ROOT\STARBITEDISPLAY\0000 ...`,
   `driver installed`, `sbdevnode exit code 0`, then `pnputil /enum-devices` of the Display and Monitor classes),
   and `C:\Windows\INF\setupapi.dev.log` (search `starbitedisplay`). The script sets the service's Start to 4
   itself; also remove it: `hive-service.py ... remove sbiddinst`.

What `install.cmd` does: `certutil -addstore Root` and `TrustedPublisher` with `starbite-test.cer` (a DriverStore
install needs a trusted signer for the signed catalog; testsigning does not cover that part), then
`sbdevnode install C:\starbite\idd\pkg\StarbiteDisplay.inf Root\StarbiteDisplay`:
SetupDiCreateDeviceInfo (`ROOT\STARBITEDISPLAY\NNNN`) + hardware id + DIF_REGISTERDEVICE (only if no device with
that hardware id exists: idempotent), then UpdateDriverForPlugAndPlayDevices(INSTALLFLAG_FORCE), which puts the
package into the DriverStore and starts the device.

By hand, once a keyboard/mouse (or Starbite Link input) works, from an elevated prompt: `C:\starbite\idd\install.cmd`,
or the pieces: `pnputil /add-driver C:\starbite\idd\pkg\StarbiteDisplay.inf /install` stages the package, but
pnputil on 22631 has no command that creates a root-enumerated devnode, so `sbdevnode install ...` (or `devcon install
StarbiteDisplay.inf Root\StarbiteDisplay` from an x64/ARM64 WDK, if one ever reaches the phone) is still needed.
Remove: `sbdevnode remove Root\StarbiteDisplay`, then `pnputil /delete-driver oemNN.inf /uninstall`
(`pnputil /enum-drivers` shows NN).

## Status values (what the driver reports, readable offline)

The driver writes `Sb*` values into its writable per-device registry key (`WdfDeviceOpenRegistryKey(
PLUGPLAY_REGKEY_DEVICE | WDF_REGKEY_DEVICE_SUBKEY)`, a subkey below `Enum\Root\StarbiteDisplay\0000\Device
Parameters`; `idd-hive.py status` dumps the whole tree, so the exact subkey name does not matter). Only on state
changes and every 1024 frames.

| Value | Meaning |
|---|---|
| SbBuild | build id (`<git hash>[+]-<UTC time>`), to match against build.sh's output |
| SbState | last state: `device added`, `adapter init started` / `refused` / `failed`, `monitor arrived`, `swapchain: running` / `SetDevice failed` / `stopped` / `lost` |
| SbDeviceInitStatus, SbAdapterInitAsyncStatus, SbAdapterInitStatus, SbMonitorCreateStatus, SbMonitorArrivalStatus | NTSTATUS of each step (0 = ok) |
| SbOsTargetId, SbOsAdapterLuid | the monitor's target id and the IddCx adapter's LUID (for QueryDisplayConfig) |
| SbMode | the committed mode, e.g. `1920x1080@30`, or `inactive` |
| SbSwapChains | swap chains assigned since load (1 per mode set / display (re)activation) |
| SbRenderLuid, SbSetDeviceHr | render adapter LUID and the HRESULT of IddCxSwapChainSetDevice |
| SbFrames, SbLoopExitHr | frames acquired+released by the last swap chain, and why its loop ended |

No value at all = the DLL never ran: check `System.evtx` (UserPnp / Kernel-PnP device events, and the
DriverFrameworks-UserMode / WUDFRd events about the host failing to load the driver),
`Microsoft-Windows-CodeIntegrity%4Operational.evtx` (signature refusals) and `C:\Windows\INF\setupapi.dev.log`.

## Verified on the Mac vs. phone-only

Verified (2026-11-07, `build.sh`):
- `StarbiteDisplay.dll`: ARM64 (0xAA64), subsystem WINDOWS_GUI, exactly one export `FxDriverEntryUm` (the UMDF 2
  entry from WdfDriverStubUm.lib, which then calls our `DriverEntry`), the IddCx class binding strings
  (`IddCx`, `WUDFx02000`) present; imports only in-box DLLs (dxgi, d3d11, kernel32, avrt, ntdll, api-ms-win-crt-*
  = ucrtbase). Built with IddCx 1.4 headers/stub (minimum IddCx 1.4 = Windows 10 1903; 22631 has 1.9) and UMDF 2.33.
- Signed with the Starbite test certificate (osslsigncode verify ok), the catalog lists the signed DLL
  (osslsigncode `-catalog` ok) and the INF (its SHA-1 and SHA-256 flat hashes), and is signed.
- EDID: host test (checksum, decoding of both detailed timings against the mode table, standard timings, range
  limits, name) and `edid-decode --check`: conformity PASS.
- `idd-hive.py`: install + remove rehearsed on copies of the phone's SYSTEM and SOFTWARE hives of 2026-11-07
  (`build/win/results-combo1/`): hivexsh reads the new keys back; after remove the touched subtrees are identical
  to the originals.

Phone-only (not verified, in the order they can fail):
1. Route A's premise: that PnP starts a root device whose keys were written offline without a DriverStore entry.
   The keys are the documented/observed ones, but PnP could still decide the device needs installing (then:
   Device Manager problem, no harm) -> route B.
2. UMDF loading a DLL with only an embedded test signature (no catalog in route A) under testsigning.
3. IddCx with the Microsoft Basic Render Driver (WARP) as the only render adapter: adapter init, swap-chain
   SetDevice on a WARP D3D11 device.
4. Desktop Duplication of an indirect-display output on WARP (the sender's side, but it depends on this).
5. Windows extending (not duplicating) onto the new display by default; the 30 Hz preferred mode being picked.
6. The INF itself (no InfVerif on the Mac): `ClassVer = 2.0`, the `.Wdf` directives, and sbdevnode, only matter
   for route B.
