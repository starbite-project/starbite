@echo off
rem gd-gl-on.cmd [x64|ec] [log] (docs/WIN11-GPU-CAPS.md 5a step 2): give Geometry Dash our OpenGL driver for the
rem phone's GPU. Copies Mesa's opengl32.dll + libgallium_wgl.dll (x64 build, or "ec" = ARM64EC: the driver's CPU
rem side runs natively) from this folder into the game's folder, where Windows loads them before its own GL 1.1
rem fallback. Only Geometry Dash is affected. "log" also turns on Mesa's GL error reports (MESA_DEBUG) for your
rem user; GD's driver messages always go to C:\ProgramData\Starbite\umdlog\GeometryDash.exe-*.log.
rem Undo (and collect the logs into gd-logs\): gd-gl-off.cmd.
setlocal
set G=C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash
set S=%~dp0
set A=x64
set L=0
for %%P in (%*) do (
  if /i "%%P"=="ec" set A=ec
  if /i "%%P"=="x64" set A=x64
  if /i "%%P"=="log" set L=1
)
if not exist "%G%\GeometryDash.exe" (echo Geometry Dash not found in "%G%" & pause & exit /b 1)
if exist "%G%\opengl32.dll" if not exist "%G%\opengl32.dll.starbite" (echo "%G%\opengl32.dll" exists and is not ours: not touching it & pause & exit /b 1)
copy /y "%S%opengl32-%A%.dll" "%G%\opengl32.dll" > nul || goto denied
copy /y "%S%libgallium_wgl-%A%.dll" "%G%\libgallium_wgl.dll" > nul || goto denied
echo starbite > "%G%\opengl32.dll.starbite"
if not exist C:\ProgramData\Starbite\umdlog mkdir C:\ProgramData\Starbite\umdlog 2> nul
if "%L%"=="1" setx MESA_DEBUG 1 > nul
echo %date% %time% on %A% log=%L% >> "%S%gd-gl.log"
echo Done (%A%).
if "%L%"=="1" echo Logging is on: quit Steam completely (tray icon, Exit) and start it again before starting the game.
echo Start Geometry Dash from Steam as usual. To undo and collect the logs: gd-gl-off.cmd
pause
exit /b 0
:denied
echo Could not write to the game folder: right-click this file and choose "Run as administrator".
pause
exit /b 1
