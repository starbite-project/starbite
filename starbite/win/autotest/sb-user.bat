@echo off
rem sb-user.bat (Track K T63, docs/WIN11-GPU.md 6.70): a session like the user's own on the big cores, measured.
rem Called by sb-gputest.cmd INSTEAD of every other test when staged. Phases (UTC-free: uptime in the log):
rem   1. 180 s idle desktop, typeperf every 5 s (CPU total/per core, DPC/interrupt, queue, memory, paging, disk,
rem      every process' CPU and working set) into umdlog\perf-user.csv;
rem   2. Geometry Dash through Steam with our GL driver from the stage (0158: 1%/0.1% lows), the user's copy kept
rem      aside and put back; clicks (sb-click.ps1): the terms Notice OK, Play, the first level (Stereo Madness), so
rem      gameplay frames are measured too; 360 s, a screenshot every 30 s;
rem   3. GD closed the normal way (taskkill without /f = WM_CLOSE), seconds until it is gone (then /f at 120 s);
rem   4. 120 s idle after GD (does the system stay slow?);
rem   5. the restart, timed: uptime logged right before "shutdown /r /t 0"; the host has the time to the menu, the
rem      next boot's Diagnostics-Performance 200/203 has Windows' own shutdown time and the services that slowed it.
rem T64: the x86/x64 translation cache (sbxta State, the XtaCache service, the folders) logged at start and before the
rem restart; Steam's and GD's start times (steam.exe first seen, GeometryDash.exe up).
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set G=C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash
cd /d %D%
call :up "USR1 start"
call :xta start
start "" /b typeperf -cf %D%\sb-perf-counters.txt -si 5 -f CSV -o %U%\perf-user.csv -y > %U%\typeperf.txt 2>&1
ping -n 4 127.0.0.1 > nul
tasklist /v /fo csv > %U%\tasks-start.csv 2>&1
for /l %%T in (1,1,6) do (
  ping -n 31 127.0.0.1 > nul
  sbshow.exe snap 40-user-idle-%%T >> %L% 2>&1
)
call :up "USR1 idle done"
tasklist /v /fo csv > %U%\tasks-idle.csv 2>&1

rem ---- 2. Geometry Dash ----
set GDKIT=0
if exist "%G%\opengl32.dll.starbite" set GDKIT=1
if not exist "%G%\GeometryDash.exe" (echo USR1: Geometry Dash not installed >> %L% & goto after_gd)
if exist "%G%\opengl32.dll" if "%GDKIT%"=="0" (echo USR1: a foreign opengl32.dll: GD skipped >> %L% & goto after_gd)
if "%GDKIT%"=="1" (
  copy /y "%G%\opengl32.dll" "%G%\opengl32.dll.user" > nul
  copy /y "%G%\libgallium_wgl.dll" "%G%\libgallium_wgl.dll.user" > nul
)
copy /y %D%\opengl32-gd.dll "%G%\opengl32.dll" > nul
copy /y %D%\libgallium_wgl-gd.dll "%G%\libgallium_wgl.dll" > nul
echo starbite > "%G%\opengl32.dll.starbite"
call :up "USR1 GD start"
start "" steam://rungameid/322170
set /a W=0
set STEAMUP=
:wait
ping -n 6 127.0.0.1 > nul
set /a W+=5
if not defined STEAMUP tasklist /fi "imagename eq steam.exe" | find /i "steam.exe" > nul && (set STEAMUP=!W!& call :up "USR1 steam.exe up after !W! s")
tasklist /fi "imagename eq GeometryDash.exe" | find /i "GeometryDash.exe" > nul && goto gd_up
if !W! lss 360 goto wait
echo USR1 GeometryDash.exe never appeared in 360 s >> %L%
goto gd_restore
:gd_up
call :up "USR1 GeometryDash.exe up after !W! s"
rem typeperf expands Process(*) only when it starts: a second one for the processes alive with GD
start "" /b typeperf "\Process(*)\%% Processor Time" "\Process(*)\Working Set - Private" -si 5 -f CSV -o %U%\perf-user-gd.csv -y > nul 2>&1
tasklist /v /fo csv > %U%\tasks-gdstart.csv 2>&1
rem let it load to the menu, then: Notice OK, Play, the first level
ping -n 41 127.0.0.1 > nul
sbshow.exe snap 41-user-gd-menu >> %L% 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File %D%\sb-click.ps1 0.5 0.733 >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
powershell -NoProfile -ExecutionPolicy Bypass -File %D%\sb-click.ps1 0.5 0.5 >> %L% 2>&1
rem T63: Play took effect only after the 6 s snap and the card click landed in the page change: wait 15 s, then click
rem the Stereo Madness card's centre (50%/31%) twice, 10 s apart (in the level a click is a jump: harmless)
ping -n 16 127.0.0.1 > nul
sbshow.exe snap 42-user-gd-levels >> %L% 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File %D%\sb-click.ps1 0.5 0.31 >> %L% 2>&1
ping -n 11 127.0.0.1 > nul
powershell -NoProfile -ExecutionPolicy Bypass -File %D%\sb-click.ps1 0.5 0.31 >> %L% 2>&1
call :up "USR1 GD clicks sent"
for /l %%T in (1,1,10) do (
  ping -n 31 127.0.0.1 > nul
  set ALIVE=gone
  for /f "tokens=5" %%m in ('tasklist /fi "imagename eq GeometryDash.exe" /nh ^| find /i "GeometryDash"') do set ALIVE=%%m K
  echo USR1 GD t+%%T x30s: !ALIVE! >> %L%
  if "!ALIVE!"=="gone" goto gd_gone
  sbshow.exe snap 43-user-gd-%%T >> %L% 2>&1
)
tasklist /v /fo csv > %U%\tasks-gd.csv 2>&1
rem ---- 3. the normal close ----
call :up "USR1 GD close (WM_CLOSE)"
taskkill /im GeometryDash.exe >> %L% 2>&1
set /a C=0
:closing
ping -n 2 127.0.0.1 > nul
set /a C+=1
tasklist /fi "imagename eq GeometryDash.exe" | find /i "GeometryDash.exe" > nul || goto closed
if !C! lss 120 goto closing
call :up "USR1 GD still running 120 s after WM_CLOSE: /f"
sbshow.exe snap 44-user-gd-noclose >> %L% 2>&1
taskkill /f /im GeometryDash.exe >> %L% 2>&1
goto gd_restore
:closed
call :up "USR1 GD gone !C! s after WM_CLOSE"
goto gd_restore
:gd_gone
call :up "USR1 GD EXITED BY ITSELF"
:gd_restore
ping -n 4 127.0.0.1 > nul
if "%GDKIT%"=="0" del /f "%G%\opengl32.dll" "%G%\libgallium_wgl.dll" "%G%\opengl32.dll.starbite"
if "%GDKIT%"=="1" if exist "%G%\opengl32.dll.user" (
  move /y "%G%\opengl32.dll.user" "%G%\opengl32.dll" > nul
  move /y "%G%\libgallium_wgl.dll.user" "%G%\libgallium_wgl.dll" > nul
)
:after_gd
rem ---- 4. idle after ----
for /l %%T in (1,1,4) do (
  ping -n 31 127.0.0.1 > nul
  sbshow.exe snap 45-user-after-%%T >> %L% 2>&1
)
tasklist /v /fo csv > %U%\tasks-after.csv 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000 or EventID=1002)]]" /rd:true /c:40 /f:text > %U%\user-app-events.txt 2>&1
taskkill /im typeperf.exe > nul 2>&1
ping -n 3 127.0.0.1 > nul
call :xta end
rem ---- 5. the timed restart (sb-gputest.cmd must not add its own) ----
call :up "USR1 restart now"
echo on> %D%\sb-user.done
shutdown /r /t 0 /c "Starbite user-style session done"
exit /b 0

:xta
echo USR1 xta %1: >> %L%
reg query HKLM\SYSTEM\CurrentControlSet\Services\sbxta\State /v Result >> %L% 2>&1
reg query HKLM\SYSTEM\CurrentControlSet\Services\sbxta\State /v Swap >> %L% 2>&1
sc query XtaCache | find "STATE" >> %L% 2>&1
for /d %%x in (C:\Windows\XtaCache*) do (
  set XN=0
  for /f %%n in ('dir /a /s /b "%%x" 2^>nul ^| find /c /v ""') do set XN=%%n
  echo USR1 xta folder %%x: !XN! entries >> %L%
)
powershell -NoProfile -Command "Get-ChildItem C:\Windows\XtaCache -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum | %%{ 'USR1 xta XtaCache: ' + $_.Count + ' files, ' + [int]($_.Sum/1MB) + ' MiB (readable part)' }" >> %L% 2>&1
exit /b 0

:up
for /f %%s in ('powershell -NoProfile -Command "[int]([Diagnostics.Stopwatch]::GetTimestamp()/[Diagnostics.Stopwatch]::Frequency)"') do set UPS=%%s
echo %~1 (uptime !UPS! s, %time%) >> %L%
exit /b 0
