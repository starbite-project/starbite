@echo off
rem sb-touchlat.bat (Track K T84, docs/WIN11-GPU.md 6.109), staged as gputest\sb-user.bat: how late does Windows
rem hand a touch to a mouse-only program (Geometry Dash)? sbtouchlat.exe injects taps, holds and drags into four
rem kinds of window and logs when the touch (WM_POINTERDOWN) and the promoted click (WM_LBUTTONDOWN) arrive.
rem Also: the user's touch settings, the tablet input services, and the touch driver's own registry values.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
cd /d %D%
echo TOUCHLAT logon reached %date% %time% >> %L%
ping -n 31 127.0.0.1 > nul
reg query "HKCU\Software\Microsoft\Wisp\Touch" >> %L% 2>&1
reg query "HKCU\Software\Microsoft\Wisp\Pen\SysEventParameters" >> %L% 2>&1
for %%S in (TabletInputService TextInputManagementService) do sc query %%S >> %L% 2>&1
tasklist /fi "imagename eq wisptis.exe" >> %L% 2>&1
tasklist /fi "imagename eq ctfmon.exe" >> %L% 2>&1
if exist %D%\sbtouchlat.exe (
  %D%\sbtouchlat.exe %L%
  echo TOUCHLAT sbtouchlat exit code !errorlevel! >> %L%
) else (
  echo TOUCHLAT no sbtouchlat.exe staged >> %L%
)
reg query HKLM\SYSTEM\CurrentControlSet\Services\S6SY761Touch > %U%\touch-service.txt 2>&1
echo TOUCHLAT end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite TOUCHLAT done"
exit /b 0
