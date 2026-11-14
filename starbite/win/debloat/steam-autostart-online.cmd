@echo off
rem steam-autostart-online.cmd (Track K, docs/WIN11-GPU.md 6.82): runs ONCE at the next logon from HKLM RunOnce
rem StarbiteSteamOn (tools/win/touch.sh steam OUT). Puts Steam back into the user's autostart (as Steam writes it):
rem debloat-1 took it out, and on the big cores a cold Steam start then made the game take 5+ minutes to come up.
setlocal
set B=C:\ProgramData\Starbite\debloat
set L=%B%\debloat.log
if not exist %B% mkdir %B%
echo steam-autostart start %date% %time% user %USERNAME% >> %L%
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Steam /t REG_SZ /d "\"C:\Program Files (x86)\Steam\steam.exe\" -silent" /f >> %L% 2>&1
reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Steam >> %L% 2>&1
echo steam-autostart end %date% %time% >> %L%
exit /b 0
