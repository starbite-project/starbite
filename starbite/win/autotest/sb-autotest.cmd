@echo off
rem Starbite unattended Windows test: runs from the Startup folder at logon, ONLY when C:\starbite\auto\enabled exists.
rem The flag is one-shot (deleted first), so a normal boot never reboots by itself. A 20-minute restart is armed as a
rem dead man in case the test script hangs; the script replaces it with a 5 s clean restart when it is done.
if not exist C:\starbite\auto\enabled exit /b 0
del /f /q C:\starbite\auto\enabled
shutdown /r /t 1200 /c "Starbite autotest dead man (20 min)"
echo %date% %time% sb-autotest.cmd start>> C:\starbite\auto\run.log
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\starbite\auto\autotest.ps1
echo %date% %time% sb-autotest.cmd: powershell exit %errorlevel%>> C:\starbite\auto\run.log
shutdown /a
shutdown /r /t 5 /c "Starbite autotest done"
