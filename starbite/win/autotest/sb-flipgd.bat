@echo off
rem sb-flipgd.bat (Track K T71, docs/WIN11-GPU.md 6.97), staged as gputest\sb-user.bat: sb-gputest.cmd hands over
rem to this at logon INSTEAD of every other test.
rem 1. FLIP2: with sbumd 0167 (no flag files) the windowed swap chain kinds once more, 10 s each, a shot each, DWM's
rem    pid before and after: hwnd (FLIP_SEQUENTIAL), hwndblt (DISCARD), dcsc (DirectComposition swap chain), dcsurf
rem    (DirectComposition surface). T70: flip-model windows compose when DWM lists the app's buffers as read.
rem 2. Geometry Dash through Steam (sb-gd.bat), four runs: A = the user's async GDI present, B = zero-copy on a
rem    FLIP_SEQUENTIAL swap chain at sync interval 0 (flag glzcflip, GD ICD 0164), B2 = FLIP_DISCARD (glzcflipd),
rem    A2 = GDI again. fps: the game driver's own log lines (glstats / "sb gl zc: 300 frames").
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set P=C:\ProgramData\Starbite
cd /d %D%
del /f %P%\devstate %P%\zcro2 %P%\zcnolist2 %P%\glzc %P%\glzcnovsync %P%\glzcflip %P%\glzcflipd %P%\glzcflipvsync > nul 2>&1
echo FLIP2 start %date% %time% >> %L%
ping -n 16 127.0.0.1 > nul
if exist %D%\sbcomp.exe for %%M in (hwnd hwndblt dcsc dcsurf) do call :comp %%M
echo FLIP2 end %time% >> %L%
if not exist %D%\sb-gd.bat goto done
call %D%\sb-gd.bat A
echo on> %P%\glzcflip
call %D%\sb-gd.bat B
del /f %P%\glzcflip > nul 2>&1
echo on> %P%\glzcflipd
call %D%\sb-gd.bat B2
del /f %P%\glzcflipd > nul 2>&1
call %D%\sb-gd.bat A2
:done
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:24 /f:text > %P%\umdlog\flipgd-app-events.txt 2>&1
echo FLIPGD end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite FLIPGD done"
exit /b 0

:comp
call :dwmpid DWM0
echo FLIP2 [%1] start %time%, dwm pid !DWM0! >> %L%
if exist %D%\sbcomp.log del /f %D%\sbcomp.log
start "sbcomp-%1" %D%\sbcomp.exe %1 10 %D%\sbcomp.log
ping -n 6 127.0.0.1 > nul
%D%\sbshow.exe snap flip2-%1 >> %L% 2>&1
set GONE=0
for /l %%T in (1,1,12) do if !GONE!==0 (
  tasklist /fi "imagename eq sbcomp.exe" 2>nul | find /i "sbcomp.exe" > nul
  if errorlevel 1 (set GONE=1) else (ping -n 3 127.0.0.1 > nul)
)
if !GONE!==0 taskkill /f /im sbcomp.exe >> %L% 2>&1
if exist %D%\sbcomp.log type %D%\sbcomp.log >> %L%
ping -n 6 127.0.0.1 > nul
call :dwmpid DWM1
if "!DWM0!"=="!DWM1!" (echo FLIP2 [%1] end %time%: DWM SURVIVED, pid !DWM1! >> %L%) else (echo FLIP2 [%1] end %time%: DWM RESTARTED, pid !DWM0! then !DWM1! >> %L%)
exit /b 0

:dwmpid
set %1=none
for /f "tokens=2" %%p in ('tasklist /fi "imagename eq dwm.exe" /nh ^| find /i "dwm.exe"') do set %1=%%p
exit /b 0
