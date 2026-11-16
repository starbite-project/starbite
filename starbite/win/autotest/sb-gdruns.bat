@echo off
rem sb-gdruns.bat (Track K T79, docs/WIN11-GPU.md 6.103), staged as gputest\sb-user.bat: Geometry Dash runs
rem (sb-gd.bat: Steam, into Stereo Madness, ~5 min each) with a set of GL-driver flag files per run. sb-gd-runs.txt
rem has one run per line:    LABEL [flag flag ...]     e.g. "C glzcflipd glzcasync". All flags named in the file are
rem removed at the start and after each run. fps: the game driver's own log lines.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set P=C:\ProgramData\Starbite
cd /d %D%
echo GDRUNS start %date% %time% >> %L%
if not exist %D%\sb-gd-runs.txt (echo GDRUNS: no sb-gd-runs.txt >> %L% & goto done)
for /f "usebackq tokens=1,*" %%a in ("%D%\sb-gd-runs.txt") do for %%F in (%%b) do del /f %P%\%%F > nul 2>&1
ping -n 16 127.0.0.1 > nul
for /f "usebackq tokens=1,*" %%a in ("%D%\sb-gd-runs.txt") do call :run %%a "%%b"
:done
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:24 /f:text > %P%\umdlog\gdruns-app-events.txt 2>&1
echo GDRUNS end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite GDRUNS done"
exit /b 0

:run
echo GDRUNS run %1: flags %~2 >> %L%
for %%F in (%~2) do echo on> %P%\%%F
call %D%\sb-gd.bat %1
for %%F in (%~2) do del /f %P%\%%F > nul 2>&1
exit /b 0
