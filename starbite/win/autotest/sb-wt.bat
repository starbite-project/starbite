@echo off
rem sb-wt.bat (Track K T74, docs/WIN11-GPU-FULL-DRIVER.md item 2), staged as gputest\sb-user.bat: sb-gputest.cmd hands
rem over to this at logon INSTEAD of every other test. The conformance run: each staged Wine test suite
rem (<suite>_test.exe from tools/win/winetests/build.sh) through sbwt.exe, one test function per process with a
rem 90 s timeout; one PASS / FAIL / CRASH / TIMEOUT line per test in umdlog\sbwt-<suite>.log (and here), each
rem test's full output in umdlog\sbwt-<suite>.log.out\<test>.txt. Then the Application crash events, and a restart.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
cd /d %D%
echo WT1 start %date% %time% >> %L%
ping -n 21 127.0.0.1 > nul
for %%S in (d3d10core d3d11 dxgi) do if exist %D%\%%S_test.exe (
  echo WT1 suite %%S start %time% >> %L%
  %D%\sbwt.exe %D%\%%S_test.exe %%S %U%\sbwt-%%S.log 90 >> %L% 2>&1
  echo WT1 suite %%S sbwt exit !errorlevel! at !time! >> %L%
)
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:40 /f:text > %U%\wt-app-events.txt 2>&1
echo WT1 end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite WT1 done"
exit /b 0
