@echo off
rem sb-glsys.bat (DRAFT, never run; build/win/patchwork/glsys-report.md), meant to be staged as gputest\sb-user.bat
rem like sb-rows.bat: is the Starbite OpenGL ICD what ordinary programs get from the SYSTEM opengl32.dll?
rem Needs in %D%: sbgltest-arm64.exe, sbgltest-arm64ec.exe, sbgltest-x64.exe and NO opengl32.dll / libgallium_wgl.dll.
rem Needs installed: System32\sbglicd.dll + sbgl-arm64.dll + sbgl-x64.dll, the adapter's OpenGLDriverName
rem (hive-opengl-icd.py set), the folder C:\ProgramData\Starbite\glsys with the file "log", and the umdlog folder.
rem   1 what is installed: the registry values, the three files' sha256, the switch files;
rem   2 the driver in a native, an ARM64EC and an x64 program (sbgltest hw): renderer, version, frames read back;
rem   3 the ways back: one program switched off by file, and a process that cannot reach the Mali, must both get
rem     Microsoft's OpenGL (sbgltest ms) and not crash;
rem   4 DWM is the same process as before; the ICD front's log lines; a verdict line.
rem Then it restarts (as the other gputest scripts do). gputest.log missing = the boot never reached the logon.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set G=C:\ProgramData\Starbite\glsys
set S=%SystemRoot%\System32
cd /d %D%
echo GLSYS logon reached %date% %time% >> %L%
ping -n 21 127.0.0.1 > nul

rem ---- 1: what is installed
for %%V in (OpenGLDriverName OpenGLVersion OpenGLFlags OpenGLDriverNameWow UserModeDriverName) do (
  reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0000" /v %%V >> %L% 2>&1
)
for %%F in (sbglicd.dll sbgl-arm64.dll sbgl-x64.dll) do (
  if exist %S%\%%F (
    for /f "skip=1 tokens=*" %%h in ('certutil -hashfile %S%\%%F SHA256 ^| findstr /v /c:"CertUtil"') do echo GLSYS file %%F sha256 %%h >> %L%
  ) else (
    echo GLSYS file %%F MISSING >> %L%
  )
)
if exist %G%\ (dir /b %G% > %D%\glsys-flags.txt 2>nul & for /f "tokens=*" %%f in (%D%\glsys-flags.txt) do echo GLSYS switch file glsys\%%f >> %L%) else (echo GLSYS no folder %G%: no switch files, no ICD front log >> %L%)
for %%F in (opengl32.dll libgallium_wgl.dll) do if exist %D%\%%F echo GLSYS WARNING %D%\%%F exists: the tests would use the app-local copy >> %L%
call :dwmpid DWM0
echo GLSYS dwm pid !DWM0! >> %L%

rem ---- 2: the hardware driver in the three kinds of process
set BAD=0
for %%A in (arm64 arm64ec x64) do (
  if exist %D%\sbgltest-%%A.exe (
    %D%\sbgltest-%%A.exe hw %D%\sbgltest-%%A.log 300 > nul 2>&1
    set RC=!errorlevel!
    echo GLSYS sbgltest-%%A hw exit !RC! >> %L%
    if not "!RC!"=="0" set /a BAD+=1
    type %D%\sbgltest-%%A.log >> %L% 2>nul
  ) else (
    echo GLSYS sbgltest-%%A.exe MISSING >> %L%
    set /a BAD+=1
  )
)

rem ---- 3a: one program switched off by file: Microsoft's OpenGL, the driver DLL not loaded
if exist %G%\ (
  type nul > %G%\off-sbgltest-arm64.exe
  %D%\sbgltest-arm64.exe ms %D%\sbgltest-off.log 30 > nul 2>&1
  set RC=!errorlevel!
  echo GLSYS switched off by glsys\off-sbgltest-arm64.exe: sbgltest ms exit !RC! >> %L%
  if not "!RC!"=="0" set /a BAD+=1
  type %D%\sbgltest-off.log >> %L% 2>nul
  del /f %G%\off-sbgltest-arm64.exe
)
rem ---- 3b: a process that cannot reach the Mali (the shim's own switch): no ICD pixel formats, Microsoft's OpenGL
set MALIKM_WDDM=0
for %%A in (arm64 x64) do if exist %D%\sbgltest-%%A.exe (
  %D%\sbgltest-%%A.exe ms %D%\sbgltest-nogpu-%%A.log 30 > nul 2>&1
  set RC=!errorlevel!
  echo GLSYS no Mali in the process ^(MALIKM_WDDM=0^): sbgltest-%%A ms exit !RC! >> %L%
  if not "!RC!"=="0" set /a BAD+=1
  type %D%\sbgltest-nogpu-%%A.log >> %L% 2>nul
)
set MALIKM_WDDM=

rem ---- 4: DWM, the front's log, the verdict
call :dwmpid DWM1
if "!DWM0!"=="!DWM1!" (echo GLSYS DWM SURVIVED, pid !DWM1! >> %L%) else (echo GLSYS DWM RESTARTED, pid !DWM0! then !DWM1! >> %L% & set /a BAD+=1)
if exist %U%\sbglicd-*.log (
  echo GLSYS the ICD front's lines, one per process that asked for OpenGL: >> %L%
  type %U%\sbglicd-*.log >> %L% 2>nul
) else (
  echo GLSYS no sbglicd-*.log: the front was never loaded, or glsys\log / the umdlog folder is missing >> %L%
)
findstr /c:"sbumd: gl:" /c:"gl present:" /c:"glstats" %U%\sbgltest-*.log >> %L% 2>nul
if "!BAD!"=="0" (echo GLSYS VERDICT: PASS >> %L%) else (echo GLSYS VERDICT: FAIL, !BAD! steps >> %L%)
echo GLSYS done %date% %time% >> %L%
shutdown /r /t 5 /f
goto :eof

:dwmpid
set %1=none
for /f "tokens=2 delims=," %%p in ('tasklist /fi "imagename eq dwm.exe" /fo csv /nh 2^>nul') do set %1=%%~p
goto :eof
