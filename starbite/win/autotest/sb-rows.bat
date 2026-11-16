@echo off
rem sb-rows.bat (Track K T83, docs/WIN11-GPU.md 6.108), staged as gputest\sb-user.bat: which D3D driver did THIS
rem boot's row select? Run once from the row "Windows 11 (new GPU)" (win11-ng) and once from "Windows 11 (big
rem cores)" (win11-big) with the selector installed (tools/win/sbsel-install.sh install + log on):
rem   - the watchdog value sbwdog recorded (the row signal) and the row it means,
rem   - the adapter's UserModeDriverName and the sha256 of sbsel.dll / sbumd.dll / sbumd-ng.dll,
rem   - a windowed blt-model and a flip-model D3D program (sbcomp) with a screen shot each, DWM survived or not,
rem   - which processes of this account have which driver loaded (tasklist /m),
rem   - one Geometry Dash run (sb-gd.bat, ~5 min) with the staged GL kit 0169, no flag files,
rem   - the selector's own log (one line per process, DWM included) and a verdict line.
rem Then the KMD's counters and state, and it restarts. gputest.log missing = the boot never reached the logon.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set S=%SystemRoot%\System32
set K=HKLM\SYSTEM\CurrentControlSet\Services\sbwddm\State
cd /d %D%
echo ROWS logon reached %date% %time% >> %L%
ping -n 21 127.0.0.1 > nul

set WT=none
for /f "tokens=3" %%v in ('reg query HKLM\SYSTEM\CurrentControlSet\Services\sbwdog\State /v WtDat 2^>nul ^| find "REG_DWORD"') do set /a WT=%%v
set ROW=standard
if "!WT!"=="none" (
  echo ROWS WtDat could not be read >> %L%
) else (
  if !WT! geq 63400 if !WT! leq 63560 set ROW=NEW-GPU
)
echo ROWS WtDat !WT! = row !ROW! ^(80 s = 63476: new GPU; 82 s = 65063: standard^) >> %L%
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0000" /v UserModeDriverName >> %L% 2>&1
for %%F in (sbsel.dll sbumd.dll sbumd-ng.dll) do (
  if exist %S%\%%F (
    for /f "skip=1 tokens=*" %%h in ('certutil -hashfile %S%\%%F SHA256 ^| findstr /v /c:"CertUtil"') do echo ROWS file %%F sha256 %%h >> %L%
  ) else (
    echo ROWS file %%F MISSING >> %L%
  )
)
if exist C:\ProgramData\Starbite\sbsellog (echo ROWS selector test log: on >> %L%) else (echo ROWS selector test log: OFF ^(no sbsel-*.log will exist^) >> %L%)
for %%F in (glzc glzcnovsync glzcflip glzcflipd glzcasync ngzcoff) do if exist C:\ProgramData\Starbite\%%F echo ROWS flag file %%F is present >> %L%

call :dwmpid DWM0
echo ROWS dwm pid !DWM0! >> %L%
if exist %D%\sbcomp.exe for %%M in (hwndblt hwnd) do (
  if exist %D%\sbcomp.log del /f %D%\sbcomp.log
  start "sbcomp-%%M" %D%\sbcomp.exe %%M 10 %D%\sbcomp.log
  ping -n 6 127.0.0.1 > nul
  %D%\sbshow.exe snap rows-%%M >> %L% 2>&1
  for %%X in (sbumd.dll sbumd-ng.dll) do (
    echo ROWS [sbcomp %%M running] processes of this account with %%X: >> %L%
    tasklist /m %%X /fo csv /nh >> %L% 2>&1
  )
  ping -n 9 127.0.0.1 > nul
  taskkill /f /im sbcomp.exe > nul 2>&1
  if exist %D%\sbcomp.log type %D%\sbcomp.log >> %L%
)
call :dwmpid DWM1
if "!DWM0!"=="!DWM1!" (echo ROWS DWM SURVIVED, pid !DWM1! >> %L%) else (echo ROWS DWM RESTARTED, pid !DWM0! then !DWM1! >> %L%)

if exist %D%\sb-gd.bat (
  echo ROWS Geometry Dash run on row !ROW! ^(GL kit 0169, no flag files^) >> %L%
  call %D%\sb-gd.bat !ROW!
  findstr /c:"sb gl zc: row" /c:"SB_GL_PRESENT" /c:"present thread" %U%\GeometryDash.exe-*.log >> %L% 2>nul
  if errorlevel 1 echo ROWS the game driver logged no zero-copy lines ^(= the GDI present path^) >> %L%
)

set NG=0
set ST=0
set DW=none
if exist %U%\sbsel-*.log (
  echo ROWS selector lines ^(one file per process^): >> %L%
  type %U%\sbsel-*.log >> %L% 2>nul
  for /f %%c in ('type %U%\sbsel-*.log 2^>nul ^| find /c "-> sbumd-ng.dll"') do set NG=%%c
  for /f %%c in ('type %U%\sbsel-*.log 2^>nul ^| find /c "-> sbumd.dll"') do set ST=%%c
  for /f "tokens=*" %%l in ('type %U%\sbsel-*.log 2^>nul ^| find /i "dwm.exe"') do set DW=%%l
) else (
  echo ROWS no sbsel-*.log >> %L%
)
echo ROWS selector log: !NG! processes loaded sbumd-ng.dll, !ST! loaded sbumd.dll >> %L%
echo ROWS DWM: !DW! >> %L%
rem what is loaded right now in the processes this account can see (catches a process that could not write a line)
set TNG=0
set TST=0
for /f %%c in ('tasklist /m sbumd-ng.dll /fo csv /nh 2^>nul ^| find /c ".exe"') do set TNG=%%c
for /f %%c in ('tasklist /m sbumd.dll /fo csv /nh 2^>nul ^| find /c ".exe"') do set TST=%%c
echo ROWS loaded now: sbumd-ng.dll in !TNG! processes, sbumd.dll in !TST! >> %L%
tasklist /m sbumd.dll /fo csv /nh >> %L% 2>&1
tasklist /m sbumd-ng.dll /fo csv /nh >> %L% 2>&1
set V=FAIL
if "!ROW!"=="NEW-GPU" if !NG! gtr 0 if !ST! equ 0 if !TST! equ 0 set V=PASS
if "!ROW!"=="standard" if !ST! gtr 0 if !NG! equ 0 if !TNG! equ 0 set V=PASS
echo ROWS VERDICT !V!: row !ROW!, selector lines ng !NG! / std !ST!, loaded now ng !TNG! / std !TST! >> %L%

for %%V in (Live_JobsTimedOut Live_GpuResets Live_FaultCount Live_HangCount) do for /f "tokens=3" %%v in ('reg query %K% /v %%V 2^>nul ^| find "REG_DWORD"') do (set /a X=%%v & echo ROWS %%V !X! >> %L%)
reg query %K% > %U%\sbwddm-state.txt 2>&1
reg query HKLM\SYSTEM\CurrentControlSet\Services\sbwdog\State > %U%\sbwdog-state.txt 2>&1
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:16 /f:text > %U%\rows-app-events.txt 2>&1
echo ROWS end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite ROWS done"
exit /b 0

:dwmpid
set %1=none
for /f "tokens=2" %%p in ('tasklist /fi "imagename eq dwm.exe" /nh ^| find /i "dwm.exe"') do set %1=%%p
exit /b 0
