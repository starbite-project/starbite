#!/usr/bin/env bash
# debloat.sh: the user's Windows lighter on the big cores (docs/WIN11-GPU.md 6.77), from TWRP, no boot.
#
#   tools/win/debloat.sh apply OUT     (1) sbxta v2 (build/win/debloat-1/sbxta.sys) replaces sbxta v1, with the list
#                                          below in Services\sbxta\Parameters\DisableOnM3Services: those services get
#                                          Start=4 on big-core boots only (originals saved, put back on every
#                                          small-core boot; starbite/win/sbxta/README-services.md);
#                                      (2) HKLM RunOnce StarbiteDebloat -> C:\starbite\debloat\debloat-online.cmd at
#                                          the next logon: Steam / OneDrive* removed from the per-user autostart
#                                          (backed up to .reg first), a Defender exclusion for the Steam folder;
#                                      (3) Geometry Dash's GL driver = the ARM64EC kit (build/win/gd-kit-ec-0158).
#   tools/win/debloat.sh undo OUT      the list removed (the next SMALL-core boot puts every saved service back),
#                                      HKLM RunOnce StarbiteUndebloat -> undebloat-online.cmd (autostart values
#                                      re-imported, the Defender exclusion removed), GD back to the x64 kit.
#                                      sbxta v2 stays: with no list it does what v1 did (plus the restore).
#                                      To put the v1 file back as well, after that small-core boot:
#                                      tools/win/debloat.sh v1 OUT
#   tools/win/debloat.sh show OUT      read-only: sbxta state/list/saved values, RunOnce, the debloat log, GD kit.
#
# Refuses a dirty SYSTEM or SOFTWARE hive (exit 3). Backups of both hives and of the replaced sbxta.sys in
# OUT/backup/. Every pushed file is read back by sha256 (exit 4 on a mismatch). The phone stays in TWRP.
set -euo pipefail
cd "$(dirname "$0")/../.."
main=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
mode=${1:?usage: debloat.sh apply|undo|v1|show OUT}
out=${2:?usage: debloat.sh apply|undo|v1|show OUT}
LIST="WpnService WpnUserService PushToInstall DiagTrack OneSyncSvc CDPUserSvc UsoSvc wuauserv"
V2=$main/build/win/debloat-1/sbxta.sys
V1=$main/build/win/sbxta/sbxta.sys
KIT_EC=$main/build/win/gd-kit-ec-0158
KIT_X64=$main/build/win/stage/sbwddm-t61/gputest
NTFS=/dev/block/sda27
W=/tmp/wwin
SYS=Windows/System32/config/SYSTEM
SOFT=Windows/System32/config/SOFTWARE
HS=tools/win/hive-service.py
SX=tools/woa/win-m3/hive-sbxta-services.py
RO=tools/win/hive-runonce.py
mkdir -p "$out/backup"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/debloat.log"; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { log "the phone is not in TWRP"; exit 1; }
tmp=$(mktemp -d); trap 'adb shell "sync; umount '"$W"'" >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
push_check() {
    adb push "$1" "$2" >/dev/null
    local a b
    a=$(shasum -a 256 "$1" | cut -c1-64); b=$(adb shell "sha256sum '$2'" | tr -d '\r' | cut -c1-64)
    [ "$a" = "$b" ] || { log "READ-BACK MISMATCH: $2"; exit 4; }
    log "pushed $(basename "$1") -> $2 (${a:0:8})"
}
case $mode in
show)
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null; adb pull "$W/$SOFT" "$tmp/SOFTWARE" >/dev/null
    python3 "$SX" "$tmp/SYSTEM" show
    python3 "$RO" "$tmp/SOFTWARE" show
    adb shell "sha256sum $W/Windows/System32/drivers/sbxta.sys; cat $W/ProgramData/Starbite/debloat/debloat.log 2>/dev/null" | tr -d '\r'
    exit 0 ;;
apply|undo|v1) ;;
*) echo "usage: debloat.sh apply|undo|v1|show OUT"; exit 1 ;;
esac
for f in "$V2" "$V1" starbite/win/debloat/debloat-online.cmd starbite/win/debloat/undebloat-online.cmd; do
    [ -f "$f" ] || { log "missing $f"; exit 1; }
done
adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
adb pull "$W/$SOFT" "$tmp/SOFTWARE" >/dev/null
for h in SYSTEM SOFTWARE; do
    if ! python3 "$HS" "$tmp/$h" check >> "$out/debloat.log" 2>&1; then
        log "the $h hive is dirty (the last Windows boot did not end cleanly): run gpu-cycle.sh --clean-only first"; exit 3
    fi
done
stamp=$(date +%Y%m%d-%H%M%S)
cp "$tmp/SYSTEM" "$out/backup/SYSTEM-$stamp"; cp "$tmp/SOFTWARE" "$out/backup/SOFTWARE-$stamp"
adb pull "$W/Windows/System32/drivers/sbxta.sys" "$out/backup/sbxta-$stamp.sys" >/dev/null
log "$mode: hives clean, backed up as *-$stamp; sbxta.sys now $(shasum -a 256 "$out/backup/sbxta-$stamp.sys" | cut -c1-8)"
python3 tools/woa/hive.py "$tmp/SYSTEM" 'ControlSet001\Services\sbxta' 2>/dev/null | grep -q Start || { log "sbxta is not installed: refusing"; exit 1; }
case $mode in
apply)
    # shellcheck disable=SC2086
    python3 "$SX" "$tmp/SYSTEM" "$tmp/S1" set $LIST >> "$out/debloat.log" 2>&1
    python3 "$HS" "$tmp/S1" "$tmp/S2" param sbxta DisableOnM3=1 SwapCache=0 >> "$out/debloat.log" 2>&1
    python3 "$RO" "$tmp/SOFTWARE" "$tmp/SW1" set StarbiteDebloat 'cmd /c C:\starbite\debloat\debloat-online.cmd' >> "$out/debloat.log" 2>&1
    adb shell "mkdir -p $W/starbite/debloat" >/dev/null
    push_check starbite/win/debloat/debloat-online.cmd "$W/starbite/debloat/debloat-online.cmd"
    push_check starbite/win/debloat/undebloat-online.cmd "$W/starbite/debloat/undebloat-online.cmd"
    push_check "$V2" "$W/Windows/System32/drivers/sbxta.sys"
    push_check "$tmp/S2" "$W/$SYS"
    push_check "$tmp/SW1" "$W/$SOFT"
    python3 "$SX" "$tmp/S2" show | tee -a "$out/debloat.log"
    adb shell "sync; umount $W" >/dev/null 2>&1 || true
    tools/win/gd-kit-push.sh "$KIT_EC" 2>&1 | tee -a "$out/debloat.log"
    log "apply done: big-core service list set, sbxta v2 installed, RunOnce StarbiteDebloat armed, GD = ARM64EC kit" ;;
undo)
    python3 "$SX" "$tmp/SYSTEM" "$tmp/S1" off >> "$out/debloat.log" 2>&1
    python3 "$RO" "$tmp/SOFTWARE" "$tmp/SW0" remove StarbiteDebloat >> "$out/debloat.log" 2>&1 || cp "$tmp/SOFTWARE" "$tmp/SW0"
    python3 "$RO" "$tmp/SW0" "$tmp/SW1" set StarbiteUndebloat 'cmd /c C:\starbite\debloat\undebloat-online.cmd' >> "$out/debloat.log" 2>&1
    adb shell "mkdir -p $W/starbite/debloat" >/dev/null
    push_check starbite/win/debloat/undebloat-online.cmd "$W/starbite/debloat/undebloat-online.cmd"
    push_check "$tmp/S1" "$W/$SYS"
    push_check "$tmp/SW1" "$W/$SOFT"
    python3 "$SX" "$tmp/S1" show | tee -a "$out/debloat.log"
    adb shell "sync; umount $W" >/dev/null 2>&1 || true
    tools/win/gd-kit-push.sh "$KIT_X64" 2>&1 | tee -a "$out/debloat.log"
    log "undo done: list removed (boot the SMALL-core row once to restore the saved services), RunOnce StarbiteUndebloat armed, GD = x64 kit" ;;
v1)
    if python3 "$SX" "$tmp/SYSTEM" show | grep -q -E '^  Saved\\'; then
        log "sbxta still holds saved services: boot the small-core row once first"; exit 1
    fi
    push_check "$V1" "$W/Windows/System32/drivers/sbxta.sys"
    log "v1 sbxta.sys back" ;;
esac
