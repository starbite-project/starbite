@echo off
rem sb-gd.bat (Track K, 2026-11-12): Geometry Dash diagnostic. Called by sb-gputest.cmd INSTEAD of the sweep when
rem staged (and the GD1 step is skipped then). T48's GD1 gave the game only 40 s after Steam took 180 s to start it,
rem then killed it while it was still loading. Here: up to 360 s for GeometryDash.exe, then 300 s of watching, a row
rem every 30 s (alive? working set, commit free, a screenshot), so a crash shows as the process vanishing on its own.
rem Our x64 GL driver goes into the game folder as in GD1 (ours only, marked, removed after unless the user's kit had
rem put it there). The game's driver log (exceptions included) is umdlog\GeometryDash.exe-*.log.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set G=C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash
cd /d %D%
echo GD2 start %date% %time% >> %L%
if not exist "%G%\GeometryDash.exe" (echo GD2: Geometry Dash not installed >> %L% & exit /b 0)
if not exist %D%\opengl32-gd.dll (echo GD2: no opengl32-gd.dll staged >> %L% & exit /b 0)
set GDKIT=0
if exist "%G%\opengl32.dll.starbite" set GDKIT=1
if exist "%G%\opengl32.dll" if "%GDKIT%"=="0" (echo GD2 skipped: a foreign opengl32.dll is in the game folder >> %L% & exit /b 0)
copy /y %D%\opengl32-gd.dll "%G%\opengl32.dll" > nul
copy /y %D%\libgallium_wgl-gd.dll "%G%\libgallium_wgl.dll" > nul
echo starbite > "%G%\opengl32.dll.starbite"
sbshow.exe disk >> %L% 2>&1
start "" steam://rungameid/322170
set /a W=0
:wait
ping -n 6 127.0.0.1 > nul
set /a W+=5
tasklist /fi "imagename eq GeometryDash.exe" | find /i "GeometryDash.exe" > nul && goto up
if !W! lss 360 goto wait
echo GD2 GeometryDash.exe never appeared in 360 s >> %L%
goto finish
:up
echo GD2 GeometryDash.exe up after !W! s >> %L%
for /l %%T in (1,1,10) do (
  ping -n 31 127.0.0.1 > nul
  set ALIVE=gone
  for /f "tokens=5" %%m in ('tasklist /fi "imagename eq GeometryDash.exe" /nh ^| find /i "GeometryDash"') do set ALIVE=%%m K
  echo GD2 t+%%T x30s: !ALIVE! >> %L%
  sbshow.exe disk >> %L% 2>&1
  if not "!ALIVE!"=="gone" sbshow.exe snap 28-gd-%%T >> %L% 2>&1
  if "!ALIVE!"=="gone" goto gone
)
echo GD2 still running after 300 s: closing it >> %L%
taskkill /f /im GeometryDash.exe >> %L% 2>&1
goto finish
:gone
echo GD2 GeometryDash.exe EXITED BY ITSELF >> %L%
:finish
ping -n 4 127.0.0.1 > nul
if "%GDKIT%"=="0" del /f "%G%\opengl32.dll" "%G%\libgallium_wgl.dll" "%G%\opengl32.dll.starbite"
wevtutil qe Application /q:"*[System[(EventID=1000 or EventID=1001 or EventID=1002 or EventID=1026)]]" /rd:true /c:30 /f:text > C:\ProgramData\Starbite\umdlog\gd-app-events.txt 2>&1
wevtutil qe System /q:"*[System[(EventID=4101 or EventID=2004 or EventID=2019 or EventID=2020)]]" /rd:true /c:30 /f:text > C:\ProgramData\Starbite\umdlog\gd-sys-events.txt 2>&1
echo GD2 end %date% %time% >> %L%
exit /b 0
