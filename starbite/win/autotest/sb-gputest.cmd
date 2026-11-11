@echo off
rem sb-gputest.cmd (docs/WIN11-GPU.md 6.18): at logon in a full-mode GPU test boot, render tests on the Mali
rem through the hardware UMD (sbumd), then a software reference. One-shot: armed by tools/win/gpu-cycle.sh with
rem C:\starbite\auto\gpu\armed. Output: C:\starbite\auto\gpu\gputest.log, sbtex*.log, triangle.ppm.
set D=C:\starbite\auto\gpu
if not exist %D%\armed exit /b 0
del %D%\armed
cd /d %D%
echo sb-gputest start %date% %time% >> gputest.log
rem T32: what Windows made of the firmware's SMBIOS (Task Manager's CPU name is ProcessorNameString; the
rem HARDWARE key is volatile, so it is in no hive); takes no time, runs before anything that could hang
reg query "HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0" /v ProcessorNameString >> gputest.log 2>&1
reg query "HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0" /v VendorIdentifier >> gputest.log 2>&1
reg query "HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0" /v ~MHz >> gputest.log 2>&1
reg query "HKLM\HARDWARE\DESCRIPTION\System\BIOS" >> gputest.log 2>&1
rem give DWM and the session a moment to settle
ping -n 16 127.0.0.1 > nul
rem T23: a window drag, DWM fps and where the time goes; with Starbite link (sblinkd) running, then without
sbdrag.exe link sbdrag.log >> gputest.log 2>&1
echo sbdrag link exit %errorlevel% >> gputest.log
taskkill /f /im sblinkd.exe >> gputest.log 2>&1
ping -n 3 127.0.0.1 > nul
sbdrag.exe nolink sbdrag.log >> gputest.log 2>&1
echo sbdrag nolink exit %errorlevel% >> gputest.log
triangle.exe hw >> gputest.log 2>&1
echo triangle hw exit %errorlevel% >> gputest.log
if exist triangle.ppm move /y triangle.ppm triangle-hw.ppm > nul
rem T15: several D3D devices in one process (Explorer has two; sb-patches/0113), six at once, and a pace
sbtwo.exe hw sbtwo.log >> gputest.log 2>&1
echo sbtwo hw exit %errorlevel% >> gputest.log
rem T17: DirectWrite text, a WIC image and Direct2D shapes, the Mali against WARP (sbtext-hw.ppm, sbtext-warp.ppm)
sbtext.exe hw sbtext.log >> gputest.log 2>&1
echo sbtext hw exit %errorlevel% >> gputest.log
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
rem GL1 (docs/WIN11-GPU-CAPS.md 5a): OpenGL on the Mali, Mesa WGL + Panfrost (sb-patches/0136), app-local in gl\
rem (own folder: no other test may load that opengl32.dll); then the same ICD on softpipe, which checks the WGL
rem plumbing alone. Output in gputest.log; the frames in umdlog\sbgl-*.ppm.
if exist %D%\sbgl.exe (
  if not exist %D%\gl mkdir %D%\gl
  move /y %D%\sbgl.exe %D%\gl\ > nul
  move /y %D%\opengl32.dll %D%\gl\ > nul
  move /y %D%\libgallium_wgl.dll %D%\gl\ > nul
)
if exist %D%\gl\sbgl.exe pushd %D%\gl
if exist %D%\gl\sbgl.exe sbgl.exe hw %D%\gl\sbgl.log C:\ProgramData\Starbite\umdlog\sbgl-hw.ppm >> %D%\gputest.log 2>&1
if exist %D%\gl\sbgl.exe echo sbgl hw exit %errorlevel% >> %D%\gputest.log
if exist %D%\gl\sbgl.exe cmd /c "set GALLIUM_DRIVER=softpipe&& sbgl.exe any %D%\gl\sbgl.log C:\ProgramData\Starbite\umdlog\sbgl-softpipe.ppm" >> %D%\gputest.log 2>&1
if exist %D%\gl\sbgl.exe echo sbgl softpipe exit %errorlevel% >> %D%\gputest.log
if exist %D%\gl\sbgl.exe popd
rem GL2 (docs/WIN11-GPU-CAPS.md 5a step 2): the same test as an x64 program (sbgl64.exe, run by Windows' x64
rem emulator like Geometry Dash) with the ICD built for x64 (opengl32-x64.dll, libgallium_wgl-x64.dll -> gl64\)
rem and, if staged, built for ARM64EC (sbgl-ec.exe, opengl32-ec.dll, libgallium_wgl-ec.dll -> glec\) and for x86
rem (32-bit games: sbgl32.exe, opengl32-x86.dll, libgallium_wgl-x86.dll -> glx86\, run under WOW64).
for %%A in (x64 ec x86) do (
  if exist %D%\libgallium_wgl-%%A.dll (
    if not exist %D%\gl%%A mkdir %D%\gl%%A
    move /y %D%\libgallium_wgl-%%A.dll %D%\gl%%A\libgallium_wgl.dll > nul
    move /y %D%\opengl32-%%A.dll %D%\gl%%A\opengl32.dll > nul
  )
)
if exist %D%\sbgl64.exe move /y %D%\sbgl64.exe %D%\glx64\ > nul
if exist %D%\sbgl-ec.exe move /y %D%\sbgl-ec.exe %D%\glec\ > nul
if exist %D%\glx64\sbgl64.exe pushd %D%\glx64
if exist %D%\glx64\sbgl64.exe sbgl64.exe hw %D%\glx64\sbgl.log C:\ProgramData\Starbite\umdlog\sbgl-x64.ppm >> %D%\gputest.log 2>&1
if exist %D%\glx64\sbgl64.exe echo sbgl x64 exit %errorlevel% >> %D%\gputest.log
if exist %D%\glx64\sbgl64.exe popd
if exist %D%\glec\sbgl-ec.exe pushd %D%\glec
if exist %D%\glec\sbgl-ec.exe sbgl-ec.exe hw %D%\glec\sbgl.log C:\ProgramData\Starbite\umdlog\sbgl-ec.ppm >> %D%\gputest.log 2>&1
if exist %D%\glec\sbgl-ec.exe echo sbgl ec exit %errorlevel% >> %D%\gputest.log
if exist %D%\glec\sbgl-ec.exe popd
if exist %D%\sbgl32.exe move /y %D%\sbgl32.exe %D%\glx86\ > nul
if exist %D%\glx86\sbgl32.exe pushd %D%\glx86
if exist %D%\glx86\sbgl32.exe sbgl32.exe hw %D%\glx86\sbgl.log C:\ProgramData\Starbite\umdlog\sbgl-x86.ppm >> %D%\gputest.log 2>&1
if exist %D%\glx86\sbgl32.exe echo sbgl x86 exit %errorlevel% >> %D%\gputest.log
if exist %D%\glx86\sbgl32.exe popd
rem UMDX (docs/WIN11-GPU-CAPS.md 5c): does Windows load the ARM64X sbumd (one file, an ARM64 view for native
rem processes and an ARM64EC view for x64 ones) and the ARM64EC one? Load only, from this folder: nothing is
rem installed, DWM keeps the T44 sbumd.dll, and no DDI entry point is called.
if exist %D%\sbumd-arm64x.dll sbload.exe %D%\sbumd-arm64x.dll OpenAdapter10 OpenAdapter10_2 >> %D%\gputest.log 2>&1
if exist %D%\sbumd-arm64x.dll echo sbload arm64 arm64x exit %errorlevel% >> %D%\gputest.log
if exist %D%\sbumd-arm64x.dll sbload64.exe %D%\sbumd-arm64x.dll OpenAdapter10 OpenAdapter10_2 >> %D%\gputest.log 2>&1
if exist %D%\sbumd-arm64x.dll echo sbload x64 arm64x exit %errorlevel% >> %D%\gputest.log
if exist %D%\sbumd-ec.dll sbload64.exe %D%\sbumd-ec.dll OpenAdapter10 OpenAdapter10_2 >> %D%\gputest.log 2>&1
if exist %D%\sbumd-ec.dll echo sbload x64 ec exit %errorlevel% >> %D%\gputest.log
rem T37: the sweep (sb-sweep.bat: ~27 surfaces, each as a GDI shot and a DWM dump, then the event logs); it replaces
rem T36's shell steps (the keyboard A/B and the hit grids are in it). Run with --full-sec 900 and no keys self-test.
if exist %D%\sb-sweep.bat call %D%\sb-sweep.bat
echo shell surfaces opened %time% >> gputest.log
echo sb-gputest end %date% %time% >> gputest.log
rem T37: with the sweep, the boot restarts itself as soon as everything is written (sbwdog's timer is the backstop)
if exist %D%\sb-sweep.bat shutdown /r /t 15 /c "Starbite gputest done"
