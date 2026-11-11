@echo off
rem gd-gl-off.cmd: undo gd-gl-on.cmd (removes only the files it put into the Geometry Dash folder and the
rem MESA_DEBUG setting) and copies the game's driver logs into gd-logs\ next to this file, for the next pull.
setlocal
set G=C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash
set S=%~dp0
if not exist "%S%gd-logs" mkdir "%S%gd-logs"
copy /y C:\ProgramData\Starbite\umdlog\GeometryDash.exe-*.log "%S%gd-logs\" > nul 2>&1
reg delete HKCU\Environment /v MESA_DEBUG /f > nul 2>&1
if not exist "%G%\opengl32.dll.starbite" (echo Nothing of ours in "%G%". Logs copied to gd-logs. & pause & exit /b 0)
del /f "%G%\opengl32.dll" "%G%\libgallium_wgl.dll" "%G%\opengl32.dll.starbite" || (echo Run as administrator. & pause & exit /b 1)
echo %date% %time% off >> "%S%gd-gl.log"
echo Removed. Geometry Dash is back to Windows' own OpenGL. Logs copied to gd-logs.
pause
