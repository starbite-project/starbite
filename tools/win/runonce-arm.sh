#!/usr/bin/env bash
# runonce-arm.sh NAME SCRIPT WINDIR OUT: from TWRP, push SCRIPT (a .cmd from this repo) to C:\WINDIR\ and set
# HKLM RunOnce NAME = "cmd /c C:\WINDIR\<script>" (runs once, elevated, at the next logon; docs/WIN11-GPU.md 6.87).
# SOFTWARE hive: clean check, backup in OUT/backup, read-back of both pushes. e.g.
#   tools/win/runonce-arm.sh StarbiteXtaM3 starbite/win/debloat/xta-m3-folder.cmd starbite\\xta OUT
set -euo pipefail
cd "$(dirname "$0")/../.."
name=${1:?usage: runonce-arm.sh NAME SCRIPT WINDIR OUT}; src=${2:?}; windir=${3:?}; out=${4:?}
[ -f "$src" ] || { echo "no $src"; exit 1; }
NTFS=/dev/block/sda27; W=/tmp/wwin; SOFT=Windows/System32/config/SOFTWARE
unixdir=$(echo "$windir" | tr '\\' '/'); base=$(basename "$src")
mkdir -p "$out/backup"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/runonce-arm.log"; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { log "the phone is not in TWRP"; exit 1; }
tmp=$(mktemp -d); trap 'adb shell "sync; umount '"$W"'" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
adb pull "$W/$SOFT" "$tmp/SOFTWARE" >/dev/null
python3 tools/win/hive-service.py "$tmp/SOFTWARE" check >> "$out/runonce-arm.log" 2>&1 || { log "SOFTWARE is dirty: gpu-cycle --clean-only first"; exit 3; }
cp "$tmp/SOFTWARE" "$out/backup/SOFTWARE-$name-$(date +%Y%m%d-%H%M%S)"
python3 tools/win/hive-runonce.py "$tmp/SOFTWARE" "$tmp/SW1" set "$name" "cmd /c C:\\$windir\\$base" >> "$out/runonce-arm.log" 2>&1
adb shell "mkdir -p '$W/$unixdir'" >/dev/null
for pair in "$src:$W/$unixdir/$base" "$tmp/SW1:$W/$SOFT"; do
    s=${pair%%:*}; d=${pair#*:}
    adb push "$s" "$d" >/dev/null
    a=$(shasum -a 256 "$s" | cut -c1-64); b=$(adb shell "sha256sum '$d'" | tr -d '\r' | cut -c1-64)
    [ "$a" = "$b" ] || { log "READ-BACK MISMATCH: $d"; exit 4; }
    log "pushed $(basename "$s") -> $d (${a:0:8})"
done
python3 tools/win/hive-runonce.py "$tmp/SW1" show | tee -a "$out/runonce-arm.log"
log "armed: HKLM RunOnce $name -> cmd /c C:\\$windir\\$base"
