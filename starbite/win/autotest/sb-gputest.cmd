@echo off
rem sb-gputest.cmd (docs/WIN11-GPU.md 6.18): at logon in a full-mode GPU test boot, render tests on the Mali
rem through the hardware UMD (sbumd), then a software reference. One-shot: armed by tools/win/gpu-cycle.sh with
rem C:\starbite\auto\gpu\armed. Output: C:\starbite\auto\gpu\gputest.log, sbtex*.log, triangle.ppm.
set D=C:\starbite\auto\gpu
if not exist %D%\armed exit /b 0
del %D%\armed
cd /d %D%
echo sb-gputest start %date% %time% >> gputest.log
rem give DWM and the session a moment to settle
ping -n 16 127.0.0.1 > nul
triangle.exe hw >> gputest.log 2>&1
echo triangle hw exit %errorlevel% >> gputest.log
if exist triangle.ppm move /y triangle.ppm triangle-hw.ppm > nul
rem T15: several D3D devices in one process (Explorer has two; sb-patches/0113), six at once, and a pace
sbtwo.exe hw sbtwo.log >> gputest.log 2>&1
echo sbtwo hw exit %errorlevel% >> gputest.log
sbtex.exe hw sbtex-hw.log >> gputest.log 2>&1
echo sbtex hw exit %errorlevel% >> gputest.log
rem T13o: shared surfaces, one process and two (as DWM opens window surfaces)
sbshare.exe hw sbshare.log >> gputest.log 2>&1
echo sbshare hw exit %errorlevel% >> gputest.log
rem T13v: DWM's composition draw, one feature at a time (indexed, constant buffer, blend, 1440x2960 BGRA)
sbdwm.exe hw sbdwm-hw.log >> gputest.log 2>&1
echo sbdwm hw exit %errorlevel% >> gputest.log
if exist libgallium_d3d10-softpipe.dll sbdwm.exe %D%\libgallium_d3d10-softpipe.dll sbdwm-softpipe.log >> gputest.log 2>&1
echo sbdwm softpipe exit %errorlevel% >> gputest.log
if exist libgallium_d3d10-softpipe.dll sbtex.exe %D%\libgallium_d3d10-softpipe.dll sbtex-softpipe.log >> gputest.log 2>&1
echo sbtex softpipe exit %errorlevel% >> gputest.log
rem T14b: a windowed D3D11 app at full speed, 20 s with a blt swap chain and 20 s with a flip one (sbfps.log)
sbfps.exe hw 15 sbfps.log >> gputest.log 2>&1
echo sbfps hw exit %errorlevel% >> gputest.log
rem the SV_VertexID variant last: on the Mali it crashed (T13m: Panfrost has no load_vertex_id_zero_base)
sbtex.exe hw sbtex-hw-vertexid.log vertexid >> gputest.log 2>&1
echo sbtex hw vertexid exit %errorlevel% >> gputest.log
echo sb-gputest end %date% %time% >> gputest.log
