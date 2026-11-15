@echo off
rem sb-elev-run.cmd (Track K T70, docs/WIN11-GPU.md 6.95): runs ONCE, ELEVATED, at the test boot's logon from HKLM
rem RunOnce (tools/win/runonce-arm.sh StarbiteElev ... 'starbite\vis'). Generic: waits for the stage's gputest files,
rem runs C:\starbite\auto\gpu\sb-elev.bat, then writes vis-elev.done (the stage's sb-user.bat waits for it and restarts).
setlocal
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
for /l %%T in (1,1,60) do (
  if exist %D%\sb-elev.bat goto ready
  ping -n 3 127.0.0.1 > nul
)
echo ELEV: no sb-elev.bat in %D% after 2 min >> %L%
goto done
:ready
set ELEV=0
net session > nul 2>&1 && set ELEV=1
echo ELEV start %date% %time% elevated %ELEV% >> %L%
call %D%\sb-elev.bat
echo ELEV end %date% %time% >> %L%
:done
echo done> %D%\vis-elev.done
exit /b 0
