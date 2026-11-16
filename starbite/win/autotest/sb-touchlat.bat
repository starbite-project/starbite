@echo off
rem sb-touchlat.bat (Track K T84, docs/WIN11-GPU.md 6.109), staged as gputest\sb-user.bat: how late does Windows
rem hand a touch to a mouse-only program (Geometry Dash)? sbtouchlat.exe injects taps, holds and drags into four
rem kinds of window and logs when the touch (WM_POINTERDOWN) and the promoted click (WM_LBUTTONDOWN) arrive.
rem Part 2 repeats it in a GL window on our GL kit with the kit's touch handling off and on (0170).
rem Also: the user's touch settings, the tablet input services, and the touch driver's own registry values.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
cd /d %D%
echo TOUCHLAT logon reached %date% %time% >> %L%
ping -n 31 127.0.0.1 > nul
reg query "HKCU\Software\Microsoft\Wisp\Touch" >> %L% 2>&1
reg query "HKCU\Software\Microsoft\Wisp\Pen\SysEventParameters" >> %L% 2>&1
for %%S in (TabletInputService TextInputManagementService) do sc query %%S >> %L% 2>&1
tasklist /fi "imagename eq wisptis.exe" >> %L% 2>&1
tasklist /fi "imagename eq ctfmon.exe" >> %L% 2>&1
if exist %D%\sbtouchlat.exe (
  %D%\sbtouchlat.exe %L%
  echo TOUCHLAT sbtouchlat exit code !errorlevel! >> %L%
) else (
  echo TOUCHLAT no sbtouchlat.exe staged >> %L%
)
rem part 2: the same touches into a GL window of an x64 program that uses our GL kit (as the game does), with the
rem kit's touch handling (sb-patches/0170) off, then on. On = the click should come with the touch-down.
if exist %D%\sbtouchlat-x64.exe if exist %D%\opengl32-gd.dll (
  if not exist %D%\tk mkdir %D%\tk
  copy /y %D%\sbtouchlat-x64.exe %D%\tk\sbtouchlat-x64.exe > nul
  copy /y %D%\opengl32-gd.dll %D%\tk\opengl32.dll > nul
  copy /y %D%\libgallium_wgl-gd.dll %D%\tk\libgallium_wgl.dll > nul
  set SB_GL_TOUCH=0
  %D%\tk\sbtouchlat-x64.exe %L% gl 2>> %D%\touchlat-gl-err.txt
  echo TOUCHLAT gl run, kit touch off: exit code !errorlevel! >> %L%
  set SB_GL_TOUCH=1
  %D%\tk\sbtouchlat-x64.exe %L% gl 2>> %D%\touchlat-gl-err.txt
  echo TOUCHLAT gl run, kit touch on: exit code !errorlevel! >> %L%
  set SB_GL_TOUCH=
  if exist %D%\touchlat-gl-err.txt type %D%\touchlat-gl-err.txt >> %L%
  findstr /c:"sb gl touch" %U%\sbtouchlat-x64.exe-*.log >> %L% 2>nul
)
reg query HKLM\SYSTEM\CurrentControlSet\Services\S6SY761Touch > %U%\touch-service.txt 2>&1
echo TOUCHLAT end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite TOUCHLAT done"
exit /b 0
