@echo off
rem sb-vis-elev.cmd (Track K T66, docs/WIN11-GPU.md 6.85): runs ONCE, ELEVATED, at the test boot's logon from HKLM
rem RunOnce StarbiteVis (tools/win/vis-arm.sh). VIS2 (sb-comp.bat) needs administrator rights for its ETW session
rem (logman: "Access is denied" from the Startup autotest in T56/T58/T60): DXGI, DxgKrnl (Base + Present) and Dwm-Core
rem events while sbcomp presents in each mode, so we see where a windowed swap chain's present stops.
setlocal
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
for /l %%T in (1,1,60) do (
  if exist %D%\sb-comp.bat goto ready
  ping -n 3 127.0.0.1 > nul
)
echo VIS-ELEV: no sb-comp.bat in %D% after 2 min >> %L%
goto done
:ready
set ELEV=0
net session > nul 2>&1 && set ELEV=1
echo VIS-ELEV start %date% %time% elevated %ELEV% >> %L%
call %D%\sb-comp.bat
echo VIS-ELEV end %date% %time% >> %L%
:done
echo done> %D%\vis-elev.done
exit /b 0
