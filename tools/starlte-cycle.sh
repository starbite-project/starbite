#!/usr/bin/env bash
# Unattended test boot: wait for TWRP (adb), flash IMG to BOOT (the phone
# reboots; with starauto=N in the boot-args the loader starts iOS by itself
# and with sweetreboot=N the kernel resets into recovery N seconds later),
# wait for the phone to come back to TWRP, pull the persistent log to LOG.
#   tools/starlte-cycle.sh out/xnu-boot-X.img build/X-log.txt
set -euo pipefail
cd "$(dirname "$0")/.."
img=$1 log=$2
state() { adb get-state 2>/dev/null || true; }
# adb sometimes stops seeing TWRP after the phone went away; restart it now and then.
wait_recovery() {
    local i=0
    until [ "$(state)" = recovery ]; do
        sleep 3; i=$((i + 1))
        if [ $((i % 20)) = 0 ]; then adb kill-server >/dev/null 2>&1; adb start-server >/dev/null 2>&1; fi
    done
}
echo "$(date +%H:%M:%S) waiting for TWRP"
wait_recovery
sleep 2
tools/twrp-flash-boot.sh "$img"
echo "$(date +%H:%M:%S) flashed; waiting for the phone to leave TWRP"
for i in $(seq 1 200); do [ "$(state)" = recovery ] || break; sleep 3; done
[ "$(state)" = recovery ] && { echo "phone did not reboot"; exit 1; }
echo "$(date +%H:%M:%S) booting; waiting for TWRP"
wait_recovery
sleep 8
for try in 1 2 3; do python3 tools/starlte-log-pull.py adb "$log" | tail -1 && break; sleep 10; done
