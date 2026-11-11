@echo off
rem gd-gl-on.cmd (docs/WIN11-GPU-CAPS.md 5a step 2): give Geometry Dash our OpenGL driver for the phone's GPU.
rem Copies Mesa's opengl32.dll + libgallium_wgl.dll (built for x64, Panfrost on the Mali) from this folder into
rem the game's folder, where Windows loads them before its own GL 1.1 fallback. Only Geometry Dash is affected.
rem Undo: gd-gl-off.cmd. Log: %~dp0gd-gl.log (and the game's own window).
setlocal
set G=C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash
set S=%~dp0
if not exist "%G%\GeometryDash.exe" (echo Geometry Dash not found in "%G%" & pause & exit /b 1)
if exist "%G%\opengl32.dll" if not exist "%G%\opengl32.dll.starbite" (echo "%G%\opengl32.dll" exists and is not ours: not touching it & pause & exit /b 1)
copy /y "%S%opengl32-x64.dll" "%G%\opengl32.dll" > nul || goto denied
copy /y "%S%libgallium_wgl-x64.dll" "%G%\libgallium_wgl.dll" > nul || goto denied
echo starbite > "%G%\opengl32.dll.starbite"
echo %date% %time% on >> "%S%gd-gl.log"
echo Done. Start Geometry Dash from Steam as usual. To undo: gd-gl-off.cmd
pause
exit /b 0
:denied
echo Could not write to the game folder: right-click this file and choose "Run as administrator".
pause
exit /b 1
