#!/usr/bin/env bash
# sbextra-install.sh: two optional additions to the Starbite display driver on the Windows volume, from TWRP.
# Neither touches System32\sbumd.dll, the kernel drivers or UserModeDriverName.
#
#   x86: the 32-bit build of the D3D driver (docs/WIN11-GPU-X86.md). 32-bit programs get no GPU without it.
#     tools/win/sbextra-install.sh x86 install STAGE OUT   STAGE/sbumd32.dll -> SysWOW64\sbumd.dll (read back), THEN
#                                                          UserModeDriverNameWow = sbumd.dll
#     tools/win/sbextra-install.sh x86 clear OUT           the value removed (32-bit programs on Windows' software
#                                                          renderer again); the file stays, unused
#   gl: our OpenGL driver for every program (docs/WIN11-GPU-GLSYS.md), through the system opengl32.dll.
#     tools/win/sbextra-install.sh gl install STAGE OUT [EXE...]
#                                                          STAGE/sbglicd.dll, sbgl-arm64.dll, sbgl-x64.dll -> System32
#                                                          (read back), THEN OpenGLDriverName/Version/Flags.
#                                                          With EXE names: allow-list mode, ONLY those programs get
#                                                          the driver (C:\ProgramData\Starbite\glsys\only + on-<exe>);
#                                                          the per-process test log is switched on (glsys\log).
#     tools/win/sbextra-install.sh gl clear OUT            the three values removed, the switch files removed (every
#                                                          program on Microsoft's OpenGL again); the DLLs stay, unused
#   tools/win/sbextra-install.sh show OUT                  read-only: both registrations, the files' sha256, switches
#
# STAGE = build/win/stage/sbwddm-x86-0172 or build/win/stage/sbwddm-glsys-0171 (files + SHA256SUMS, checked first).
# Refuses a dirty SYSTEM hive (exit 3). The hive is backed up in OUT/backup/. Every push is read back (exit 4).
set -euo pipefail
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
cd "$(dirname "$0")/../.."
usage() { sed -n '2,22p' "$self"; exit 1; }
what=${1:-}; [ -n "$what" ] || usage
stage=; out=; exes=()
case $what in
    show) out=${2:-}; mode=show ;;
    x86|gl)
        mode=${2:-}
        case $mode in
            install) stage=${3:-}; out=${4:-}; [ -n "$stage" ] || usage; if [ $# -gt 4 ]; then shift 4; exes=("$@"); fi ;;
            clear) out=${3:-} ;;
            *) usage ;;
        esac ;;
    *) usage ;;
esac
[ -n "$out" ] || usage
for e in "${exes[@]:-}"; do case $e in ''|*[!A-Za-z0-9._-]*) [ -z "$e" ] || { echo "bad program name $e"; exit 1; } ;; esac; done
NTFS=/dev/block/sda27
W=/tmp/wwin
SYS=Windows/System32/config/SYSTEM
S32=$W/Windows/System32
S64=$W/Windows/SysWOW64
G=$W/ProgramData/Starbite/glsys
HS=tools/win/hive-service.py
mkdir -p "$out/backup"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$out/sbextra-install.log"; }
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
    local f h
    for f in "$S32/sbumd.dll" "$S64/sbumd.dll" "$S32/sbglicd.dll" "$S32/sbgl-arm64.dll" "$S32/sbgl-x64.dll"; do
        h=$(adb shell "[ -f $f ] && sha256sum $f || true" | tr -d '\r' | cut -c1-16)
        log "${f#$W/}: ${h:-not there}"
    done
    log "glsys switch files: $(adb shell "ls $G 2>/dev/null || true" | tr -d '\r' | tr '\n' ' ')"
}
mount_rw() { adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null; }
hive_pull() {
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
    python3 "$HS" "$tmp/SYSTEM" check >> "$out/sbextra-install.log" 2>&1 || { log "the SYSTEM hive is dirty: run gpu-cycle.sh --clean-only first"; exit 3; }
    cp "$tmp/SYSTEM" "$out/backup/SYSTEM-$what-$mode-$(date +%Y%m%d-%H%M%S)"
}
stage_ok() {   # the named files exist and match STAGE/SHA256SUMS
    local f
    [ -f "$stage/SHA256SUMS" ] || { log "no $stage/SHA256SUMS"; exit 1; }
    for f in "$@"; do
        [ -f "$stage/$f" ] || { log "no $stage/$f"; exit 1; }
        (cd "$stage" && grep -E "  $f\$" SHA256SUMS | shasum -a 256 -c - >/dev/null 2>&1) || { log "$stage/$f does not match SHA256SUMS"; exit 1; }
    done
}
if [ "$mode" = show ]; then
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null
    adb pull "$W/$SYS" "$tmp/SYSTEM" >/dev/null
    python3 tools/win/hive-umdname-wow.py "$tmp/SYSTEM" show 2>&1 | tee -a "$out/sbextra-install.log"
    python3 tools/win/hive-opengl-icd.py "$tmp/SYSTEM" show 2>&1 | tee -a "$out/sbextra-install.log"
    files
    exit 0
fi
case $what-$mode in
x86-install)
    stage_ok sbumd32.dll
    mount_rw
    adb shell "[ -d $S64 ]" || { log "no SysWOW64 on this Windows: nothing changed"; exit 1; }
    hive_pull
    python3 tools/win/hive-umdname-wow.py "$tmp/SYSTEM" "$tmp/S1" set sbumd.dll 2>&1 | tee -a "$out/sbextra-install.log"
    [ -f "$tmp/S1" ] || { log "the hive edit failed: nothing pushed"; exit 1; }
    push_check "$stage/sbumd32.dll" "$S64/sbumd.dll"
    push_check "$tmp/S1" "$W/$SYS"
    files
    log "x86 install done: 32-bit programs now load SysWOW64\\sbumd.dll as their D3D10/11 driver" ;;
x86-clear)
    mount_rw
    hive_pull
    python3 tools/win/hive-umdname-wow.py "$tmp/SYSTEM" "$tmp/S1" clear 2>&1 | tee -a "$out/sbextra-install.log"
    [ -f "$tmp/S1" ] || { log "the hive edit failed: nothing pushed"; exit 1; }
    push_check "$tmp/S1" "$W/$SYS"
    files
    log "x86 clear done: 32-bit programs are on Windows' software renderer again (the file stays in SysWOW64, unused)" ;;
gl-install)
    stage_ok sbglicd.dll sbgl-arm64.dll sbgl-x64.dll
    mount_rw
    hive_pull
    python3 tools/win/hive-opengl-icd.py "$tmp/SYSTEM" "$tmp/S1" set 2>&1 | tee -a "$out/sbextra-install.log"
    [ -f "$tmp/S1" ] || { log "the hive edit failed: nothing pushed"; exit 1; }
    for f in sbgl-arm64.dll sbgl-x64.dll sbglicd.dll; do push_check "$stage/$f" "$S32/$f"; done
    # the switch files before the registration: with an allow list no other program ever sees the driver
    adb shell "mkdir -p $G $W/ProgramData/Starbite/umdlog && rm -f $G/only $G/on-* $G/off $G/off-* && echo on > $G/log" >/dev/null
    if [ ${#exes[@]} -gt 0 ]; then
        adb shell "echo on > $G/only" >/dev/null
        for e in "${exes[@]}"; do adb shell "echo on > $G/on-$e" >/dev/null; done
        log "allow list: only ${exes[*]} get the driver"
    else
        log "no allow list: EVERY OpenGL program gets the driver"
    fi
    push_check "$tmp/S1" "$W/$SYS"
    files
    log "gl install done: the adapter's OpenGL driver is sbglicd.dll" ;;
gl-clear)
    mount_rw
    hive_pull
    python3 tools/win/hive-opengl-icd.py "$tmp/SYSTEM" "$tmp/S1" clear 2>&1 | tee -a "$out/sbextra-install.log"
    [ -f "$tmp/S1" ] || { log "the hive edit failed: nothing pushed"; exit 1; }
    push_check "$tmp/S1" "$W/$SYS"
    adb shell "rm -f $G/only $G/on-* $G/off $G/off-* $G/log" >/dev/null
    files
    log "gl clear done: every program is on Microsoft's OpenGL again (the three DLLs stay in System32, unused)" ;;
esac
exit 0
