@echo off
rem undebloat-online.cmd (Track K, docs/WIN11-GPU.md 6.77): runs ONCE at the next logon from HKLM RunOnce
rem (StarbiteUndebloat, written offline by tools/win/debloat.sh undo). Puts back what debloat-online.cmd changed:
rem the per-user autostart lists from its .reg backups, and (elevated) removes the Defender exclusion for Steam.
setlocal
set B=C:\ProgramData\Starbite\debloat
set L=%B%\debloat.log
if not exist %B% mkdir %B%
echo undebloat-1 start %date% %time% user %USERNAME% >> %L%
set ELEV=0
net session > nul 2>&1 && set ELEV=1
echo elevated: %ELEV% >> %L%
if exist %B%\run-before.reg (reg import %B%\run-before.reg >> %L% 2>&1 & echo Run restored >> %L%)
if exist %B%\runonce-before.reg (reg import %B%\runonce-before.reg >> %L% 2>&1 & echo RunOnce restored >> %L%)
if "%ELEV%"=="1" (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-MpPreference -ExclusionPath 'C:\Program Files (x86)\Steam'; 'Defender exclusions: ' + ((Get-MpPreference).ExclusionPath -join '; ')" >> %L% 2>&1
) else (
  echo Defender exclusion NOT removed: this logon is not elevated >> %L%
)
reg query HKLM\SYSTEM\CurrentControlSet\Services\sbxta\State /v Svc >> %L% 2>&1
echo undebloat-1 end %date% %time% >> %L%
exit /b 0
