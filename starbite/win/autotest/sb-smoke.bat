@echo off
rem sb-smoke.bat (Track K, docs/WIN11-GPU.md 6.105), staged as gputest\sb-user.bat: the five-minute check a driver
rem build gets before any long test is queued on it. Does Windows reach the logon with this sbumd, does DWM stay
rem up, does a windowed D3D program get our adapter and show? Then the KMD's counters, the whole sbwddm State key
rem and the Application crash events are saved, and it restarts. If the boot never gets here, the watchdog ends it
rem (--full-sec 600) and gputest.log is missing: that is the answer too.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set K=HKLM\SYSTEM\CurrentControlSet\Services\sbwddm\State
cd /d %D%
echo SMOKE logon reached %date% %time% >> %L%
ping -n 21 127.0.0.1 > nul
call :dwmpid DWM0
echo SMOKE dwm pid !DWM0! >> %L%
if exist %D%\sbcomp.exe for %%M in (hwndblt hwnd) do (
  if exist %D%\sbcomp.log del /f %D%\sbcomp.log
  start "sbcomp-%%M" %D%\sbcomp.exe %%M 6 %D%\sbcomp.log
  ping -n 5 127.0.0.1 > nul
  %D%\sbshow.exe snap smoke-%%M >> %L% 2>&1
  ping -n 8 127.0.0.1 > nul
  taskkill /f /im sbcomp.exe > nul 2>&1
  if exist %D%\sbcomp.log type %D%\sbcomp.log >> %L%
)
if exist %D%\d3d10core_test.exe if exist %D%\sbwt.exe %D%\sbwt.exe %D%\d3d10core_test.exe d3d10core %U%\sbwt-d3d10core-smoke.log 60 0 12 >> %L% 2>&1
call :dwmpid DWM1
if "!DWM0!"=="!DWM1!" (echo SMOKE DWM SURVIVED, pid !DWM1! >> %L%) else (echo SMOKE DWM RESTARTED, pid !DWM0! then !DWM1! >> %L%)
for %%V in (Live_JobsTimedOut Live_GpuResets Live_FaultCount Live_HangCount) do for /f "tokens=3" %%v in ('reg query %K% /v %%V 2^>nul ^| find "REG_DWORD"') do (set /a X=%%v & echo SMOKE %%V !X! >> %L%)
reg query %K% > %U%\sbwddm-state.txt 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:16 /f:text > %U%\smoke-app-events.txt 2>&1
echo SMOKE end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite SMOKE done"
exit /b 0

:dwmpid
set %1=none
for /f "tokens=2" %%p in ('tasklist /fi "imagename eq dwm.exe" /nh ^| find /i "dwm.exe"') do set %1=%%p
exit /b 0
