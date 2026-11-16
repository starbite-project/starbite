#!/usr/bin/env bash
# sbsel-install.sh: the "Windows 11 (new GPU)" row's driver set on the Windows volume, from TWRP
# (docs/WIN11-GPU.md 6.108). The existing rows keep C:\Windows\System32\sbumd.dll, which this script never writes.
#
#   tools/win/sbsel-install.sh show OUT              read-only: the adapter's driver name, the three files' sha256,
#                                                    the test-log switch
#   tools/win/sbsel-install.sh install STAGE OUT     push STAGE/sbsel.dll and STAGE/sbumd-ng.dll to System32 (read
#                                                    back), THEN set UserModeDriverName = sbsel.dll
#   tools/win/sbsel-install.sh restore OUT           UserModeDriverName = sbumd.dll again (every row loads sbumd.dll
#                                                    directly, as before); the two pushed files stay, unused
#   tools/win/sbsel-install.sh log on|off OUT        the selector's test log (C:\ProgramData\Starbite\sbsellog ->
#                                                    umdlog\sbsel.log, one line per process): on for a test, off after
#
# STAGE = build/win/stage/sbwddm-ng-0169 (sbsel.dll, sbumd-ng.dll, SHA256SUMS; checked before anything is pushed).
# The row itself (catalog key win11-ng, wdt = 80) is a store change and not done here.
# Refuses a dirty SYSTEM hive (exit 3; run gpu-cycle.sh --clean-only first). The hive is backed up in OUT/backup/.
# Every push is read back by sha256 (exit 4). install refuses when System32\sbumd.dll is missing. No boot.
set -euo pipefail
cd "$(dirname "$0")/../.."
usage() { sed -n '2,18p' "$0"; exit 1; }
mode=${1:-}; [ -n "$mode" ] || usage
case $mode in
    show|restore) out=${2:-}; [ -n "$out" ] || usage ;;
    install) stage=${2:-}; out=${3:-}; [ -n "$stage" ] && [ -n "$out" ] || usage ;;
    log) sw=${2:-}; out=${3:-}; case $sw in on|off) ;; *) usage ;; esac; [ -n "$out" ] || usage ;;
    *) usage ;;
esac
NTFS=/dev/block/sda27
W=/tmp/wwin
SYS=Windows/System32/config/SYSTEM
S32=$W/Windows/System32
HS=tools/win/hive-service.py
UN=tools/win/hive-umdname.py
mkdir -p "$out/backup"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/sbsel-install.log"; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { log "the phone is not in TWRP"; exit 1; }
tmp=$(mktemp -d); trap 'adb shell "sync; umount '"$W"'" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
push_check() {
    adb push "$1" "$2" >/dev/null
    local a b
    a=$(shasum -a 256 "$1" | cut -c1-64); b=$(adb shell "sha256sum '$2'" | tr -d '\r' | cut -c1-64)
    [ "$a" = "$b" ] || { log "READ-BACK MISMATCH: $2"; exit 4; }
    log "pushed $(basename "$1") -> $2 (${a:0:8})"
}
files() {
    for f in sbumd.dll sbsel.dll sbumd-ng.dll; do
        h=$(adb shell "[ -f $S32/$f ] && sha256sum $S32/$f" | tr -d '\r' | cut -c1-16)
        log "System32\\$f: ${h:-not there}"
    done
    if adb shell "[ -f $W/ProgramData/Starbite/sbsellog ]"; then log "test log: ON (umdlog\\sbsel.log)"; else log "test log: off"; fi
}
hive_pull() {
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
    python3 "$HS" "$tmp/SYSTEM" check >> "$out/sbsel-install.log" 2>&1 || { log "the SYSTEM hive is dirty: run gpu-cycle.sh --clean-only first"; exit 3; }
}
case $mode in
show)
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
    python3 "$UN" "$tmp/SYSTEM" show 2>&1 | tee -a "$out/sbsel-install.log"
    files
    exit 0 ;;
log)
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
    if [ "$sw" = on ]; then
        adb shell "mkdir -p $W/ProgramData/Starbite/umdlog && echo on > $W/ProgramData/Starbite/sbsellog && rm -f $W/ProgramData/Starbite/umdlog/sbsel.log" >/dev/null
    else
        adb shell "rm -f $W/ProgramData/Starbite/sbsellog" >/dev/null
    fi
    files
    exit 0 ;;
install)
    for f in sbsel.dll sbumd-ng.dll SHA256SUMS; do [ -f "$stage/$f" ] || { log "no $stage/$f"; exit 1; }; done
    (cd "$stage" && grep -E '  (sbsel|sbumd-ng)\.dll$' SHA256SUMS | shasum -a 256 -c - >/dev/null) || { log "$stage does not match its SHA256SUMS"; exit 1; }
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
    adb shell "[ -f $S32/sbumd.dll ]" || { log "System32\\sbumd.dll is missing: the standard driver is not installed; nothing changed"; exit 1; }
    hive_pull
    cp "$tmp/SYSTEM" "$out/backup/SYSTEM-sbsel-$(date +%Y%m%d-%H%M%S)"
    before=$(adb shell "sha256sum $S32/sbumd.dll" | tr -d '\r' | cut -c1-64)
    python3 "$UN" "$tmp/SYSTEM" "$tmp/S1" set sbsel.dll 2>&1 | tee -a "$out/sbsel-install.log"
    [ -f "$tmp/S1" ] || { log "the hive edit failed: nothing pushed"; exit 1; }
    # files first: the hive never names a file that is not there
    push_check "$stage/sbsel.dll" "$S32/sbsel.dll"
    push_check "$stage/sbumd-ng.dll" "$S32/sbumd-ng.dll"
    push_check "$tmp/S1" "$W/$SYS"
    after=$(adb shell "sha256sum $S32/sbumd.dll" | tr -d '\r' | cut -c1-64)
    [ "$before" = "$after" ] || { log "System32\\sbumd.dll CHANGED (${before:0:8} -> ${after:0:8}): this script does not write it; stop and look"; exit 4; }
    files
    log "install done: the adapter's driver name is sbsel.dll; the standard rows' sbumd.dll (${after:0:8}) is untouched"
    exit 0 ;;
restore)
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
    hive_pull
    cp "$tmp/SYSTEM" "$out/backup/SYSTEM-sbsel-restore-$(date +%Y%m%d-%H%M%S)"
    python3 "$UN" "$tmp/SYSTEM" "$tmp/S1" set sbumd.dll 2>&1 | tee -a "$out/sbsel-install.log"
    [ -f "$tmp/S1" ] || { log "the hive edit failed: nothing pushed"; exit 1; }
    push_check "$tmp/S1" "$W/$SYS"
    adb shell "rm -f $W/ProgramData/Starbite/sbsellog" >/dev/null
    files
    log "restore done: every row loads sbumd.dll directly again (sbsel.dll and sbumd-ng.dll stay in System32, unused)"
    exit 0 ;;
esac
