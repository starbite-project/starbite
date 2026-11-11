@echo off
rem gd-gl-off.cmd: undo gd-gl-on.cmd (removes only the files it put into the Geometry Dash folder).
setlocal
set G=C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash
if not exist "%G%\opengl32.dll.starbite" (echo Nothing of ours in "%G%". & pause & exit /b 0)
del /f "%G%\opengl32.dll" "%G%\libgallium_wgl.dll" "%G%\opengl32.dll.starbite" || (echo Run as administrator. & pause & exit /b 1)
echo %date% %time% off >> "%~dp0gd-gl.log"
echo Removed. Geometry Dash is back to Windows' own OpenGL.
pause
