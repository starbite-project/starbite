@echo off
rem VIS1 (Track K T54, docs/WIN11-GPU.md 6.59): do windowed D3D11 swap chains reach the screen at all?
rem T52: the zero-copy GL present (a DXGI FLIP_SEQUENTIAL swap chain per window) drew the right frames into its
rem buffers, but Geometry Dash never appeared in the screen snaps, and DWM's umdlog shows no OpenResource of any app
rem swap chain buffer (sbfps's included): only DComp surfaces. sbfps's 600-800 fps may be frames nobody shows.
rem Here each windowed swap chain runs ~20 s while sbshow takes a GDI shot + DWM dump of the screen:
rem   32 sbfps flip-model (FLIP_SEQUENTIAL)   33 sbfps blt-model (DISCARD)   (full-screen window, changing colour)
rem   34 sbglzc zero-copy, FLIP_SEQUENTIAL    35 sbglzc zero-copy, DISCARD    36 sbglzc GDI present (control: shown in T51)
rem   (sbglzc: a 1280x720 window at 20,20 with 8 colour bars and a moving white square)
rem Called by sb-gputest.cmd after ZC1 when staged. Never waits more than ~60 s per program (taskkill after).
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
cd /d %D%
echo VIS1 start %time% >> %L%

start "sbfps-vis" sbfps.exe hw 20 sbfps-vis.log
ping -n 9 127.0.0.1 > nul
sbshow.exe snap 32-vis-sbfps-flip >> %L% 2>&1
ping -n 21 127.0.0.1 > nul
sbshow.exe snap 33-vis-sbfps-blt >> %L% 2>&1
call :waitexit sbfps.exe
if exist sbfps-vis.log type sbfps-vis.log >> %L%

if not exist %D%\glzc\sbglzc64.exe (echo VIS1: no glzc\sbglzc64.exe, sbglzc part skipped >> %L% & goto end)
pushd %D%\glzc
set SBGLZC_SECS=20
set SB_GL_ZC_LOG=1
call :glzc 34-vis-glzc-flip
set SB_GL_ZC_SWAP=discard
call :glzc 35-vis-glzc-discard
set SB_GL_ZC_SWAP=
set SB_GL_PRESENT=gdi
call :glzc 36-vis-glzc-gdi
set SB_GL_PRESENT=
set SB_GL_ZC_LOG=
set SBGLZC_SECS=
popd

:end
echo VIS1 end %time% >> %L%
endlocal
exit /b 0

:glzc
echo VIS1 %1: SB_GL_PRESENT=%SB_GL_PRESENT% SB_GL_ZC_SWAP=%SB_GL_ZC_SWAP% >> %L%
start "sbglzc-vis" sbglzc64.exe one %D%\glzc-%1.log
ping -n 9 127.0.0.1 > nul
sbshow.exe snap %1 >> %L% 2>&1
call :waitexit sbglzc64.exe
if exist %D%\glzc-%1.log type %D%\glzc-%1.log >> %L%
exit /b 0

:waitexit
for /l %%T in (1,1,30) do (
  tasklist /fi "imagename eq %1" 2>nul | find /i "%1" > nul
  if errorlevel 1 (echo VIS1 %1 ended >> %L% & exit /b 0)
  ping -n 3 127.0.0.1 > nul
)
echo VIS1 %1 still running after ~60 s: taskkill >> %L%
taskkill /f /im %1 >> %L% 2>&1
exit /b 0
