@echo off
rem sb-flip.bat (Track K T70, docs/WIN11-GPU.md 6.95), staged as gputest\sb-elev.bat, run elevated by sb-elev-run.cmd.
rem The flip-model DWM device loss (T66, 6.91: DWM opens the app's two flip buffers, its D3D device is removed, it
rem recreates the device ~9 times and fails fast with 0x8898008D). sbcomp hwnd (FLIP_SEQUENTIAL, 1200x700 topmost red
rem window, 12 s as in T66) under four sets of sbumd 0163 switches (flag files in C:\ProgramData\Starbite; sbumd reads
rem them at device creation and again whenever a device opens another process's allocation, so DWM needs no restart):
rem   base     devstate only: which Render / open / present changes DWM's device execution state, and to what
rem   ro2      + zcro2: allocations opened from another process are listed without WriteOperation
rem   nolist2  + zcnolist2: they are not forced into every submission (0138 off for them)
rem   both     + both
rem then hwndblt (DISCARD, magenta) as the control that composed in T66. A screen shot in each; DWM's pid before/after.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set P=C:\ProgramData\Starbite
cd /d %D%
echo FLIP1 start %time% >> %L%
for %%C in (base ro2 nolist2 both) do call :one %%C hwnd
call :one base hwndblt
del /f %P%\devstate %P%\zcro2 %P%\zcnolist2 > nul 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:16 /f:text > %P%\umdlog\flip-app-events.txt 2>&1
dir /a C:\sbdumps >> %L% 2>&1
echo FLIP1 end %time% >> %L%
endlocal
exit /b 0

:one
del /f %P%\devstate %P%\zcro2 %P%\zcnolist2 > nul 2>&1
echo on> %P%\devstate
if "%1"=="ro2" echo on> %P%\zcro2
if "%1"=="nolist2" echo on> %P%\zcnolist2
if "%1"=="both" (echo on> %P%\zcro2 & echo on> %P%\zcnolist2)
call :dwmpid DWM0
echo FLIP1 [%1 %2] start %time%, dwm pid !DWM0! >> %L%
if exist %D%\sbcomp.log del /f %D%\sbcomp.log
start "sbcomp-%1" %D%\sbcomp.exe %2 12 %D%\sbcomp.log
ping -n 7 127.0.0.1 > nul
%D%\sbshow.exe snap flip-%1-%2 >> %L% 2>&1
call :dwmpid DWMM
set GONE=0
for /l %%T in (1,1,15) do if !GONE!==0 (
  tasklist /fi "imagename eq sbcomp.exe" 2>nul | find /i "sbcomp.exe" > nul
  if errorlevel 1 (set GONE=1) else (ping -n 3 127.0.0.1 > nul)
)
if !GONE!==0 (
  echo FLIP1 [%1 %2] sbcomp still running after ~30 s: taskkill >> %L%
  taskkill /f /im sbcomp.exe >> %L% 2>&1
)
if exist %D%\sbcomp.log type %D%\sbcomp.log >> %L%
rem a crashed DWM: let its dump and its successor's start finish before the next run
ping -n 9 127.0.0.1 > nul
call :dwmpid DWM1
if "!DWM0!"=="!DWM1!" (echo FLIP1 [%1 %2] end %time%: DWM SURVIVED, pid !DWM1! >> %L%) else (echo FLIP1 [%1 %2] end %time%: DWM RESTARTED, pid !DWM0! then !DWMM! at the shot then !DWM1! >> %L%)
exit /b 0

:dwmpid
set %1=none
for /f "tokens=2" %%p in ('tasklist /fi "imagename eq dwm.exe" /nh ^| find /i "dwm.exe"') do set %1=%%p
exit /b 0
