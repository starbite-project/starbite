@echo off
rem ===========================================================================
rem  Starbite WinPE / WinRE for the Galaxy S9 (run INSIDE the phone's Windows 11,
rem  as Administrator: right-click > Run as administrator). docs/WINPE-ARM64-PLAN.md
rem
rem    starbite-winpe.cmd re              WinRE (C:\Windows\System32\Recovery\Winre.wim)
rem    starbite-winpe.cmd pe D:\boot.wim  any WinPE/WinRE WIM (index 1), e.g. an ISO's boot.wim
rem    starbite-winpe.cmd transplant      after re/pe: put THIS Windows' winload/ntoskrnl/hal
rem                                       into the image (firmware build match, plan step B)
rem    starbite-winpe.cmd once            boot the WinPE entry on the next restart only
rem    starbite-winpe.cmd remove          delete the entry and C:\Starbite\WinPE
rem
rem  What it writes (nothing else): C:\Starbite\WinPE\{boot.wim,boot.sdi,versions.txt},
rem  a mount dir C:\Starbite\mnt (removed at the end), and two NEW BCD objects with
rem  fixed ids. The Windows 11 entry, the BCD default and the boot order stay as they are.
rem  Windows (DISM, bcdedit, reagentc) does all image servicing: ADK WinPE tools
rem  cannot run on macOS, but this Windows can.
rem ===========================================================================
setlocal enableextensions
set "PE={5b1e5d7a-2c61-4f0b-9e2a-535441525045}"
set "RO={5b1e5d7a-2c61-4f0b-9e2a-53544152524f}"
set "DIR=C:\Starbite\WinPE"
set "MNT=C:\Starbite\mnt"
set "LOG=%DIR%\starbite-winpe.log"

net session >nul 2>&1 || (echo Run this from an Administrator command prompt. & exit /b 1)
if /i "%~1"=="re" goto :re
if /i "%~1"=="pe" goto :pe
if /i "%~1"=="transplant" goto :transplant
if /i "%~1"=="once" goto :once
if /i "%~1"=="remove" goto :remove
findstr /b "rem " "%~f0" | more
exit /b 1

:re
rem WinRE is a WinPE with the touch-friendly "Choose an option" recovery UI.
set "SRC="
for %%F in ("C:\Windows\System32\Recovery\Winre.wim" "C:\Recovery\WindowsRE\Winre.wim") do if exist %%F if not defined SRC set "SRC=%%~F"
if not defined SRC (
    echo No Winre.wim found. "reagentc /info" shows where WinRE lives; with WinRE enabled,
    echo run "reagentc /disable" first, which moves Winre.wim back to C:\Windows\System32\Recovery.
    exit /b 1
)
call :build "%SRC%" re || exit /b 1
goto :bcd

:pe
if "%~2"=="" (echo usage: starbite-winpe.cmd pe PATH\boot.wim & exit /b 1)
if not exist "%~2" (echo %~2 not found & exit /b 1)
call :build "%~f2" pe || exit /b 1
goto :bcd

:build
rem %1 = source WIM, %2 = re|pe
if not exist "%DIR%" mkdir "%DIR%"
echo [%date% %time%] build from %~1 (%~2) > "%LOG%"
dism /English /Get-ImageInfo /ImageFile:"%~1" >> "%LOG%"
copy /y "%~1" "%DIR%\boot.wim" >nul || exit /b 1
attrib -r -h -s "%DIR%\boot.wim"
if not exist "C:\Windows\Boot\DVD\EFI\boot.sdi" (echo C:\Windows\Boot\DVD\EFI\boot.sdi missing & exit /b 1)
copy /y "C:\Windows\Boot\DVD\EFI\boot.sdi" "%DIR%\boot.sdi" >nul || exit /b 1
if exist "%MNT%" rmdir "%MNT%" 2>nul
mkdir "%MNT%"
dism /English /Mount-Image /ImageFile:"%DIR%\boot.wim" /Index:1 /MountDir:"%MNT%" >> "%LOG%" || (echo DISM mount failed, see %LOG% & exit /b 1)
rem The S9 drivers exactly as this Windows runs them (UFS with the S9 write fence,
rem HSI2C, S6SY761 touch). The UFS driver also takes over the firmware's boot
rem watchdog (RWD1): without it the phone resets about 80 s into WinPE.
set "NDRV=0"
for /d %%D in (C:\Windows\System32\DriverStore\FileRepository\exynos9810*.inf_arm64_* C:\Windows\System32\DriverStore\FileRepository\s6sy761*.inf_arm64_*) do (
    echo   driver %%~nxD
    dism /English /Image:"%MNT%" /Add-Driver /Driver:"%%D" /Recurse /ForceUnsigned >> "%LOG%" && set /a NDRV+=1
)
echo %NDRV% driver package(s) added.
if "%~2"=="pe" (
    rem Plain WinPE: a command prompt (wpeinit brings up PnP and the drivers).
    > "%MNT%\Windows\System32\startnet.cmd" (
        echo wpeinit
        echo echo Starbite WinPE ^(Galaxy S9^)
    )
)
call :versions
dism /English /Unmount-Image /MountDir:"%MNT%" /Commit >> "%LOG%" || (echo DISM commit failed, see %LOG% & exit /b 1)
rmdir "%MNT%" 2>nul
exit /b 0

:versions
rem The woa firmware patches winload and ntoskrnl for one build (22621.7582 / 22631.7584).
> "%DIR%\versions.txt" (
    for %%F in ("%MNT%\Windows\System32\Boot\winload.efi" "%MNT%\Windows\System32\ntoskrnl.exe" "%MNT%\Windows\System32\hal.dll" "C:\Windows\System32\Boot\winload.efi" "C:\Windows\System32\ntoskrnl.exe" "C:\Windows\System32\hal.dll") do (
        powershell -NoProfile -Command "$v=(Get-Item -LiteralPath '%%~F' -ErrorAction SilentlyContinue).VersionInfo; if($v){'{0}  {1}.{2}.{3}.{4}' -f '%%~F',$v.FileMajorPart,$v.FileMinorPart,$v.FileBuildPart,$v.FilePrivatePart}else{'%%~F  (missing)'}"
    )
)
type "%DIR%\versions.txt"
exit /b 0

:transplant
rem Plan step B: image kernel/loader != this Windows' (which the firmware is matched to).
if not exist "%DIR%\boot.wim" (echo run re or pe first & exit /b 1)
if exist "%MNT%" rmdir "%MNT%" 2>nul
mkdir "%MNT%"
dism /English /Mount-Image /ImageFile:"%DIR%\boot.wim" /Index:1 /MountDir:"%MNT%" >> "%LOG%" || (echo DISM mount failed & exit /b 1)
for %%P in ("Windows\System32\Boot\winload.efi" "Windows\System32\ntoskrnl.exe" "Windows\System32\hal.dll") do (
    if exist "%MNT%\%%~P" (
        takeown /f "%MNT%\%%~P" >nul && icacls "%MNT%\%%~P" /grant Administrators:F >nul
        copy /y "C:\%%~P" "%MNT%\%%~P" >nul && echo   replaced %%~P
    )
)
call :versions
dism /English /Unmount-Image /MountDir:"%MNT%" /Commit >> "%LOG%" || (echo DISM commit failed & exit /b 1)
rmdir "%MNT%" 2>nul
exit /b 0

:bcd
rem Ramdisk boot of C:\Starbite\WinPE\boot.wim; bcdedit stores C: GPT-qualified
rem (the WINDOWS partition), so it also works when WinPE itself is X:.
bcdedit /delete %PE% /f >nul 2>&1
bcdedit /delete %RO% /f >nul 2>&1
bcdedit /create %RO% /d "Starbite WinPE ramdisk" /device || exit /b 1
bcdedit /set %RO% ramdisksdidevice partition=C: || exit /b 1
bcdedit /set %RO% ramdisksdipath \Starbite\WinPE\boot.sdi || exit /b 1
bcdedit /create %PE% /d "Starbite WinPE" /application osloader || exit /b 1
bcdedit /set %PE% device ramdisk=[C:]\Starbite\WinPE\boot.wim,%RO% || exit /b 1
bcdedit /set %PE% osdevice ramdisk=[C:]\Starbite\WinPE\boot.wim,%RO% || exit /b 1
bcdedit /set %PE% path \Windows\System32\Boot\winload.efi || exit /b 1
bcdedit /set %PE% systemroot \Windows || exit /b 1
bcdedit /set %PE% winpe yes
bcdedit /set %PE% detecthal yes
rem Same platform settings as the Windows 11 entry (woa image build / our BCD).
bcdedit /set %PE% testsigning yes
bcdedit /set %PE% numproc 4
bcdedit /set %PE% nx OptIn
bcdedit /set %PE% hypervisorlaunchtype off
bcdedit /set %PE% bootstatuspolicy IgnoreAllFailures
bcdedit /set %PE% bootmenupolicy standard
bcdedit /displayorder %PE% /addlast
bcdedit /enum %PE%
echo.
echo Done. Boot it once with: starbite-winpe.cmd once  (then restart; pick Windows 11 in the
echo Starbite menu). The Windows 11 entry stays the default.
exit /b 0

:once
bcdedit /enum %PE% >nul 2>&1 || (echo no Starbite WinPE entry: run re or pe first & exit /b 1)
bcdedit /bootsequence %PE% || exit /b 1
echo Next boot through the Windows 11 firmware starts Starbite WinPE once. Restart now.
exit /b 0

:remove
bcdedit /delete %PE% /f
bcdedit /delete %RO% /f
if exist "%MNT%" dism /English /Unmount-Image /MountDir:"%MNT%" /Discard >nul 2>&1
rmdir /s /q "%MNT%" 2>nul
rmdir /s /q "%DIR%" 2>nul
echo removed.
exit /b 0
