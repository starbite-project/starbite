@echo off
rem sb-wt-env.bat (Track K T94, docs/WIN11-GPU.md 6.126), staged as gputest\sb-user.bat: the conformance suites in
rem several passes, as sb-wt-multi.bat, but a pass's driver switches are ENVIRONMENT VARIABLES of the test processes
rem (SBSW_<name>=1, read by sbumd from fix build B on), not flag files. T90 showed why: the flag file nirshaders
rem switched the new translator on for every process on the system, Windows' own Settings app included, and the
rem test stopped. sb-wt-passes.txt has one pass per line:    LABEL  suite[+suite...]  [switch switch ...]
rem A heartbeat (sb-beat.bat, started here) writes the time and the programs that do not respond to
rem umdlog\beat.txt once a minute, so a stall says what was stuck.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set P=C:\ProgramData\Starbite
cd /d %D%
echo WT4 start %date% %time% >> %L%
if not exist %D%\sb-wt-passes.txt (echo WT4: no sb-wt-passes.txt >> %L% & goto done)
rem no switch may be left on as a FILE from an earlier test
for %%F in (nirshaders gsemu msaa d3d11ddi fl11 tessemu) do if exist %P%\%%F (echo WT4 removing the flag FILE %%F >> %L% & del /f %P%\%%F > nul 2>&1)
if exist %D%\sb-beat.bat start "sb-beat" /min cmd /c %D%\sb-beat.bat
ping -n 21 127.0.0.1 > nul
for /f "usebackq tokens=1,2,*" %%a in ("%D%\sb-wt-passes.txt") do call :pass %%a %%b "%%c"
:done
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:40 /f:text > %U%\wt-app-events.txt 2>&1
echo WT4 end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite WT4 done"
exit /b 0

:pass
echo WT4 pass %1: suites %2, switches %~3 (environment), start !time! >> %L%
for %%F in (%~3) do set SBSW_%%F=1
set SU=%2
set SU=!SU:+= !
for %%S in (!SU!) do if exist %D%\%%S_test.exe (
  echo WT4 pass %1 suite %%S: starting sbwt at !time! >> %L%
  %D%\sbwt.exe %D%\%%S_test.exe %%S %U%\sbwt-%%S-%1.log 90 >> %L% 2>&1
  echo WT4 pass %1 suite %%S sbwt exit !errorlevel! at !time! >> %L%
)
for %%F in (%~3) do set SBSW_%%F=
exit /b 0
