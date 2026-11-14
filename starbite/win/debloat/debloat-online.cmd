@echo off
rem debloat-online.cmd (Track K, docs/WIN11-GPU.md 6.77): runs ONCE at the next logon from HKLM RunOnce
rem (StarbiteDebloat, written offline by tools/win/debloat.sh apply). What it does, all logged to
rem C:\ProgramData\Starbite\debloat\debloat.log, all undone by undebloat-online.cmd (tools/win/debloat.sh undo):
rem   1. backs up the per-user autostart lists (HKCU ...\Run and ...\RunOnce) to .reg files (kept from the first run);
rem   2. removes the autostart values named Steam and OneDrive* from them (Steam still works when you start it;
rem      the OneDrive installer no longer runs at every logon);
rem   3. elevated only: a Windows Defender exclusion for C:\Program Files (x86)\Steam (games there are not scanned
rem      on every read; the rest of the PC is scanned as before).
setlocal
set B=C:\ProgramData\Starbite\debloat
set L=%B%\debloat.log
if not exist %B% mkdir %B%
echo debloat-1 start %date% %time% user %USERNAME% >> %L%
set ELEV=0
net session > nul 2>&1 && set ELEV=1
echo elevated: %ELEV% >> %L%
if not exist %B%\run-before.reg reg export "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" %B%\run-before.reg /y >> %L% 2>&1
if not exist %B%\runonce-before.reg reg export "HKCU\Software\Microsoft\Windows\CurrentVersion\RunOnce" %B%\runonce-before.reg /y >> %L% 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command "foreach ($k in 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run','HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce') { $p = Get-ItemProperty $k -ErrorAction SilentlyContinue; if ($p) { foreach ($n in $p.PSObject.Properties.Name) { if ($n -match '^(Steam|OneDrive.*)$') { Remove-ItemProperty $k -Name $n; 'removed ' + $k + ' ' + $n } } } }" >> %L% 2>&1
if "%ELEV%"=="1" (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-MpPreference -ExclusionPath 'C:\Program Files (x86)\Steam'; 'Defender exclusions: ' + ((Get-MpPreference).ExclusionPath -join '; ')" >> %L% 2>&1
) else (
  echo Defender exclusion skipped: this logon is not elevated >> %L%
)
reg query HKLM\SYSTEM\CurrentControlSet\Services\sbxta\State /v Svc >> %L% 2>&1
echo debloat-1 end %date% %time% >> %L%
exit /b 0
