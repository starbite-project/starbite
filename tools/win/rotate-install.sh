#!/usr/bin/env bash
# rotate-install.sh: put sbrotate (screen orientation helper, docs/WIN11-GPU.md 6.57) into the user's Windows from TWRP:
# C:\starbite\rotate\sbrotate.exe and a Startup entry that runs "sbrotate.exe daemon" at logon (first logon: landscape;
# then Ctrl+Alt+arrows rotate). Needs the full driver with rotation (sbwddm with Parameters\Rotation = 1, T51 on).
#   tools/win/rotate-install.sh SBROTATE.EXE [--landscape-270] | --remove
# Phone in TWRP, no other phone job. The user's account is "starbite" (the same Startup folder gpu-cycle uses).
set -uo pipefail
W=/tmp/wwin; NTFS=/dev/block/sda27
SU="$W/Users/starbite/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Startup"
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "phone not in TWRP"; exit 1; }
adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
adb shell "grep -q ' $W ' /proc/mounts" || { echo "C: did not mount"; exit 2; }
trap 'adb shell "sync; umount $W" >/dev/null 2>&1' EXIT
if [ "${1:-}" = --remove ]; then
    adb shell "rm -f '$SU/sbrotate-daemon.cmd'; rm -rf $W/starbite/rotate"
    echo "removed"; exit 0
fi
exe=${1:?usage: rotate-install.sh SBROTATE.EXE [--landscape-270] | --remove}
adb shell "mkdir -p $W/starbite/rotate" >/dev/null
adb push "$exe" "$W/starbite/rotate/sbrotate.exe" >/dev/null
a=$(shasum -a 256 "$exe" | cut -c1-64); b=$(adb shell "sha256sum $W/starbite/rotate/sbrotate.exe" | tr -d '\r' | cut -c1-64)
[ "$a" = "$b" ] || { echo "READ-BACK MISMATCH"; exit 3; }
[ "${2:-}" = --landscape-270 ] && adb shell "echo on > $W/starbite/rotate/landscape-270"
printf '@echo off\r\nstart "" C:\\starbite\\rotate\\sbrotate.exe daemon\r\n' > /tmp/sbrotate-daemon.cmd
adb push /tmp/sbrotate-daemon.cmd "$SU/sbrotate-daemon.cmd" >/dev/null && rm -f /tmp/sbrotate-daemon.cmd
echo "installed sbrotate (${a:0:8}) + Startup entry; first logon switches to landscape"
