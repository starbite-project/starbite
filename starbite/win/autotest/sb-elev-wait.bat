@echo off
rem sb-elev-wait.bat (Track K T70, docs/WIN11-GPU.md 6.95), staged as gputest\sb-user.bat: sb-gputest.cmd hands over
rem to this at logon; it only waits for the elevated run (sb-elev-run.cmd from HKLM RunOnce, which calls the stage's
rem sb-elev.bat) to write vis-elev.done, then restarts.
setlocal
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
echo ELEV waiter start %time% >> %L%
for /l %%T in (1,1,120) do (
  if exist %D%\vis-elev.done goto done
  ping -n 6 127.0.0.1 > nul
)
echo ELEV waiter: no vis-elev.done after ~10 min: the RunOnce did not run? >> %L%
:done
echo ELEV waiter end %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite elevated test done"
exit /b 0
