@echo off
rem sb-apps3.bat (Track K T87, docs/WIN11-GPU.md 6.112), staged as gputest\sb-user.bat. The re-read of T78
rem (build/win/patchwork/b78-report.md): the compositor's GPU-job timeouts did not start with Edge but seconds after
rem Calculator opened, the hung job is the same DWM draw every time, and the storm lasted as long as Calculator
rem stayed open (the old script never closed it). This script brackets the onset inside each step and repeats
rem T78's situation on purpose:
rem   - the three clocks tied together at the start and the end (wall time, tick count, the KMD's Live_Seconds);
rem   - every app: counters 5 s and 12 s after its start, a shot at 20 s, counters after it is closed;
rem   - whenever the timeout count grew: the KMD's hang records (Live_HangLog, Live_HangPtrs, Live_HangPtrsFirst,
rem     Live_FaultLog) into umdlog\hang-<step>.txt (the rings hold 8 and 3 records);
rem   - a step that KEEPS Calculator open (counters at 5, 12, 20 s, Notepad on top, two shots, a task list),
rem     then closes it (counters 5 and 15 s later: does the storm stop with the window?);
rem   - the desktop's pace (sbsmooth), then Edge as the user starts it.
rem Flag files that must exist before DWM starts (noidvs, nirshaders, listall) are set from TWRP, not here.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set K=HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\STRB0072\0\Device Parameters\sbwddm
set PAGE=file:///C:/starbite/auto/gpu/sb-web.html
set LASTT=0
cd /d %D%
echo APPS3 start %date% %time% >> %L%
for %%F in (noidvs nirshaders listall noafbc b78nowait gsemu msaa d3d11ddi) do if exist C:\ProgramData\Starbite\%%F echo APPS3 flag file %%F is present >> %L%
ping -n 31 127.0.0.1 > nul
call :clocks start
taskkill /f /im msedge.exe > nul 2>&1
ping -n 6 127.0.0.1 > nul
call :stat logon
call :app settings SystemSettings.exe "ms-settings:display"
call :app calc Calculator.exe "calc.exe"
call :app notepad Notepad.exe "notepad.exe"
call :app paint mspaint.exe "mspaint.exe"
call :app explorer explorer.exe "explorer.exe C:\Windows"
call :app taskmgr Taskmgr.exe "taskmgr.exe"

rem ---- Calculator kept open, as T78 had it by accident
echo APPS3 [calc-hold] start %time%: calc.exe, not closed >> %L%
start "" calc.exe
ping -n 6 127.0.0.1 > nul
call :stat calc-hold-5s
ping -n 8 127.0.0.1 > nul
call :stat calc-hold-12s
ping -n 9 127.0.0.1 > nul
call :stat calc-hold-20s
tasklist /v /fo csv > %U%\tasks-calc-hold.csv 2>&1
echo APPS3 [calc-hold] Notepad on top %time% >> %L%
start "" notepad.exe
ping -n 9 127.0.0.1 > nul
call :stat calc-hold-notepad
%D%\sbshow.exe snap app-calc-hold-1 >> %L% 2>&1
ping -n 11 127.0.0.1 > nul
%D%\sbshow.exe snap app-calc-hold-2 >> %L% 2>&1
call :stat calc-hold-shots
taskkill /im Notepad.exe > nul 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im Notepad.exe > nul 2>&1
echo APPS3 [calc-hold] closing Calculator %time% >> %L%
taskkill /im Calculator.exe > nul 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im Calculator.exe > nul 2>&1
taskkill /f /im CalculatorApp.exe > nul 2>&1
set NC=0
for /f %%c in ('tasklist /nh ^| find /c /i "Calculator"') do set NC=%%c
echo APPS3 [calc-hold] Calculator processes left: !NC! >> %L%
ping -n 4 127.0.0.1 > nul
call :stat calc-closed-5s
ping -n 11 127.0.0.1 > nul
call :stat calc-closed-15s

if exist %D%\sbsmooth.exe %D%\sbsmooth.exe after %D%\sbsmooth.log >> %L% 2>&1
call :stat smooth
call :app edge msedge.exe "msedge.exe --no-first-run --new-window %PAGE%"
reg query "%K%" > %U%\sbwddm-state.txt 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:30 /f:text > %U%\apps-app-events.txt 2>&1
tasklist /v /fo csv > %U%\tasks-apps-end.csv 2>&1
tasklist /svc /fo csv > %U%\tasks-apps-svc.csv 2>&1
call :clocks end
echo APPS3 end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite APPS3 done"
exit /b 0

:clocks
set TK=?
set LS=?
for /f %%t in ('powershell -NoProfile -Command "[Environment]::TickCount"') do set TK=%%t
for /f "tokens=3" %%v in ('reg query "%K%" /v Live_Seconds 2^>nul ^| find "REG_DWORD"') do set /a LS=%%v
echo APPS3 clocks %1: wall %date% %time%, tick count !TK! ms, KMD Live_Seconds !LS! >> %L%
exit /b 0

:stat
set T=?& set R=?& set F=?& set H=?
for /f "tokens=3" %%v in ('reg query "%K%" /v Live_JobsTimedOut 2^>nul ^| find "REG_DWORD"') do set /a T=%%v
for /f "tokens=3" %%v in ('reg query "%K%" /v Live_GpuResets 2^>nul ^| find "REG_DWORD"') do set /a R=%%v
for /f "tokens=3" %%v in ('reg query "%K%" /v Live_JobsFailed 2^>nul ^| find "REG_DWORD"') do set /a F=%%v
for /f "tokens=3" %%v in ('reg query "%K%" /v Live_TdrEngineResets 2^>nul ^| find "REG_DWORD"') do set /a H=%%v
set DW=none
for /f "tokens=2" %%p in ('tasklist /fi "imagename eq dwm.exe" /nh ^| find /i "dwm.exe"') do set DW=%%p
echo APPS3 stat %1 at %time%: job timeouts !T!, GPU resets !R!, failed jobs !F!, TDR engine resets !H!, dwm pid !DW! >> %L%
if "!T!"=="?" exit /b 0
if !T! gtr !LASTT! (
  echo APPS3 stat %1: timeouts grew from !LASTT! to !T!: hang records saved in umdlog\hang-%1.txt >> %L%
  echo step %1 at %time%: job timeouts !LASTT! to !T! > %U%\hang-%1.txt
  for %%V in (Live_HangLog Live_HangPtrs Live_HangPtrsFirst Live_FaultLog Live_HangCount) do reg query "%K%" /v %%V >> %U%\hang-%1.txt 2>&1
  set LASTT=!T!
)
exit /b 0

:app
rem %1 name, %2 image name, %3 command line
echo APPS3 [%1] start %time%: %~3 >> %L%
start "" %~3
ping -n 6 127.0.0.1 > nul
call :stat %1-5s
ping -n 8 127.0.0.1 > nul
call :stat %1-12s
ping -n 9 127.0.0.1 > nul
set N1=0
for /f %%c in ('tasklist /fi "imagename eq %2" /nh ^| find /c /i "%2"') do set N1=%%c
%D%\sbshow.exe snap app-%1 >> %L% 2>&1
echo APPS3 [%1] after 20 s: !N1! %2 running >> %L%
if /i not "%2"=="explorer.exe" (
  taskkill /im %2 > nul 2>&1
  ping -n 4 127.0.0.1 > nul
  taskkill /f /im %2 > nul 2>&1
) else (
  powershell -NoProfile -Command "(New-Object -ComObject Shell.Application).Windows() | ForEach-Object { $_.Quit() }" > nul 2>&1
)
ping -n 4 127.0.0.1 > nul
call :stat %1-closed
exit /b 0
