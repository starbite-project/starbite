@echo off
rem sb-smooth.bat (Track K T72, docs/WIN11-GPU.md 6.96), staged as gputest\sb-user.bat: sb-gputest.cmd hands over to
rem this at logon INSTEAD of every other test. How smooth is the user's desktop on the big cores, in numbers, and how
rem does it change in the first minutes after logon (the user: "the desktop was so slow i had to fiddle around for
rem 5 mins to get to the startmenu")? sbsmooth.exe (CPU speed per core, timer grain, wake-up time, DWM frame pace
rem with a window and full screen, input-to-frame time) right at logon and at about 1, 2.5 and 5 minutes; sbdrag at
rem the end (window drag pace, as T25-T57); typeperf every 2 s over the whole run (CPU per core, DPC/interrupt,
rem queue, memory, paging, disk) into umdlog\perf-smooth.csv; the process list at the start and the end.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
cd /d %D%
echo SMOOTH1 start %date% %time% >> %L%
start "" /b typeperf -cf %D%\sb-perf-counters.txt -si 2 -f CSV -o %U%\perf-smooth.csv -y > %U%\typeperf-smooth.txt 2>&1
tasklist /v /fo csv > %U%\tasks-smooth-start.csv 2>&1
call :run logon
ping -n 31 127.0.0.1 > nul
call :run t60
ping -n 61 127.0.0.1 > nul
call :run t150
ping -n 121 127.0.0.1 > nul
call :run t300
if exist %D%\sbdrag.exe (
  %D%\sbdrag.exe t300 %D%\sbdrag.log >> %L% 2>&1
  echo SMOOTH1 sbdrag exit !errorlevel! >> %L%
)
tasklist /v /fo csv > %U%\tasks-smooth-end.csv 2>&1
powercfg /getactivescheme >> %L% 2>&1
taskkill /im typeperf.exe > nul 2>&1
ping -n 4 127.0.0.1 > nul
echo SMOOTH1 end %date% %time% >> %L%
shutdown /r /t 0 /c "Starbite SMOOTH1 done"
exit /b 0

:run
echo SMOOTH1 run %1 at %time% >> %L%
%D%\sbsmooth.exe %1 %D%\sbsmooth.log >> %L% 2>&1
echo SMOOTH1 run %1 exit !errorlevel! at %time% >> %L%
exit /b 0
