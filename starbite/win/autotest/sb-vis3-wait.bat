@echo off
rem sb-user.bat for T66 (VIS3, docs/WIN11-GPU.md 6.85): sb-gputest.cmd hands over to this at logon; it only waits for the
rem elevated VIS2 run (C:\starbite\vis\sb-vis-elev.cmd from HKLM RunOnce) to write vis-elev.done, then restarts.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
echo VIS3 waiter start %time% >> %L%
for /l %%T in (1,1,180) do (
  if exist %D%\vis-elev.done goto done
  ping -n 6 127.0.0.1 > nul
)
echo VIS3: no vis-elev.done after ~15 min (the RunOnce did not run?) >> %L%
:done
echo VIS3 waiter end %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite VIS3 done"
exit /b 0
