@echo off
rem touch-online.cmd (Track K, docs/WIN11-GPU.md 6.78): runs ONCE at the next logon from HKLM RunOnce StarbiteTouch
rem (tools/win/touch.sh apply). Turns off "press and hold for right-click" for this user: with it on, Windows holds
rem back the mouse click a tap makes in programs that are not touch-aware (Geometry Dash) until it knows the touch is
rem not a hold. The old value is saved first; untouch-online.cmd puts it back. Takes effect at the next sign-in.
setlocal
set B=C:\ProgramData\Starbite\debloat
set L=%B%\touch.log
if not exist %B% mkdir %B%
echo touch-1 start %date% %time% user %USERNAME% >> %L%
if not exist %B%\wisp-touch-before.reg reg export "HKCU\Software\Microsoft\Wisp\Touch" %B%\wisp-touch-before.reg /y >> %L% 2>&1
reg add "HKCU\Software\Microsoft\Wisp\Touch" /v TouchMode_hold /t REG_DWORD /d 0 /f >> %L% 2>&1
reg query "HKCU\Software\Microsoft\Wisp\Touch" >> %L% 2>&1
echo touch-1 end %date% %time% >> %L%
exit /b 0
