@echo off
rem sb-wt-multi.bat (Track K T77, docs/WIN11-GPU-FULL-DRIVER.md), staged as gputest\sb-user.bat: the conformance
rem suites in several passes, each with its own set of sbumd flag files. sb-wt-passes.txt has one pass per line:
rem     LABEL  suite[+suite...]  [flag flag ...]
rem e.g. "N d3d10core+d3d11 nirshaders" (a comma would split the batch arguments). Every test function is its own process and reads the flags when it starts.
rem Logs: umdlog\sbwt-<suite>-<LABEL>.log (+ .out folders). All flags named in the file are removed at the start and
rem after each pass.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set P=C:\ProgramData\Starbite
cd /d %D%
echo WT3 start %date% %time% >> %L%
if not exist %D%\sb-wt-passes.txt (echo WT3: no sb-wt-passes.txt >> %L% & goto done)
for /f "usebackq tokens=1,2,*" %%a in ("%D%\sb-wt-passes.txt") do for %%F in (%%c) do del /f %P%\%%F > nul 2>&1
ping -n 21 127.0.0.1 > nul
for /f "usebackq tokens=1,2,*" %%a in ("%D%\sb-wt-passes.txt") do call :pass %%a %%b "%%c"
:done
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:40 /f:text > %U%\wt-app-events.txt 2>&1
echo WT3 end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite WT3 done"
exit /b 0

:pass
echo WT3 pass %1: suites %2, flags %~3, start !time! >> %L%
for %%F in (%~3) do echo on> %P%\%%F
set SU=%2
set SU=!SU:+= !
for %%S in (!SU!) do if exist %D%\%%S_test.exe (
  %D%\sbwt.exe %D%\%%S_test.exe %%S %U%\sbwt-%%S-%1.log 90 >> %L% 2>&1
  echo WT3 pass %1 suite %%S sbwt exit !errorlevel! at !time! >> %L%
)
for %%F in (%~3) do del /f %P%\%%F > nul 2>&1
exit /b 0
