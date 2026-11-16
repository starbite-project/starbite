@echo off
rem sb-apps2.bat (Track K T80, docs/WIN11-GPU.md 6.104), staged as gputest\sb-user.bat. T78: on sbumd 0167 the
rem compositor was blocked on the GPU from the moment Edge started (404 GPU job timeouts in that boot, every one a
rem dwm.exe vertex job that ran to the 752 ms limit). Which app sets it off, and does the user's driver do the same?
rem After every step the KMD's counters are read from the registry (Live_JobsTimedOut, Live_GpuResets,
rem Live_JobsFailed, Live_TdrEngineResets; test cycles flush them every second):
rem   0  the Edge processes Windows started at logon are closed; counters
rem   1  Settings, Calculator, Notepad, Paint, a File Explorer window, Task Manager: 20 s each
rem   2  sbsmooth "before" (the desktop's pace)
rem   3  Edge with --disable-gpu (no GPU process: the control)
rem   4  Edge with --disable-gpu-sandbox (its GPU process can write sbumd's log: umdlog\msedge.exe-*.log)
rem   5  Edge as the user starts it
rem   6  sbsmooth "after"; the whole sbwddm State key into umdlog\sbwddm-state.txt; crash events; tasklist /svc
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set K=HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\STRB0072\0\Device Parameters\sbwddm
set PAGE=file:///C:/starbite/auto/gpu/sb-web.html
cd /d %D%
echo APPS2 start %date% %time% >> %L%
ping -n 31 127.0.0.1 > nul
taskkill /f /im msedge.exe > nul 2>&1
ping -n 6 127.0.0.1 > nul
call :stat logon
call :app settings SystemSettings.exe "ms-settings:display"
call :app calc Calculator.exe "calc.exe"
call :app notepad Notepad.exe "notepad.exe"
call :app paint mspaint.exe "mspaint.exe"
call :app explorer explorer.exe "explorer.exe C:\Windows"
call :app taskmgr Taskmgr.exe "taskmgr.exe"
if exist %D%\sbsmooth.exe %D%\sbsmooth.exe before %D%\sbsmooth.log >> %L% 2>&1
call :stat smooth-before
call :app edge-nogpu msedge.exe "msedge.exe --no-first-run --disable-gpu --new-window %PAGE%"
call :app edge-nosandbox msedge.exe "msedge.exe --no-first-run --disable-gpu-sandbox --new-window %PAGE%"
call :app edge msedge.exe "msedge.exe --no-first-run --new-window %PAGE%"
if exist %D%\sbsmooth.exe %D%\sbsmooth.exe after %D%\sbsmooth.log >> %L% 2>&1
call :stat smooth-after
reg query "%K%" > %U%\sbwddm-state.txt 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:30 /f:text > %U%\apps-app-events.txt 2>&1
tasklist /v /fo csv > %U%\tasks-apps-end.csv 2>&1
tasklist /svc /fo csv > %U%\tasks-apps-svc.csv 2>&1
echo APPS2 end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite APPS2 done"
exit /b 0

:stat
set T=?& set R=?& set F=?& set H=?
for /f "tokens=3" %%v in ('reg query "%K%" /v Live_JobsTimedOut 2^>nul ^| find "REG_DWORD"') do set /a T=%%v
for /f "tokens=3" %%v in ('reg query "%K%" /v Live_GpuResets 2^>nul ^| find "REG_DWORD"') do set /a R=%%v
for /f "tokens=3" %%v in ('reg query "%K%" /v Live_JobsFailed 2^>nul ^| find "REG_DWORD"') do set /a F=%%v
for /f "tokens=3" %%v in ('reg query "%K%" /v Live_TdrEngineResets 2^>nul ^| find "REG_DWORD"') do set /a H=%%v
set DW=none
for /f "tokens=2" %%p in ('tasklist /fi "imagename eq dwm.exe" /nh ^| find /i "dwm.exe"') do set DW=%%p
echo APPS2 stat after %1 at %time%: job timeouts !T!, GPU resets !R!, failed jobs !F!, TDR engine resets !H!, dwm pid !DW! >> %L%
exit /b 0

:app
rem %1 name, %2 image name, %3 command line
echo APPS2 [%1] start %time%: %~3 >> %L%
start "" %~3
ping -n 21 127.0.0.1 > nul
set N1=0
for /f %%c in ('tasklist /fi "imagename eq %2" /nh ^| find /c /i "%2"') do set N1=%%c
%D%\sbshow.exe snap app-%1 >> %L% 2>&1
powershell -NoProfile -Command "Get-Process | Where-Object { $_.MainWindowTitle -and $_.ProcessName -match '%~n2' } | ForEach-Object { 'APPS2 [%1] window: ' + $_.ProcessName + ' pid ' + $_.Id + ' title ' + $_.MainWindowTitle }" >> %L% 2>&1
echo APPS2 [%1] after 20 s: !N1! %2 running >> %L%
if /i not "%2"=="explorer.exe" (
  taskkill /im %2 > nul 2>&1
  ping -n 4 127.0.0.1 > nul
  taskkill /f /im %2 > nul 2>&1
) else (
  powershell -NoProfile -Command "(New-Object -ComObject Shell.Application).Windows() | ForEach-Object { $_.Quit() }" > nul 2>&1
)
ping -n 4 127.0.0.1 > nul
call :stat %1
exit /b 0
