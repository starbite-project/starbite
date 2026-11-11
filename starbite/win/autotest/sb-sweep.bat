@echo off
rem sb-sweep.bat (docs/WIN11-GPU.md 6.46, T37): the sweep. Called by sb-gputest.cmd after its tests (a .bat, so
rem gpu-cycle.sh pushes it to C:\starbite\auto\gpu and not to Startup). Opens a fixed list of surfaces one by one;
rem for each, sbshow "snap NAME" writes a GDI screenshot (umdlog\shot-NAME.ppm, quarter size) and has DWM dump its
rem composed frame (umdlog\dump-dwm-NAME-p<n>.ppm, half size; only with our driver: sb-patches/0130). The same
rem list in a reference boot (gpu-cycle.sh --reference: sbwddm display-only, Windows draws with WARP) gives the
rem shots tools/win/sweep-diff.py compares against. Ends with the event logs and a restart.
rem T39: Widgets, Photos, Edge and Store (what filled C: in T38: 482 -> 97 MiB) run only with >= 1.5 GB free;
rem shots and DWM dumps stop below 600 MiB (sbshow, sb-patches/0134).
set D=C:\starbite\auto\gpu
set U=C:\ProgramData\Starbite\umdlog
set L=%D%\gputest.log
cd /d %D%
echo sweep start %date% %time% >> %L%
sbshow.exe disk >> %L% 2>&1
sbshow.exe mincon >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe snap 00-desktop >> %L% 2>&1
ping -n 3 127.0.0.1 > nul

rem ---- the shell's own surfaces --------------------------------------------------------------------------
sbshow.exe keys win >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe snap 01-start >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
ping -n 2 127.0.0.1 > nul
sbshow.exe keys win+s >> %L% 2>&1
ping -n 5 127.0.0.1 > nul
sbshow.exe snap 02-search >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
ping -n 2 127.0.0.1 > nul
sbshow.exe keys win+a >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe snap 03-quicksettings >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
ping -n 2 127.0.0.1 > nul
sbshow.exe keys win+n >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe snap 04-notifications >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
ping -n 2 127.0.0.1 > nul
sbshow.exe diskok 1500 >> %L% 2>&1
if errorlevel 1 goto skip_widgets
sbshow.exe keys win+w >> %L% 2>&1
ping -n 7 127.0.0.1 > nul
sbshow.exe snap 05-widgets >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
ping -n 2 127.0.0.1 > nul
:skip_widgets
sbshow.exe keys win+tab >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe snap 06-taskview >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
ping -n 2 127.0.0.1 > nul

rem ---- File Explorer (its process traced: sb-patches/0129) ------------------------------------------------
echo on> %U%\trace-explorer.exe
start "" explorer.exe C:\
sbshow.exe max CabinetWClass 30 >> %L% 2>&1
ping -n 6 127.0.0.1 > nul
sbshow.exe list >> %L% 2>&1
sbshow.exe snap 07-explorer-c >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys ctrl+l >> %L% 2>&1
ping -n 2 127.0.0.1 > nul
sbshow.exe type C:\Windows\System32 >> %L% 2>&1
sbshow.exe keys enter >> %L% 2>&1
ping -n 7 127.0.0.1 > nul
sbshow.exe snap 08-explorer-system32 >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe rclick 50 60 >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe snap 09-explorer-context >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
ping -n 2 127.0.0.1 > nul
sbshow.exe alttab 10-alttab >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe close CabinetWClass >> %L% 2>&1
del %U%\trace-explorer.exe
ping -n 3 127.0.0.1 > nul

rem ---- Settings, several pages, and Snap layouts -----------------------------------------------------------
start "" ms-settings:display
sbshow.exe max ApplicationFrameWindow 30 >> %L% 2>&1
ping -n 6 127.0.0.1 > nul
sbshow.exe snap 11-settings-display >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys win+z >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe snap 12-snaplayouts >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe keys esc >> %L% 2>&1
start "" ms-settings:personalization
ping -n 6 127.0.0.1 > nul
sbshow.exe snap 13-settings-personalization >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
start "" ms-settings:colors
ping -n 6 127.0.0.1 > nul
sbshow.exe snap 14-settings-colours >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
start "" ms-settings:about
ping -n 6 127.0.0.1 > nul
sbshow.exe snap 15-settings-about >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
start "" ms-settings:windowsupdate
ping -n 7 127.0.0.1 > nul
sbshow.exe snap 16-settings-update >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im SystemSettings.exe >> %L% 2>&1

rem ---- apps ---------------------------------------------------------------------------------------------
start "" taskmgr.exe
sbshow.exe max TaskManagerWindow 20 >> %L% 2>&1
ping -n 6 127.0.0.1 > nul
sbshow.exe snap 17-taskmanager >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
start "" notepad.exe
sbshow.exe max Notepad 20 >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe type "Starbite sweep: Notepad text 0123456789 The quick brown fox jumps over the lazy dog." >> %L% 2>&1
ping -n 2 127.0.0.1 > nul
sbshow.exe snap 18-notepad >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im notepad.exe >> %L% 2>&1
sbshow.exe image %D%\sweep.bmp >> %L% 2>&1
start "" mspaint.exe %D%\sweep.bmp
sbshow.exe max MSPaintApp 20 >> %L% 2>&1
ping -n 7 127.0.0.1 > nul
sbshow.exe snap 19-paint >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im mspaint.exe >> %L% 2>&1
sbshow.exe diskok 1500 >> %L% 2>&1
if errorlevel 1 goto skip_heavyapps
start "" ms-photos:viewer?fileName=%D%\sweep.bmp
ping -n 11 127.0.0.1 > nul
sbshow.exe list >> %L% 2>&1
sbshow.exe snap 20-photos >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im Photos.exe >> %L% 2>&1
taskkill /f /im Microsoft.Photos.exe >> %L% 2>&1
start "" msedge.exe --no-first-run --no-default-browser-check --new-window file:///C:/starbite/auto/gpu/sweep.html
ping -n 13 127.0.0.1 > nul
sbshow.exe max Chrome_WidgetWin_1 10 >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe snap 21-edge >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im msedge.exe >> %L% 2>&1
start "" ms-windows-store:
ping -n 13 127.0.0.1 > nul
sbshow.exe max ApplicationFrameWindow 10 >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe snap 22-store >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
taskkill /f /im WinStore.App.exe >> %L% 2>&1
:skip_heavyapps
start "" sbshow.exe dialog 12
ping -n 4 127.0.0.1 > nul
sbshow.exe snap 23-dialog >> %L% 2>&1
ping -n 9 127.0.0.1 > nul

rem ---- the touch keyboard: as the user has it, then with transparency effects off (restored after) ----------
start "" "C:\Program Files\Common Files\microsoft shared\ink\TabTip.exe"
ping -n 4 127.0.0.1 > nul
sbshow.exe kbd >> %L% 2>&1
ping -n 6 127.0.0.1 > nul
sbshow.exe list >> %L% 2>&1
sbshow.exe hit >> %L% 2>&1
sbshow.exe snap 24-keyboard >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe transparency 0 >> %L% 2>&1
sbshow.exe kbd >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe kbd >> %L% 2>&1
ping -n 6 127.0.0.1 > nul
sbshow.exe hit >> %L% 2>&1
sbshow.exe snap 25-keyboard-notransparency >> %L% 2>&1
ping -n 4 127.0.0.1 > nul
sbshow.exe transparency restore >> %L% 2>&1
sbshow.exe kbd >> %L% 2>&1
ping -n 3 127.0.0.1 > nul
sbshow.exe snap 26-desktop-end >> %L% 2>&1
ping -n 3 127.0.0.1 > nul

rem ---- what crashed meanwhile: the app and system events, WER, live kernel reports (per surface by time) ----
wevtutil qe Application /q:"*[System[(EventID=1000 or EventID=1001 or EventID=1002)]]" /rd:true /c:80 /f:text > %U%\sweep-app-events.txt 2>&1
wevtutil qe System /q:"*[System[(EventID=4101 or EventID=41 or EventID=6008 or EventID=1001 or EventID=141 or EventID=117)]]" /rd:true /c:40 /f:text > %U%\sweep-sys-events.txt 2>&1
dir /a /o:d /t:w C:\ProgramData\Microsoft\Windows\WER\ReportArchive C:\ProgramData\Microsoft\Windows\WER\ReportQueue > %U%\sweep-wer.txt 2>&1
dir /s /a C:\Windows\LiveKernelReports > %U%\sweep-livekernel.txt 2>&1
sbshow.exe disk >> %L% 2>&1
echo sweep end %date% %time% >> %L%
