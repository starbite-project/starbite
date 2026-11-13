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
rem T45 (docs/WIN11-GPU-CAPS.md 5c): D3D11 from a native and from an x64 (emulated) program: which adapter, is our
rem sbumd.dll loaded in the process, and do a clear and a triangle come out right. Exit 0 = on the Mali.
if exist %D%\sbd3d.exe sbd3d.exe %D%\sbd3d.log >> %D%\gputest.log 2>&1
if exist %D%\sbd3d.exe echo sbd3d arm64 exit %errorlevel% >> %D%\gputest.log
if exist %D%\sbd3d64.exe sbd3d64.exe %D%\sbd3d.log >> %D%\gputest.log 2>&1
if exist %D%\sbd3d64.exe echo sbd3d x64 exit %errorlevel% >> %D%\gputest.log
rem ROT1 (docs/WIN11-GPU.md 6.57): landscape on the full driver. sbrotate.exe sets the orientation with Windows' own
rem ChangeDisplaySettingsEx; screenshots and fps in each orientation; the run then stays in landscape (GD2 and the
rem sweep run sideways, as the user wants it). Exit codes: 0 ok, 1 failed (e.g. the driver refused the rotation).
if exist %D%\sbrotate.exe (
  sbrotate.exe query >> %D%\gputest.log 2>&1
  sbrotate.exe landscape-flipped >> %D%\gputest.log 2>&1
  ping -n 6 127.0.0.1 > nul
  sbshow.exe snap 30-landscape-flipped >> %D%\gputest.log 2>&1
  sbrotate.exe landscape >> %D%\gputest.log 2>&1
  ping -n 6 127.0.0.1 > nul
  sbrotate.exe query >> %D%\gputest.log 2>&1
  sbshow.exe snap 31-landscape >> %D%\gputest.log 2>&1
  sbfps.exe hw 10 sbfps-landscape.log >> %D%\gputest.log 2>&1
  sbd3d.exe %D%\sbd3d-landscape.log >> %D%\gputest.log 2>&1
)
if exist %D%\sbrotate.exe echo sbd3d landscape exit %errorlevel% >> %D%\gputest.log
rem UMDX (docs/WIN11-GPU-CAPS.md 5c): does Windows load the ARM64X sbumd (one file, an ARM64 view for native
rem processes and an ARM64EC view for x64 ones) and the ARM64EC one? Load only, from this folder: nothing is
rem installed, DWM keeps the T44 sbumd.dll, and no DDI entry point is called.
if exist %D%\sbumd-arm64x.dll sbload.exe %D%\sbumd-arm64x.dll OpenAdapter10 OpenAdapter10_2 >> %D%\gputest.log 2>&1
if exist %D%\sbumd-arm64x.dll echo sbload arm64 arm64x exit %errorlevel% >> %D%\gputest.log
if exist %D%\sbumd-arm64x.dll sbload64.exe %D%\sbumd-arm64x.dll OpenAdapter10 OpenAdapter10_2 >> %D%\gputest.log 2>&1
if exist %D%\sbumd-arm64x.dll echo sbload x64 arm64x exit %errorlevel% >> %D%\gputest.log
if exist %D%\sbumd-ec.dll sbload64.exe %D%\sbumd-ec.dll OpenAdapter10 OpenAdapter10_2 >> %D%\gputest.log 2>&1
if exist %D%\sbumd-ec.dll echo sbload x64 ec exit %errorlevel% >> %D%\gputest.log
rem GD1 (docs/WIN11-GPU-CAPS.md 2, 5a): Geometry Dash itself on our x64 OpenGL driver, unattended. Only if the
rem game is installed and opengl32-gd.dll is staged (the x64 ICD): our two DLLs go into the game folder (marked as ours, never
rem over a foreign opengl32.dll), Steam starts it, 45 s later a screenshot, then the game is closed and our files
rem removed (left in place if the user's kit had put them there). The game's driver log is umdlog\GeometryDash.exe-*.log.
set G=C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash
set GDKIT=0
if exist %D%\sb-gd.bat goto gd1_done
if not exist %D%\opengl32-gd.dll goto gd1_done
if not exist "%G%\GeometryDash.exe" goto gd1_clean
if exist "%G%\opengl32.dll.starbite" set GDKIT=1
if exist "%G%\opengl32.dll" if "%GDKIT%"=="0" (echo GD1 skipped: a foreign opengl32.dll is in the game folder >> %D%\gputest.log & goto gd1_clean)
copy /y %D%\opengl32-gd.dll "%G%\opengl32.dll" > nul
copy /y %D%\libgallium_wgl-gd.dll "%G%\libgallium_wgl.dll" > nul
echo starbite > "%G%\opengl32.dll.starbite"
echo GD1 start %time% (kit already there: %GDKIT%) >> %D%\gputest.log
start "" steam://rungameid/322170
set /a GDW=0
:gd1_wait
ping -n 6 127.0.0.1 > nul
set /a GDW+=5
tasklist /fi "imagename eq GeometryDash.exe" | find /i "GeometryDash.exe" > nul && goto gd1_up
if %GDW% lss 180 goto gd1_wait
echo GD1 GeometryDash.exe never appeared in 180 s (Steam not signed in or too slow) >> %D%\gputest.log
goto gd1_stop
:gd1_up
echo GD1 GeometryDash.exe up after %GDW% s >> %D%\gputest.log
ping -n 41 127.0.0.1 > nul
:gd1_stop
tasklist /fi "imagename eq GeometryDash.exe" >> %D%\gputest.log 2>&1
sbshow.exe snap 27-geometrydash >> %D%\gputest.log 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im GeometryDash.exe >> %D%\gputest.log 2>&1
ping -n 4 127.0.0.1 > nul
echo GD1 end %time% >> %D%\gputest.log
if "%GDKIT%"=="0" del /f "%G%\opengl32.dll" "%G%\libgallium_wgl.dll" "%G%\opengl32.dll.starbite"
:gd1_clean
del /f %D%\opengl32-gd.dll %D%\libgallium_wgl-gd.dll
:gd1_done
rem ZC1 (sb-patches 0150/0151, docs/WIN11-GPU-ZEROCOPY.md): the zero-copy GL present. sbglzc64.exe (an x64 GL program)
rem draws 300 frames at 1280x720 through GDI, then 300 through the zero-copy path (DXGI flip swap chain on our
rem adapter), with the zero-copy ICD app-local in glzc\; it prints fps for each, which path the driver took, and readback.
if exist %D%\sbglzc64.exe (
  if not exist %D%\glzc mkdir %D%\glzc
  move /y %D%\sbglzc64.exe %D%\glzc\ > nul
  move /y %D%\opengl32-zc.dll %D%\glzc\opengl32.dll > nul
  move /y %D%\libgallium_wgl-zc.dll %D%\glzc\libgallium_wgl.dll > nul
)
if exist %D%\glzc\sbglzc64.exe pushd %D%\glzc
if exist %D%\glzc\sbglzc64.exe sbglzc64.exe >> %D%\gputest.log 2>&1
if exist %D%\glzc\sbglzc64.exe echo sbglzc x64 exit %errorlevel% >> %D%\gputest.log
if exist %D%\glzc\sbglzc64.exe popd
rem VIS1 (T54, sb-vis.bat): do windowed D3D11 swap chains (sbfps, zero-copy sbglzc) reach the screen? Snaps 32-36.
if exist %D%\sb-vis.bat call %D%\sb-vis.bat
rem VIS2 (T56, sb-comp.bat): HWND swap chains vs DirectComposition (sbcomp), each with a snap and an ETW present trace.
if exist %D%\sb-comp.bat call %D%\sb-comp.bat
rem T44: free the ~110 MB the GL and UMD tests brought before the sweep (C: had 841 MiB in T42 and Explorer's
rem pagefile growth took 490 of it); their results are in gputest.log and umdlog already.
for %%F in (gl glx64 glec glx86 glzc) do if exist %D%\%%F rmdir /s /q %D%\%%F
if exist %D%\sbumd-arm64x.dll del /f %D%\sbumd-arm64x.dll
if exist %D%\sbumd-ec.dll del /f %D%\sbumd-ec.dll
sbshow.exe disk >> %D%\gputest.log 2>&1
rem T37: the sweep (sb-sweep.bat: ~27 surfaces, each as a GDI shot and a DWM dump, then the event logs); it replaces
rem T36's shell steps (the keyboard A/B and the hit grids are in it). Run with --full-sec 900 and no keys self-test.
rem Track K soak (sb-soak.bat): when staged it replaces the sweep (gpu-cycle.sh --full-sec 3600)
if exist %D%\sb-soak.bat goto soak
if exist %D%\sb-gd.bat goto gd2
if exist %D%\sb-sweep.bat call %D%\sb-sweep.bat
goto after_sweep
:soak
call %D%\sb-soak.bat
goto after_sweep
:gd2
rem T53: with sb-gd-ab.txt staged, an A/B: run A plain, run B with a flag file C:\ProgramData\Starbite\FLAG (the game
rem reads it when it starts), then the flag is removed again. FLAG = the first word of sb-gd-ab.txt (T55: glasync, the
rem async present, sb-patches 0153); empty: panthreaded (T53, sb-patches 0152).
set ABF=panthreaded
if exist %D%\sb-gd-ab.txt for /f "usebackq tokens=1" %%a in ("%D%\sb-gd-ab.txt") do set ABF=%%a
if not exist %D%\sb-gd-ab.txt call %D%\sb-gd.bat A
rem T62: a second word "twice" in sb-gd-ab.txt runs A B A2 B2 (T61 on the big cores: A sat on the menu, B was in a
rem level, so one pass each compared two different scenes).
set ABN=
if exist %D%\sb-gd-ab.txt for /f "usebackq tokens=2" %%a in ("%D%\sb-gd-ab.txt") do set ABN=%%a
if exist %D%\sb-gd-ab.txt (
  call %D%\sb-gd.bat A
  echo on> C:\ProgramData\Starbite\%ABF%
  call %D%\sb-gd.bat B
  del C:\ProgramData\Starbite\%ABF%
  if /i "%ABN%"=="twice" (
    call %D%\sb-gd.bat A2
    echo on> C:\ProgramData\Starbite\%ABF%
    call %D%\sb-gd.bat B2
    del C:\ProgramData\Starbite\%ABF%
  )
)
:after_sweep
echo shell surfaces opened %time% >> gputest.log
echo sb-gputest end %date% %time% >> gputest.log
rem T37: with the sweep, the boot restarts itself as soon as everything is written (sbwdog's timer is the backstop)
if exist %D%\sb-sweep.bat shutdown /r /t 15 /c "Starbite gputest done"
if exist %D%\sb-soak.bat shutdown /r /t 15 /c "Starbite soak done"
if exist %D%\sb-gd.bat shutdown /r /t 15 /c "Starbite GD diagnostic done"
