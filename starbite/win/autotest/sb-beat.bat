@echo off
rem sb-beat.bat: once a minute, the time, the programs Windows reports as not responding, and the test and
rem compositor processes, into umdlog\beat.txt. Started minimised by a test script; ends with the restart.
set B=C:\ProgramData\Starbite\umdlog\beat.txt
:again
echo BEAT %date% %time% >> %B%
tasklist /fi "status eq not responding" /fo csv /nh >> %B% 2>&1
tasklist /fo csv /nh | findstr /i "sbwt _test.exe dwm.exe conhost OpenConsole WindowsTerminal SystemSettings cmd.exe" >> %B% 2>&1
ping -n 61 127.0.0.1 > nul
goto again
