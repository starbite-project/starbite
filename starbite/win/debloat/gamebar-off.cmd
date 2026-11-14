@echo off
rem gamebar-off.cmd (Track K, docs/WIN11-GPU.md 6.92): runs ONCE at the next logon from HKLM RunOnce StarbiteGameBarOff
rem (tools/win/runonce-arm.sh). Why: twice the shell host sihost.exe aborted ~3 min after Geometry Dash started (the
rem user's session 11-14 07:47, T67a 20:43), each time right after DCOM could not start
rem Windows.Gaming.GameBar.PresenceServer.Internal.PresenceWriter; the crash dump's stack is in twinui.pcshell.dll
rem with REGDB_E_CLASSNOTREG. That is Game Bar's game detection; this turns it off for this user (Game Bar, game
rem capture, Game Mode's automatic switch). Old values exported first; gamebar-on.cmd puts them back.
setlocal
set B=C:\ProgramData\Starbite\debloat
set L=%B%\gamebar.log
if not exist %B% mkdir %B%
echo gamebar-off start %date% %time% user %USERNAME% >> %L%
if not exist %B%\gameconfigstore-before.reg reg export "HKCU\System\GameConfigStore" %B%\gameconfigstore-before.reg /y >> %L% 2>&1
if not exist %B%\gamebar-before.reg reg export "HKCU\Software\Microsoft\GameBar" %B%\gamebar-before.reg /y >> %L% 2>&1
if not exist %B%\gamedvr-before.reg reg export "HKCU\Software\Microsoft\Windows\CurrentVersion\GameDVR" %B%\gamedvr-before.reg /y >> %L% 2>&1
reg add "HKCU\System\GameConfigStore" /v GameDVR_Enabled /t REG_DWORD /d 0 /f >> %L% 2>&1
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\GameDVR" /v AppCaptureEnabled /t REG_DWORD /d 0 /f >> %L% 2>&1
reg add "HKCU\Software\Microsoft\GameBar" /v UseNexusForGameBarEnabled /t REG_DWORD /d 0 /f >> %L% 2>&1
reg add "HKCU\Software\Microsoft\GameBar" /v AutoGameModeEnabled /t REG_DWORD /d 0 /f >> %L% 2>&1
reg add "HKCU\Software\Microsoft\GameBar" /v ShowStartupPanel /t REG_DWORD /d 0 /f >> %L% 2>&1
echo gamebar-off end %date% %time% >> %L%
exit /b 0
