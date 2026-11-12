@echo off
rem c-clean-win.cmd (docs/WIN11-GPU-CAPS.md "C: space"): free space on C: from inside Windows, elevated. Started once
rem by the HKLM RunOnce value "StarbiteCleanup" (written offline by tools/win/c-clean-stage.sh) at the next logon,
rem or by hand: right-click > Run as administrator. Log: C:\starbite\cleanup\c-clean-win.log. Runs once (done marker).
rem   1. CompactOS (compact /compactos:always): Windows' own binaries stored compressed. Reversible
rem      (compact /compactos:never). Expect 1.5-3 GB.
rem   2. DISM StartComponentCleanup: superseded components in WinSxS (11.3 GB here). Expect 0.3-1.5 GB.
rem   3. Only if C: still has under 4.5 GB free: DISM /ResetBase. Expect 0.5-2 GB more. IRREVERSIBLE: the updates
rem      installed now can no longer be uninstalled.
rem   4. Delivery Optimization cache, this account's Temp and Windows\Temp. Small.
rem   Not touched: apps, Steam, games, user files, the pagefile (stays Windows-managed: it grows into the space this
rem   frees), reserved storage (not active on this install), hibernation (already off).
setlocal EnableDelayedExpansion
set D=C:\starbite\cleanup
set L=%D%\c-clean-win.log
if not exist %D% mkdir %D%
echo ==== c-clean-win start %date% %time% >> %L%
net session >nul 2>&1
if errorlevel 1 (
  echo NOT ELEVATED: nothing done. Right-click c-clean-win.cmd and choose Run as administrator. >> %L%
  echo Not elevated: right-click this file and choose "Run as administrator".
  exit /b 1
)
if exist %D%\done (echo already done before: %D%\done >> %L% & exit /b 0)
echo running elevated >> %L%
call :free start
echo ---- 1. CompactOS >> %L%
compact /compactos:query >> %L% 2>&1
compact /compactos:always >> %L% 2>&1
echo compactos exit %errorlevel% >> %L%
call :free after-compactos
echo ---- 2. component store >> %L%
Dism /Online /Cleanup-Image /AnalyzeComponentStore >> %L% 2>&1
Dism /Online /Cleanup-Image /StartComponentCleanup >> %L% 2>&1
echo startcomponentcleanup exit %errorlevel% >> %L%
call :free after-componentcleanup
powershell -NoProfile -Command "if ((Get-PSDrive C).Free -ge 4.5GB) { exit 0 } else { exit 1 }"
if errorlevel 1 (
  echo ---- 3. ResetBase: still under 4.5 GB free >> %L%
  Dism /Online /Cleanup-Image /StartComponentCleanup /ResetBase >> %L% 2>&1
  echo resetbase exit !errorlevel! >> %L%
  call :free after-resetbase
) else (
  echo ---- 3. ResetBase skipped: 4.5 GB or more free >> %L%
)
echo ---- 4. caches >> %L%
powershell -NoProfile -Command "try { Delete-DeliveryOptimizationCache -Force -ErrorAction Stop; 'DO cache cleared' } catch { 'DO cache: ' + $_ }" >> %L% 2>&1
del /f /s /q "%TEMP%\*" > nul 2>&1
del /f /s /q "C:\Windows\Temp\*" > nul 2>&1
powercfg /h off >> %L% 2>&1
call :free end
echo done %date% %time% > %D%\done
echo ==== c-clean-win end %date% %time% >> %L%
if exist %D%\restart (
  del %D%\restart
  shutdown /r /t 60 /c "Starbite: C: cleanup done, restarting"
)
exit /b 0

:free
powershell -NoProfile -Command "'%1: C: free {0:N2} GB' -f ((Get-PSDrive C).Free/1GB)" >> %L% 2>&1
exit /b 0
