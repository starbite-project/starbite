@echo off
rem sb-x86.bat (DRAFT, 2026-11-16, never run): do 32-bit x86 programs get our adapter? Modelled on sb-wt.bat: staged
rem as gputest\sb-user.bat, so sb-gputest.cmd hands over to it at logon INSTEAD of every other test. Needs, in the
rem test folder: sbd3d32.exe, sbwt32.exe, d3d10core_test32.exe (build/win/umd-x86). The driver under test is
rem C:\Windows\SysWOW64\sbumd.dll with UserModeDriverNameWow set (build/win/patchwork/hive-umdname-wow.py).
rem Lines in gputest.log start with X86. A restart at the end, as sb-wt.bat.
setlocal EnableDelayedExpansion
set D=C:\starbite\auto\gpu
set L=%D%\gputest.log
set U=C:\ProgramData\Starbite\umdlog
set C="HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0000"
set K="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\STRB0072\0\Device Parameters\sbwddm"
cd /d %D%
echo X86 start %date% %time% >> %L%
ping -n 21 127.0.0.1 > nul

rem 1. what is installed: both driver names, both files
reg query %C% /v UserModeDriverName >> %L% 2>&1
reg query %C% /v UserModeDriverNameWow >> %L% 2>&1
for %%F in (C:\Windows\System32\sbumd.dll C:\Windows\SysWOW64\sbumd.dll) do (
  if exist %%F (certutil -hashfile %%F SHA256 | findstr /v /c:"hash" /c:"CertUtil" > %D%\x86-hash.txt & set /p H=<%D%\x86-hash.txt & echo X86 file %%F sha256 !H! >> %L%) else echo X86 file %%F MISSING >> %L%
)
rem 2. the value names the graphics kernel itself knows (answers: is it UserModeDriverNameWow on ARM64 Windows?)
powershell -NoProfile -Command "$b=[IO.File]::ReadAllBytes('C:\Windows\System32\drivers\dxgkrnl.sys'); $s=[Text.Encoding]::Unicode.GetString($b); [regex]::Matches($s,'UserModeDriverName[A-Za-z0-9]*') | ForEach-Object { $_.Value } | Sort-Object -Unique | ForEach-Object { 'X86 dxgkrnl knows ' + $_ }" >> %L% 2>&1

rem 3. the 10-second answer: a 32-bit D3D11 program. Exit 0 = on our driver, clear and triangle right;
rem    1 = drew, but on the software renderer (our driver was not loaded); 2 = wrong pixels; 3 = no device
%D%\sbd3d32.exe %D%\sbd3d32.log >> %L% 2>&1
echo X86 sbd3d x86 exit !errorlevel! >> %L%
rem the same program native, if staged, as the control of this boot
if exist %D%\sbd3d.exe (
  %D%\sbd3d.exe %D%\sbd3d.log >> %L% 2>&1
  echo X86 sbd3d arm64 exit !errorlevel! >> %L%
)

rem 4. the kernel driver's own record of 32-bit binds (sbumd x86 sends UmdNote_48 = binds, UmdNote_49 = pid)
reg query %K% /v UmdNote_48 >> %L% 2>&1
reg query %K% /v UmdNote_49 >> %L% 2>&1

rem 5. (sb-x86-full.bat) ALL test functions of Wine's d3d10core as a 32-bit program, one process each, 90 s each.
rem    A test that ran on another adapter is marked [NOT OUR ADAPTER: ...] by sbwt.
if exist %D%\d3d10core_test32.exe (
  echo X86 suite d3d10core x86 start !time! >> %L%
  %D%\sbwt32.exe %D%\d3d10core_test32.exe d3d10core %U%\sbwt-x86-d3d10core.log 90 >> %L% 2>&1
  echo X86 suite d3d10core x86 sbwt exit !errorlevel! at !time! >> %L%
)

rem 6. is the desktop still ours afterwards (DWM alive, the KMD's fault counters), and what crashed
tasklist /fi "imagename eq dwm.exe" /fo csv /nh >> %L% 2>&1
for %%V in (Live_JobsTimedOut Live_GpuResets Live_JobsFailed Live_TdrEngineResets) do for /f "tokens=3" %%v in ('reg query %K% /v %%V 2^>nul ^| find "REG_DWORD"') do (set /a X=%%v & echo X86 %%V !X! >> %L%)
wevtutil qe Application /q:"*[System[(EventID=1000)]]" /rd:true /c:40 /f:text > %U%\x86-app-events.txt 2>&1
echo X86 end %date% %time% >> %L%
ping -n 4 127.0.0.1 > nul
shutdown /r /t 0 /c "Starbite X86 done"
exit /b 0
