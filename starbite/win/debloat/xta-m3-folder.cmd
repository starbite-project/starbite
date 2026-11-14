@echo off
rem xta-m3-folder.cmd (Track K T67, docs/WIN11-GPU.md 6.87): runs ELEVATED from HKLM RunOnce StarbiteXtaM3 at logon.
rem With sbxta's SwapCache mode (Parameters DisableOnM3=0, SwapCache=1) a big-core boot parks the small cores' x64/x86
rem translation cache as C:\Windows\XtaCache.a55 and leaves no C:\Windows\XtaCache, and the XtaCache service then
rem stops with 7023 "file not found" (T64). This makes the empty folder once (access rules copied from the parked
rem folder; if that fails the inherited C:\Windows rules stay: SYSTEM full control), starts the service and logs it.
rem On a small-core boot (no XtaCache.a55) it re-arms itself for the next logon. Log: C:\ProgramData\Starbite\debloat\xta.log
setlocal
set B=C:\ProgramData\Starbite\debloat
set L=%B%\xta.log
if not exist %B% mkdir %B%
echo xta-m3 start %date% %time% >> %L%
set ELEV=0
net session > nul 2>&1 && set ELEV=1
echo elevated %ELEV% >> %L%
if not exist C:\Windows\XtaCache.a55 goto rearm
if exist C:\Windows\XtaCache goto have
mkdir C:\Windows\XtaCache >> %L% 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $src = Get-Acl 'C:\Windows\XtaCache.a55'; $dst = Get-Acl 'C:\Windows\XtaCache'; $dst.SetSecurityDescriptorSddlForm($src.Sddl, 'Access'); Set-Acl 'C:\Windows\XtaCache' $dst -ErrorAction Stop; 'access rules copied: ' + (Get-Acl 'C:\Windows\XtaCache').Sddl } catch { 'access rules NOT copied (inherited kept): ' + $_ }" >> %L% 2>&1
:have
sc start XtaCache >> %L% 2>&1
ping -n 6 127.0.0.1 > nul
sc query XtaCache >> %L% 2>&1
dir /a C:\Windows\XtaCache >> %L% 2>&1
echo xta-m3 done %date% %time% >> %L%
exit /b 0
:rearm
echo no XtaCache.a55 (not a big-core boot in SwapCache mode): re-armed for the next logon >> %L%
reg add HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce /v StarbiteXtaM3 /t REG_SZ /d "cmd /c C:\starbite\xta\xta-m3-folder.cmd" /f >> %L% 2>&1
exit /b 0
