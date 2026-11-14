@echo off
rem untouch-online.cmd (Track K, docs/WIN11-GPU.md 6.78): runs ONCE at the next logon from HKLM RunOnce StarbiteUntouch
rem (tools/win/touch.sh restore): "press and hold for right-click" back as it was (the saved .reg, or the value
rem removed = Windows' default, on).
setlocal
set B=C:\ProgramData\Starbite\debloat
set L=%B%\touch.log
if not exist %B% mkdir %B%
echo untouch-1 start %date% %time% user %USERNAME% >> %L%
reg delete "HKCU\Software\Microsoft\Wisp\Touch" /v TouchMode_hold /f >> %L% 2>&1
if exist %B%\wisp-touch-before.reg reg import %B%\wisp-touch-before.reg >> %L% 2>&1
reg query "HKCU\Software\Microsoft\Wisp\Touch" >> %L% 2>&1
echo untouch-1 end %date% %time% >> %L%
exit /b 0
