@echo off
rem sb-apps.bat (Track K T78, docs/WIN11-GPU.md 6.102), staged as gputest\sb-user.bat: do everyday apps work on this
rem driver? Each app is started, given 25 s, shot (sbshow snap app-<name>), checked alive, its window title logged,
rem then closed. Before and after: DWM's pid. At the end: sbsmooth and sbdrag once (the desktop's pace on this
rem driver, against T72), the Application crash events with their times, the process list.
rem   edge      msedge with sb-web.html (CSS animation, 2D canvas, WebGL; the page title carries its fps and the
rem             WebGL renderer string)
rem   settings  ms-settings:display (WinUI / XAML)       calc     Calculator (UWP)
rem   notepad   Notepad (Windows 11's)                    paint    Paint
rem   explorer  a File Explorer window                    taskmgr  Task Manager
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
cd /d %D%
echo APPS1 start %date% %time% >> %L%
ping -n 31 127.0.0.1 > nul
call :dwmpid DWM0
echo APPS1 dwm pid !DWM0! >> %L%
call :app edge msedge.exe "msedge.exe --no-first-run --new-window file:///C:/starbite/auto/gpu/sb-web.html"
call :app settings SystemSettings.exe "ms-settings:display"
call :app calc CalculatorApp.exe "calc.exe"
call :app notepad Notepad.exe "notepad.exe"
call :app paint mspaint.exe "mspaint.exe"
call :app explorer explorer.exe "explorer.exe C:\Windows"
call :app taskmgr Taskmgr.exe "taskmgr.exe"
call :dwmpid DWM1
if "!DWM0!"=="!DWM1!" (echo APPS1 DWM SURVIVED, pid !DWM1! >> %L%) else (echo APPS1 DWM RESTARTED, pid !DWM0! then !DWM1! >> %L%)
if exist %D%\sbsmooth.exe %D%\sbsmooth.exe apps %D%\sbsmooth.log >> %L% 2>&1
if exist %D%\sbdrag.exe %D%\sbdrag.exe apps %D%\sbdrag.log >> %L% 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:30 /f:text > %U%\apps-app-events.txt 2>&1
tasklist /v /fo csv > %U%\tasks-apps-end.csv 2>&1
tasklist /svc /fo csv > %U%\tasks-apps-svc.csv 2>&1
echo APPS1 end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite APPS1 done"
exit /b 0

:app
rem %1 name, %2 image name, %3 command line
set N0=0
for /f %%c in ('tasklist /fi "imagename eq %2" /nh ^| find /c /i "%2"') do set N0=%%c
echo APPS1 [%1] start %time%: %~3 ^(!N0! %2 before^) >> %L%
start "" %~3
ping -n 26 127.0.0.1 > nul
set N1=0
for /f %%c in ('tasklist /fi "imagename eq %2" /nh ^| find /c /i "%2"') do set N1=%%c
%D%\sbshow.exe snap app-%1 >> %L% 2>&1
powershell -NoProfile -Command "Get-Process | Where-Object { $_.MainWindowTitle -and $_.ProcessName -match '%~n2' } | ForEach-Object { 'APPS1 [%1] window: ' + $_.ProcessName + ' pid ' + $_.Id + ' title ' + $_.MainWindowTitle }" >> %L% 2>&1
if /i "%2"=="explorer.exe" (echo APPS1 [%1] explorer windows are judged by the window line and the shot >> %L%) else if !N1! gtr !N0! (echo APPS1 [%1] ALIVE after 25 s: !N1! %2 >> %L%) else (echo APPS1 [%1] NOT RUNNING after 25 s: !N1! %2, !N0! before >> %L%)
if /i not "%2"=="explorer.exe" (
  taskkill /im %2 > nul 2>&1
  ping -n 4 127.0.0.1 > nul
  taskkill /f /im %2 > nul 2>&1
) else (
  powershell -NoProfile -Command "(New-Object -ComObject Shell.Application).Windows() | ForEach-Object { $_.Quit() }" > nul 2>&1
)
ping -n 4 127.0.0.1 > nul
exit /b 0

:dwmpid
set %1=none
for /f "tokens=2" %%p in ('tasklist /fi "imagename eq dwm.exe" /nh ^| find /i "dwm.exe"') do set %1=%%p
exit /b 0
