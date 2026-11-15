@echo off
rem sb-wt-ab.bat (Track K T76, docs/WIN11-GPU-FULL-DRIVER.md items 2 + 6), staged as gputest\sb-user.bat: the
rem conformance suites twice on one driver, A without and B with a flag file of sbumd's (the first word of
rem sb-wt-ab.txt, e.g. "nirshaders": the DXBC -> NIR shader translator). Each test function is its own process and
rem reads the flag when it starts. Logs: umdlog\sbwt-<suite>-A.log and -B.log (+ .out folders). The flag is removed
rem at the start and at the end.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set P=C:\ProgramData\Starbite
cd /d %D%
set ABF=
if exist %D%\sb-wt-ab.txt for /f "usebackq tokens=1" %%a in ("%D%\sb-wt-ab.txt") do set ABF=%%a
echo WT2 start %date% %time%, flag "%ABF%" >> %L%
if not "%ABF%"=="" del /f %P%\%ABF% > nul 2>&1
ping -n 21 127.0.0.1 > nul
call :pass A
if not "%ABF%"=="" (
  echo on> %P%\%ABF%
  call :pass B
  del /f %P%\%ABF% > nul 2>&1
)
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:40 /f:text > %U%\wt-app-events.txt 2>&1
echo WT2 end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite WT2 done"
exit /b 0

:pass
for %%S in (d3d10core d3d11 dxgi) do if exist %D%\%%S_test.exe (
  echo WT2 pass %1 suite %%S start !time! >> %L%
  %D%\sbwt.exe %D%\%%S_test.exe %%S %U%\sbwt-%%S-%1.log 90 >> %L% 2>&1
  echo WT2 pass %1 suite %%S sbwt exit !errorlevel! at !time! >> %L%
)
exit /b 0
