#!/usr/bin/env bash
# One unattended Windows test cycle, started with the phone in TWRP:
#   tools/win/autotest-cycle.sh OUTDIR [--wifi-sys FILE] [--sblinkd FILE] [--key win11-usb]
# installs the optional driver/sender, arms the one-shot autotest (starbite/win/autotest), boots Windows by catalog key,
# waits for Windows to restart itself into the loader menu (sb-autotest.cmd restarts cleanly after the test, with a
# 10-minute dead man), takes the phone back to TWRP and pulls the logs: run.log, sbwificx.txt, sblinkd logs, the
# WLAN-AutoConfig event log. Exit 2 if the phone does not come back (then it needs the user).
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:?usage: autotest-cycle.sh OUTDIR [--wifi-sys F] [--sblinkd F] [--key K]}; shift
wsys= ; lnk= ; key=win11-usb
while [ $# -gt 0 ]; do case $1 in --wifi-sys) wsys=$2; shift 2;; --sblinkd) lnk=$2; shift 2;; --key) key=$2; shift 2;; *) echo "bad arg $1"; exit 1;; esac; done
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
mkdir -p "$out"
DS=/tmp/wwin/Windows/System32/DriverStore/FileRepository/sbwificx.inf_arm64_54945f5add3c8c52
SU="/tmp/wwin/Users/starbite/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Startup"
adb shell 'mkdir -p /tmp/wwin; umount /tmp/wwin 2>/dev/null; mount.ntfs -o rw,inherit /dev/block/sda27 /tmp/wwin' >/dev/null
adb shell 'mkdir -p /tmp/wwin/starbite/auto; rm -f /tmp/wwin/starbite/auto/run.log'
adb push starbite/win/autotest/autotest.ps1 /tmp/wwin/starbite/auto/autotest.ps1 >/dev/null
adb push starbite/win/autotest/sb-autotest.cmd "$SU/sb-autotest.cmd" >/dev/null
if [ -n "$wsys" ]; then adb push "$wsys" "$DS/sbwificx.sys" >/dev/null; adb push "$wsys" /tmp/wwin/starbite/wifi/sbwificx/sbwificx.sys >/dev/null; fi
if [ -n "$lnk" ]; then adb push "$lnk" /tmp/wwin/starbite/link/sblinkd.exe >/dev/null; adb push "$lnk" "$SU/sblinkd.exe" >/dev/null; fi
adb shell 'echo on > /tmp/wwin/starbite/auto/enabled; sync; umount /tmp/wwin; mount.ntfs -o ro /dev/block/sda27 /tmp/wwin' >/dev/null
echo "installed: wifi $(adb shell "sha256sum $DS/sbwificx.sys" | cut -c1-8), sblinkd $(adb shell 'sha256sum /tmp/wwin/starbite/link/sblinkd.exe' | cut -c1-8), flag $(adb shell 'ls /tmp/wwin/starbite/auto/enabled' | tr -d '\r')"
adb shell umount /tmp/wwin
start=$(date +%s)
timeout 25 tools/store/boot-next.sh "$key" 5 >/dev/null 2>&1 || true
echo "booted $key at $(date +%H:%M:%S); waiting for the menu"
sleep 90
until fastboot devices 2>/dev/null | grep -q fastboot; do
    [ $(( $(date +%s) - start )) -gt 1080 ] && { echo "NO MENU after 18 min: phone stuck (needs the user)"; exit 2; }
    sleep 5
done
echo "menu after $(( $(date +%s) - start )) s"
fastboot oem log dmesg >/dev/null 2>&1 || true; fastboot get_staged "$out/zone0.bin" >/dev/null 2>&1 || true
python3 tools/store/sbfb.py reboot recovery >/dev/null 2>&1
for i in $(seq 1 60); do [ "$(adb get-state 2>/dev/null)" = recovery ] && break; sleep 3; done
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "no TWRP"; exit 2; }
adb shell 'mkdir -p /tmp/wwin; mount.ntfs -o ro /dev/block/sda27 /tmp/wwin' >/dev/null
for f in starbite/auto/run.log starbite/sbwificx.txt starbite/link/sblinkd.log starbite/link/sblinkd-service.log "Windows/System32/winevt/Logs/Microsoft-Windows-WLAN-AutoConfig%4Operational.evtx"; do
    adb pull "/tmp/wwin/$f" "$out/" >/dev/null 2>&1 || echo "missing $f"
done
adb shell 'ls /tmp/wwin/starbite/auto/' | tr -d '\r' | sed 's/^/auto: /'
adb shell umount /tmp/wwin
n=$(grep -a -n "SbWifiCx build" "$out/sbwificx.txt" | tail -1 | cut -d: -f1); tail -n +$((n-1)) "$out/sbwificx.txt" > "$out/wifi-boot.txt"
echo "--- run.log"; cat "$out/run.log" 2>/dev/null | tr -d '\r'
