#!/usr/bin/env bash
# sbflag.sh: Starbite driver flag files on the Windows volume, from TWRP. A flag file is an empty-ish file
# C:\ProgramData\Starbite\NAME that sbumd / the GL driver look for; some must exist before the desktop compositor
# starts, so they are set here and not by a test script.
#   tools/win/sbflag.sh show             the flag files there now (read-only)
#   tools/win/sbflag.sh on NAME [NAME..] create
#   tools/win/sbflag.sh off NAME [NAME..] remove
# Names: letters, digits, '-' and '_' only. No registry change, no boot; the phone stays in TWRP.
set -euo pipefail
NTFS=/dev/block/sda27; W=/tmp/wwin; P=$W/ProgramData/Starbite
mode=${1:-}; [ -n "$mode" ] || { sed -n '2,9p' "$0"; exit 1; }
shift || true
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "the phone is not in TWRP"; exit 1; }
trap 'adb shell "sync; umount '"$W"'" >/dev/null 2>&1 || true' EXIT
list() { echo "flag files now: $(adb shell "ls -p $P 2>/dev/null | grep -v / || true" | tr -d '\r' | tr '\n' ' ')"; }
case $mode in
show)
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null; list ;;
on|off)
    [ $# -gt 0 ] || { echo "no flag name"; exit 1; }
    for f in "$@"; do case $f in ''|*[!A-Za-z0-9_-]*) echo "bad flag name $f"; exit 1 ;; esac; done
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
    adb shell "mkdir -p $P" >/dev/null
    for f in "$@"; do
        if [ "$mode" = on ]; then
            adb shell "echo on > $P/$f && [ -f $P/$f ]" >/dev/null || { echo "could not create $f (is the volume read-only?)"; exit 4; }
            echo "flag on: C:\\ProgramData\\Starbite\\$f"
        else
            adb shell "rm -f $P/$f; [ ! -e $P/$f ]" >/dev/null || { echo "could not remove $f"; exit 4; }
            echo "flag off: C:\\ProgramData\\Starbite\\$f"
        fi
    done
    list ;;
*) sed -n '2,9p' "$0"; exit 1 ;;
esac
