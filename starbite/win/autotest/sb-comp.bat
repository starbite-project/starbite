@echo off
rem VIS2 (Track K T56, docs/WIN11-GPU.md 6.61): which ways of putting D3D11 frames in a window reach the screen?
rem T54 VIS1: sbfps's topmost full-screen window (HWND swap chain, flip or blt model) never appeared in the shots;
rem DWM never opened an app swap chain buffer, while Explorer's DirectComposition surfaces reach DWM.
rem sbcomp.exe (a 1200x700 topmost window at 60,60) runs 12 s per mode, a screen snap at ~6 s, while ETW records the
rem DXGI, DxgKrnl (Base + Present) and Dwm-Core events; tracerpt turns each trace into CSV in the umdlog folder.
rem   gdi (yellow, control)  hwnd (red, FLIP_SEQUENTIAL)  hwndblt (magenta, DISCARD)  dcsc (green, DComp swap chain)
rem   dcsurf (blue, DComp surface)
setlocal
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
cd /d %D%
echo VIS2 start %time% >> %L%
> %D%\sbpres.pf echo "Microsoft-Windows-DXGI" 0xFFFFFFFFFFFFFFFF 5
>> %D%\sbpres.pf echo "Microsoft-Windows-DxgKrnl" 0x8000001 5
>> %D%\sbpres.pf echo "Microsoft-Windows-Dwm-Core" 0xFFFFFFFFFFFFFFFF 4
for %%M in (gdi hwnd hwndblt dcsc dcsurf) do call :one %%M
if exist %D%\sbcomp.log type %D%\sbcomp.log >> %L%
echo VIS2 end %time% >> %L%
endlocal
exit /b 0

:one
echo VIS2 %1 %time% >> %L%
logman start sbpres -pf %D%\sbpres.pf -o %D%\pres-%1.etl -ets -max 64 >> %L% 2>&1
start "sbcomp-%1" %D%\sbcomp.exe %1 12 %D%\sbcomp.log
ping -n 7 127.0.0.1 > nul
%D%\sbshow.exe snap comp-%1 >> %L% 2>&1
call :waitexit sbcomp.exe
logman stop sbpres -ets >> %L% 2>&1
if exist %D%\pres-%1.etl tracerpt %D%\pres-%1.etl -o %U%\pres-%1.csv -of CSV -y > nul 2>&1
if exist %U%\pres-%1.csv (for %%S in (%U%\pres-%1.csv) do echo VIS2 %1 trace csv %%~zS bytes >> %L%) else (echo VIS2 %1 no trace csv >> %L%)
if exist %D%\pres-%1.etl del /f %D%\pres-%1.etl
exit /b 0

:waitexit
for /l %%T in (1,1,15) do (
  tasklist /fi "imagename eq %1" 2>nul | find /i "%1" > nul
  if errorlevel 1 (echo VIS2 %1 ended >> %L% & exit /b 0)
  ping -n 3 127.0.0.1 > nul
)
echo VIS2 %1 still running after ~30 s: taskkill >> %L%
taskkill /f /im %1 >> %L% 2>&1
exit /b 0
