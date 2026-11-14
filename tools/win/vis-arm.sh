#!/usr/bin/env bash
# vis-arm.sh OUT: arm the elevated VIS2 run for the NEXT Windows logon (T66, docs/WIN11-GPU.md 6.85), from TWRP.
# Pushes starbite/win/autotest/sb-vis-elev.cmd to C:\starbite\vis\ and sets HKLM RunOnce StarbiteVis to it (SOFTWARE
# hive: clean check, backup in OUT/backup, read-back). Then run the T66 gpu-cycle (its gputest has sb-comp.bat,
# sbcomp.exe and a waiter sb-user.bat). The RunOnce is consumed by the test boot's logon.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:?usage: vis-arm.sh OUT}
NTFS=/dev/block/sda27; W=/tmp/wwin; SOFT=Windows/System32/config/SOFTWARE
mkdir -p "$out/backup"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/vis-arm.log"; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { log "the phone is not in TWRP"; exit 1; }
tmp=$(mktemp -d); trap 'adb shell "sync; umount '"$W"'" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
adb pull "$W/$SOFT" "$tmp/SOFTWARE" >/dev/null
python3 tools/win/hive-service.py "$tmp/SOFTWARE" check >> "$out/vis-arm.log" 2>&1 || { log "SOFTWARE is dirty: gpu-cycle --clean-only first"; exit 3; }
cp "$tmp/SOFTWARE" "$out/backup/SOFTWARE-vis-$(date +%Y%m%d-%H%M%S)"
python3 tools/win/hive-runonce.py "$tmp/SOFTWARE" "$tmp/SW1" set StarbiteVis 'cmd /c C:\starbite\vis\sb-vis-elev.cmd' >> "$out/vis-arm.log" 2>&1
adb shell "mkdir -p $W/starbite/vis" >/dev/null
for pair in "starbite/win/autotest/sb-vis-elev.cmd:$W/starbite/vis/sb-vis-elev.cmd" "$tmp/SW1:$W/$SOFT"; do
    src=${pair%%:*}; dst=${pair#*:}
    adb push "$src" "$dst" >/dev/null
    a=$(shasum -a 256 "$src" | cut -c1-64); b=$(adb shell "sha256sum '$dst'" | tr -d '\r' | cut -c1-64)
    [ "$a" = "$b" ] || { log "READ-BACK MISMATCH: $dst"; exit 4; }
    log "pushed $(basename "$src") -> $dst (${a:0:8})"
done
log "armed: HKLM RunOnce StarbiteVis -> C:\\starbite\\vis\\sb-vis-elev.cmd (next logon, elevated)"
