@echo off
rem sb-soak.bat (Track K, 2026-11-12): long-session stability. Called by sb-gputest.cmd INSTEAD of the sweep when it
rem is staged (gpu-cycle.sh --full-sec 3600, its maximum). 20 rounds of a mixed workload (~2.5 min each); after every round the
rem KMD's live counters, memory/commit/disk, and DWM's PID (a change = DWM restarted) go to gputest.log, so a leak, a
rem fault or hang trend, or a DWM restart shows as a row. No screenshots (disk). Ends with the event logs.
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set K=HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\STRB0072\0\Device Parameters\sbwddm
cd /d %D%
echo soak start %date% %time% >> %L%
sbshow.exe mincon >> %L% 2>&1
for /l %%R in (1,1,20) do call :round %%R
wevtutil qe Application /q:"*[System[(EventID=1000 or EventID=1001 or EventID=1002)]]" /rd:true /c:80 /f:text > C:\ProgramData\Starbite\umdlog\soak-app-events.txt 2>&1
wevtutil qe System /q:"*[System[(EventID=4101 or EventID=41 or EventID=6008 or EventID=1001 or EventID=141 or EventID=117)]]" /rd:true /c:40 /f:text > C:\ProgramData\Starbite\umdlog\soak-sys-events.txt 2>&1
echo soak end %date% %time% >> %L%
exit /b 0

:round
echo ==== soak round %1 %time% >> %L%
rem D3D load (native) and an x64 D3D11 program
sbfps.exe hw 45 sbfps-soak.log >> %L% 2>&1
echo soak %1 sbfps exit %errorlevel% >> %L%
if exist sbd3d64.exe sbd3d64.exe %D%\sbd3d-soak.log >> %L% 2>&1
if exist sbd3d64.exe echo soak %1 sbd3d x64 exit %errorlevel% >> %L%
rem OpenGL on the Mali, as an x64 program (if the GL test is staged)
if exist %D%\glx64\sbgl64.exe pushd %D%\glx64
if exist %D%\glx64\sbgl64.exe sbgl64.exe hw %D%\glx64\sbgl.log >> %L% 2>&1
if exist %D%\glx64\sbgl64.exe echo soak %1 sbgl x64 exit %errorlevel% >> %L%
if exist %D%\glx64\sbgl64.exe popd
rem the shell surfaces that hung the GPU in T41 (Quick Settings, Notification Centre), Start, task view
sbshow.exe keys win+a >> %L% 2>&1
ping -n 5 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
sbshow.exe keys win+n >> %L% 2>&1
ping -n 5 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
sbshow.exe keys win >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
sbshow.exe keys win+tab >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
rem File Explorer and Notepad, opened and closed (allocations come and go)
start "" explorer.exe C:\Windows
sbshow.exe max CabinetWClass 30 >> %L% 2>&1
ping -n 8 127.0.0.1 > nul
sbshow.exe close CabinetWClass >> %L% 2>&1
start "" notepad.exe
sbshow.exe max Notepad 20 >> %L% 2>&1
sbshow.exe type "soak round %1 The quick brown fox jumps over the lazy dog 0123456789" >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im notepad.exe >> %L% 2>&1
rem the row: counters, memory, DWM
for /f "tokens=1,3" %%a in ('reg query "%K%" 2^>nul ^| findstr /r "Live_FaultCount Live_FaultsInLive Live_JobsTimedOut Live_GpuResets Live_ScratchMaps Live_HangCount Live_TdrEngineResets"') do echo soak %1 %%a=%%b >> %L%
sbshow.exe disk >> %L% 2>&1
for /f "tokens=2" %%p in ('tasklist /fi "imagename eq dwm.exe" /nh') do echo soak %1 dwm pid %%p >> %L%
for /f "tokens=5" %%m in ('tasklist /fi "imagename eq dwm.exe" /nh') do echo soak %1 dwm mem %%m K >> %L%
ping -n 30 127.0.0.1 > nul
exit /b 0
