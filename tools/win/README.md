# tools/win: Windows 11 ARM64 drivers and programs, built on the Mac

Step L0 of `docs/PLAN-2026-11-07.md`. No Windows machine, nothing uploaded: clang-cl + lld-link from Homebrew
LLVM, Microsoft's SDK/WDK/CRT headers and libraries from Microsoft's own servers, osslsigncode with the Starbite
test certificate. Target: the S9's Windows 11 23H2 (22631, ARM64, testsigning on), KMDF 1.33, UMDF 2.33.

| File | What |
|---|---|
| `setup.sh` | Downloads (pinned versions + sha256), extracts the ARM64 kit into `${SBWIN:-$HOME/sbwin}`, applies the clang fixups, checks the files other sessions need, prints sizes. Idempotent. |
| `env.sh` | `. tools/win/env.sh`: paths (`SBWIN_INC`, `SBWIN_LIB`, `SBWIN_WDF_INC`, ...) and tools (`SB_CLANG_CL`, `SB_LLD_LINK`, `SB_READOBJ`, ...). |
| `build.sh` | `build.sh kmdf\|wdm\|umdf\|exe\|dll OUT SRC...`: the compiler/linker flags for each kind. |
| `sign.sh` | Signs .sys/.dll/.exe/.cat in place with the Starbite test certificate and verifies. `-v` = verify only. |
| `makecat.py` | Makes a driver catalog (.cat) like Inf2Cat; `--selftest` reproduces an Inf2Cat catalog byte for byte. |
| `hive-service.py` | Adds/disables/removes a driver service in an offline SYSTEM hive copy (hivexsh in the lima VM). |
| `samples/` | `hello-kmdf` (the L0 test driver), `hello-exe`, `hello-umdf` (UMDF 2 / IddCx build check), `build-samples.sh`. |

## The kit: `$SBWIN` (stable path; the GPU, USB and Wi-Fi sessions build against it)

`SBWIN` defaults to `$HOME/sbwin` (= `/Users/dev/sbwin`). Keep it there; set `SBWIN` only to move the
whole kit (e.g. to iCloud Drive `~/Library/Mobile Documents/com~apple~CloudDocs/Starbite-overflow/wintools/` if
the SSD fills up; the SSD is faster). Installed 2026-11-07: 999 MB (Include 283 MB, Lib 452 MB, msvc 263 MB);
the archives are deleted after extraction (`KEEP_DL=1` keeps them). Nothing from it is committed.

| Package (pinned in setup.sh) | Version | sha256 |
|---|---|---|
| NuGet Microsoft.Windows.SDK.CPP | 10.0.26100.6584 | 5d31b382... |
| NuGet Microsoft.Windows.SDK.CPP.arm64 | 10.0.26100.6584 | ac069221... |
| NuGet Microsoft.Windows.WDK.ARM64 | 10.0.26100.6584 | e705b2a6... |
| VS 17.14.44: Microsoft.VC.14.44.17.14.CRT.Headers.base | 14.44.35220 | 852382a9... |
| VS 17.14.44: Microsoft.VC.14.44.17.14.CRT.ARM64.Desktop.base | 14.44.35226 | ba1aeca6... |
| VS 17.14.44: Microsoft.VC.14.44.17.14.CRT.ARM64.Store.base | 14.44.35226 | 57ece917... |

Licences: the user accepted the WDK/SDK/VS terms for this on 2026-11-07; the WDK licence text is kept in
`$SBWIN/licences/`. Only ARM64 libraries are extracted, no `.idl`, no C++/WinRT projection, no Windows tools.

```
$SBWIN/kit/Include/10.0.26100.0/{km,shared,um,ucrt,winrt}    # km/crt = the kernel CRT headers
$SBWIN/kit/Include/wdf/kmdf/1.15..1.35   $SBWIN/kit/Include/wdf/umdf/2.15..2.35
$SBWIN/kit/Lib/10.0.26100.0/{km,um,ucrt}/arm64               # all directory names lower case
$SBWIN/kit/Lib/wdf/kmdf/arm64/<ver>/{wdfdriverentry,wdfldr}.lib   $SBWIN/kit/Lib/wdf/umdf/arm64/<ver>/WdfDriverStubUm.lib
$SBWIN/msvc/include   $SBWIN/msvc/lib/arm64
```

`setup.sh` fails if any of these is missing (the list the sessions asked for): `km/wdm.h`, `km/ntddk.h`,
`km/ntifs.h`, `km/dispmprt.h`, `shared/d3dkmddi.h`, `shared/d3dkmdt.h`, `shared/d3dukmdt.h`, `km/usbfnioctl.h`,
`km/ufx/1.1/ufxclient.h`, `um/iddcx/1.4/IddCx.h`, KMDF 1.33 / UMDF 2.33 `wdf.h`; libs `km/arm64/ntoskrnl.lib`,
`hal.lib`, `displib.lib`, `wdmsec.lib`, `bufferoverflowfastfailk.lib`, `ufx/1.1/ufxstub.lib`, `um/arm64/kernel32.lib`,
`iddcx/1.4/iddcxstub.lib`, `ucrt/arm64/ucrt.lib`, the WDF stubs, `libcmt/libvcruntime/msvcrt.lib`. Also present:
NetCx, WiFiCx (`km/wificx`, `um/arm64/wificx`), UCX/UDE, ACX, `kusbfnclasslib`, IddCx 1.0-1.10, `shared/GenericUsbFnIoctl.h`.

The Mac volume is case-insensitive, so `#include <Windows.h>` vs `windows.h` and `WdfLdr.lib` vs `wdfldr.lib`
just work; the lib directories are still lower-cased so the kit also works on a case-sensitive volume (headers
are not; on such a volume add a case-folding overlay or use the exact names).

Clang fixups applied by setup.sh to our copy (MSVC accepts these, clang does not):
1. WDF in C++: `wdfdevice.h` forward-declares `enum _WDF_REQUEST_TYPE : int;`, `wdfrequest.h` defines it without
   `: int`. Each `wdfrequest.h` gets `WDF_SB_ENUM_INT` (`: int` in C++ only), marked `SBWIN fixup`.

## Building

```sh
tools/win/setup.sh                      # once (idempotent)
tools/win/samples/build-samples.sh      # builds + signs + checks the samples into build/win/samples/
tools/win/build.sh kmdf out/x.sys x.c   # your own
IDDCX=1.9 tools/win/build.sh umdf out/idd.dll idd.cpp
```

What `build.sh` does per kind (all: `--target=aarch64-pc-windows-msvc`, x18 stays reserved for the TEB/KPCR,
`-imsvc` for the kit so its warnings stay quiet, `/Brepro`, `NTDDI_VERSION=0x0A00000C` (Windows 11 22H2/23H2),
`-DSB_BUILD_ID="<git hash>[+]-<UTC time>"` unless `BUILD_ID` is set; extra `CFLAGS`, `CXXFLAGS`, `LDFLAGS`, `LIBS`;
`DEBUG=1` for /Od /Z7 + /debug):

- **kmdf / wdm** (kernel driver): `/kernel` (no RTTI/EH, `_KERNEL_MODE`), `/GS`, km + km/crt + shared headers;
  link `/driver /subsystem:native,10.00 /nodefaultlib /tsaware:no`, entry `FxDriverEntry` (kmdf, then
  `wdfldr.lib wdfdriverentry.lib`, KMDF 1.33) or `GsDriverEntry` (wdm); `bufferoverflowfastfailk.lib ntoskrnl.lib
  hal.lib wmilib.lib`. Two things lld-link does not do by itself that link.exe does:
  - `/include:_load_config_used`: the load config tells the kernel loader where `__security_cookie` is. Without
    it the cookie keeps its default and `__security_init_cookie` fast-fails (`brk #0xf003`) at load.
  - Section flags spelled out (`/section:.text,ERP` etc.): resident sections NOT_PAGED, `INIT` execute +
    discardable, exactly as in the WDK-built `Exynos9810Ufs.sys` (lld replaces all permission bits of a named section).
- **umdf** (UMDF 2 driver DLL): `/dll /subsystem:windows`, `WdfDriverStubUm.lib ntdll.lib onecoreuap.lib`,
  UMDF 2.33; `IDDCX=1.9` adds the IddCx headers and `iddcxstub.lib`. **IddCx headers are C++ only**: use .cpp.
  In C++, `DriverEntry` must be `extern "C"`.
- **exe / dll** (user mode): `/guard:cf`, "hybrid CRT": vcruntime/startup code static (`libcmt libvcruntime`), the
  UCRT from the in-box `ucrtbase.dll` (`ucrt.lib`), so nothing like vcruntime140.dll is needed on the phone.
- Sources are passed after `--` (clang-cl reads `/Users/...` as the `/U` option). `.rc` goes through llvm-rc.

Built and checked 2026-11-07 (`samples/build-samples.sh`, llvm-readobj):

| File | Size | Machine | Subsystem | Imports |
|---|---|---|---|---|
| hello-kmdf.sys | 12264 | ARM64 (0xAA64) | NATIVE (1) | ntoskrnl.exe, HAL.dll (KeGetCurrentIrql: on ARM64 it is a HAL export), WDFLDR.SYS |
| hello-exe.exe | 21480 | ARM64 | WINDOWS_CUI (3) | KERNEL32.dll, api-ms-win-crt-* (ucrtbase) |
| hello-umdf.dll / hello-idd.dll | 22504 | ARM64 | WINDOWS_GUI (2) | ntdll, KERNEL32, api-ms-win-crt-* |

hello-kmdf.sys sections .text 0x68000020, .rdata/.pdata 0x48000040, .data 0xC8000040, INIT 0x62000020 (= the
WDK-built driver), DLL characteristics DYNAMIC_BASE | HIGH_ENTROPY_VA | NX_COMPAT, load config with
SecurityCookie = `__security_cookie`. Not yet run on Windows (needs the phone, below).

## Signing

`tools/win/sign.sh FILE...` signs with SHA-256 Authenticode (embedded; no timestamp) and verifies against our
certificate. Key and certificate: `build/woa/ufs-sys/sign/starbite-test.{key,crt}` (`build/` is git-ignored; the key
is mode 600; never print or commit it; override with `SB_SIGN_KEY`/`SB_SIGN_CERT`). Certificate: self-signed
`CN=Starbite Test Signing`, CA:TRUE, EKU codeSigning, 2026-10-26 .. 2036-10-23, SHA-1 thumbprint
`334F75CA7DA45E8F676F3568522F2CFCDADDCC0E`. Windows on the S9 already loads a driver signed with it (the
re-signed boot-start `Exynos9810Ufs.sys`, NOTES.md 2026-10-27): with testsigning on, kernel code integrity takes
an embedded signature from any certificate.

### Catalogs (.cat): when they are needed

- **Loading a driver** (service key, or a PnP device whose driver is already registered): no catalog. The kernel
  checks the embedded signature; under testsigning ours is accepted.
- **Installing a driver package from its INF** (pnputil /add-driver, DISM, Device Manager, i.e. anything that puts
  it into the DriverStore): the INF must name a `CatalogFile=`, the catalog must hash every file of the package
  and be signed, and the signer must be trusted on that Windows (LocalMachine Root + TrustedPublisher), or the
  install is refused/prompted. Testsigning does not change this part.
- Making one on the Mac: `tools/win/makecat.py pkg/x.cat pkg/x.inf pkg/x.sys [--hwid root\starbitedisplay]`, then
  `tools/win/sign.sh pkg/x.cat`; `osslsigncode verify -CAfile <crt> -catalog pkg/x.cat -in pkg/x.sys` checks a file
  against it. Format checked: `makecat.py --selftest` rebuilds the member list (SHA-1 + SHA-256 members, PE and
  flat hashes, OSAttr/File/memberinfo2/SpcIndirectData attributes) and catalog attributes (OS, HWID1) of the
  Inf2Cat-made `build/woa/ufs-sys/Exynos9810Ufs.cat` byte for byte.

## Install on the phone (offline, nothing run on Windows) -- for the main session

Route for **non-PnP drivers** like hello-kmdf: put the .sys into `C:\Windows\System32\drivers\` from TWRP and add
a service key to the offline SYSTEM hive. This background session ran none of it (no adb/fastboot/sbfb/serial);
everything up to step 3 was rehearsed on the Mac with a copy of `build/woa/hive/SYSTEM-childcompletion0`
(add, update, disable, hiberboot 0, remove: the removed hive's Services tree is identical to the original).

Preconditions: Windows was **shut down cleanly** (Start > Power > Shut down; Shift+Shut down for a full shutdown
while fast startup is on), so ntfs-3g mounts read-write and the SYSTEM hive is reconciled. TWRP running.

1. **Mac: build and sign.** `tools/win/samples/build-samples.sh` -> `build/win/samples/hello-kmdf.sys` (+ SHA256SUMS).
2. **Phone (TWRP): mount and pull the hive.** Find the WINDOWS partition (sda27 on the old layout; check
   `ls -l /dev/block/by-name/` on layout v3), then
   ```sh
   adb shell 'mkdir -p /mnt/win && mount.ntfs -o rw /dev/block/by-name/WINDOWS /mnt/win && ls /mnt/win/Windows/System32/config/SYSTEM'
   adb pull /mnt/win/Windows/System32/config/SYSTEM build/win/hive/SYSTEM.pulled
   cp build/win/hive/SYSTEM.pulled backup/<date>-win-hives/SYSTEM      # the undo copy; keep it
   ```
   If mount.ntfs refuses read-write ("unclean"/"hibernated"): boot Windows, shut it down fully, retry. Do not force.
3. **Mac: edit the copy.**
   ```sh
   tools/win/hive-service.py build/win/hive/SYSTEM.pulled build/win/hive/SYSTEM.new add hello-kmdf
   tools/win/hive-service.py build/win/hive/SYSTEM.new build/win/hive/SYSTEM.new2 hiberboot 0   # optional: fast startup off
   ```
   It refuses a hive whose two sequence numbers differ (logs not reconciled = not cleanly shut down). The service:
   Type 1, Start 2 (auto: loaded by the Service Control Manager when the system is up and C: exists),
   ErrorControl 0 (a failure is ignored, no recovery), ImagePath `\SystemRoot\System32\drivers\hello-kmdf.sys`,
   `Parameters\OneShot = 1`. Fast startup off (HiberbootEnabled 0) makes every later shutdown a full one, so
   ntfs-3g never meets a hibernated volume.
4. **Phone: put both files in place and verify.**
   ```sh
   adb push build/win/samples/hello-kmdf.sys /mnt/win/Windows/System32/drivers/hello-kmdf.sys
   adb push build/win/hive/SYSTEM.new2 /mnt/win/Windows/System32/config/SYSTEM
   adb shell 'sha256sum /mnt/win/Windows/System32/drivers/hello-kmdf.sys /mnt/win/Windows/System32/config/SYSTEM; sync; umount /mnt/win'
   shasum -a 256 build/win/samples/hello-kmdf.sys build/win/hive/SYSTEM.new2    # must match
   ```
5. **Boot Windows** (boot menu, Windows). Expected: normal boot to the desktop; no visible sign of the driver.
   About when the desktop appears, the SCM loads hello-kmdf; DriverEntry first sets its own `Start` to 4 (flushed),
   then appends to `C:\starbite\hello-kmdf.txt`:
   `hello-kmdf build <git>-<time> loaded (one-shot, now Start=4) 2026-11-.. hh:mm:ss.mmm (UTC ..) KMDF 1.33 irql 0`
   and writes the same line as `Parameters\LastLoad` (+ `LastStatus` = the file write's NTSTATUS, 0 = ok).
6. **Read back**: shut Windows down, TWRP, `mount.ntfs -o ro ...`, `adb shell cat /mnt/win/starbite/hello-kmdf.txt`.
   Or pull the SYSTEM hive and `python3 tools/woa/hive.py SYSTEM 'ControlSet001\Services\hello-kmdf\Parameters'`.
   The build id in the line must match the one build-samples.sh printed. L0 is done when the line is there.

What can go wrong, and the way back:
- **No file, Start still 2, no LastLoad**: the driver was not loaded (signature refused, KMDF bind failed, file not
  found). Harmless (ErrorControl 0). The reason is in the System event log (Service Control Manager 7000/7026,
  CodeIntegrity): pull `Windows/System32/winevt/Logs/System.evtx` and `Microsoft-Windows-CodeIntegrity%4Operational.evtx`.
- **Bugcheck/reset during boot**: the one-shot already set Start 4 (the flush lands in the hive log, which winload
  replays), so the next boot skips the driver. If it crashed before that (inside the KMDF stub), from TWRP: delete
  `/mnt/win/Windows/System32/drivers/hello-kmdf.sys` (a missing file is ignored with ErrorControl 0), or put back the
  backup hive from step 2. After a crash the volume is dirty; ntfs-3g's default `recover` mode should still mount it
  read-write (untested here); if it refuses, use the raw-cluster route of NOTES.md 2026-10-26
  (tools/woa/ntfs-locate.py) for the 4 KiB of the hive holding `Start`.
- **Remove completely**: `hive-service.py IN OUT remove hello-kmdf` + delete the .sys. Re-run: `add` again (re-arms).

### PnP drivers (L1 USB function, L4 IddCx display)

- **In-box drivers through ACPI (best for L1)**: give the DWC3 controller a node in our firmware's DSDT whose
  `_HID` one of Windows' own INFs matches (list below); PnP then installs Microsoft's signed drivers by itself.
  Nothing of ours to sign, no catalog.
- **Our own PnP driver (L4 IddCx)**: an INF package (INF + signed .cat from makecat.py + signed binaries) has to
  enter the DriverStore once, plus a root-enumerated device node. Plan without typing on Windows: a one-shot
  LocalSystem service added offline (Type 0x10, ImagePath `cmd.exe /c C:\starbite\install.cmd`; SCM gives up on it
  after 30 s but the script runs) whose script does `certutil -addstore Root/TrustedPublisher starbite-test.cer`,
  `pnputil /add-driver C:\starbite\pkg\x.inf /install` and creates the root device (a small SetupAPI program built
  here, devcon-style: SetupDiCreateDeviceInfo + DIF_REGISTERDEVICE + UpdateDriverForPlugAndPlayDevices; the
  ARM64 WDK NuGet has no devcon/devgen). Not built yet; writing PnP state (Enum/Class/DriverDatabase) straight into
  the hive is possible in principle but too fragile.

## L1 research: USB function (device mode) stack in our Windows

Source: our image's own SYSTEM hive on the Mac, `build/woa/hive/SYSTEM-childcompletion0` (pulled 2026-10-26 from
the phone's Windows, before its first boot finished; the same in-box package set as the installed system) and
`backup/20261026-win-hives/SYSTEM`, read with tools/woa/hive.py. No Windows image (install.wim/esd, VHDX) is on
the Mac or in the lima VM: `starbite-win.vhdx` lives only on the phone's SD card.

**Yes, the whole USB function stack is in-box** (DriverDatabase\DriverPackages + ControlSet001\Services):

| Piece | Package / service | Notes |
|---|---|---|
| UFX class extension (ufxclient) | service `Ufx01000`, `system32\drivers\ufx01000.sys`, Start 3 | "USB Function Class Extension" |
| Synopsys DWC3 function controller | `ufxsynopsys.inf_arm64_a4c5aab063fa7da2`, service `ufxsynopsys` -> `\SystemRoot\System32\drivers\ufxsynopsys.sys`, DependOnService ufx01000 | matches **ACPI\INT3445** (plain), ACPI\QCOM2484 (needs Qualcomm's QcUsbFnSSFilter, not present), Intel PCI ids, URS\PNP0CA1&FUNCTION, URS\QCOM24B6&FUNCTION |
| Role switch (dual-role) | `urssynopsys.inf_arm64_75870528b4a286be`, service `UrsSynopsys` (+ `UrsCx01000`) | matches ACPI\PNP0CA1, ACPI\QCOM24B6; creates the URS\...&FUNCTION / &HOST children |
| Generic function driver | `genericusbfn.inf_arm64_9254286d4b432204`, service `genericusbfn` (DriverStore path) | matches USBFN\GENERICUSBFN, IpOverUSB, MTP, MainOSFlashing, UOSFlashing |
| USB function device class | `c_usbfn.inf_arm64_1259d691fb52c874` | |
| Also | `ufxchipidea.inf`, `UrsChipidea`, `UcmCx0101`/`UcmUcsiCx0101`/`UcmTcpciCx0101`, `winusb.inf`, `usbser.inf` | |

Headers in the kit: `km/ufx/1.1/ufxclient.h` + `km/arm64/ufx/1.1/ufxstub.lib` (to write our own UFX client
controller driver from libsb's DWC3 code if ufxsynopsys does not fit), `km/usbfnioctl.h`, `km/usbfnattach.h`,
`shared/usbfnbase.h`, `shared/GenericUsbFnIoctl.h` (the user-mode IOCTLs of GenericUSBFn), `km/kusbfnclasslib.h`.

What is missing: `HKLM\SYSTEM\CurrentControlSet\Control\USBFN` does not exist (no function configurations or
interfaces defined; on phones the OEM set them), and our DSDT (`build/woa/fw/Platform/Star2LtePkg/AcpiTables/`)
has no USB controller node (only the USB2 PHY/core prep switches in GpioHandoffDevices.asl). So the shortest L1
path is: DSDT node for the DWC3 (MMIO + interrupt; `_HID INT3445`, or PNP0CA1 for the URS route), a USBFN
configuration in the hive (offline, like hive-service.py: a GenericUSBFn interface with our VID/PID), then a
user-mode program on GenericUsbFnIoctl.h for the bulk pipes. Open questions for the phone: does ufxsynopsys accept
the Exynos9810 DWC3 without Intel/Qualcomm _DSM methods, and how it learns of VBUS attach (URS/UCM or a _DSM).
To confirm the files themselves on the installed system (read-only, TWRP): `ls /mnt/win/Windows/System32/drivers/ufx*
/mnt/win/Windows/System32/DriverStore/FileRepository/ | grep -i -E 'ufx|usbfn|urs'`.
