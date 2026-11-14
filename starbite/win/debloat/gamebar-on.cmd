@echo off
rem gamebar-on.cmd (Track K, docs/WIN11-GPU.md 6.92): undo of gamebar-off.cmd, once at the next logon (HKLM RunOnce
rem StarbiteGameBarOn): the values it set are removed, then the exported originals imported.
setlocal
set B=C:\ProgramData\Starbite\debloat
set L=%B%\gamebar.log
echo gamebar-on start %date% %time% user %USERNAME% >> %L%
reg delete "HKCU\System\GameConfigStore" /v GameDVR_Enabled /f >> %L% 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\GameDVR" /v AppCaptureEnabled /f >> %L% 2>&1
for %%v in (UseNexusForGameBarEnabled AutoGameModeEnabled ShowStartupPanel) do reg delete "HKCU\Software\Microsoft\GameBar" /v %%v /f >> %L% 2>&1
for %%f in (gameconfigstore gamebar gamedvr) do if exist %B%\%%f-before.reg reg import %B%\%%f-before.reg >> %L% 2>&1
echo gamebar-on end %date% %time% >> %L%
exit /b 0
