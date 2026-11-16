#!/usr/bin/env bash
# gd-kit-push.sh DIR: give the user's Geometry Dash our x64 OpenGL driver, from TWRP (docs/WIN11-GPU.md 6.69).
# DIR holds opengl32-gd.dll and libgallium_wgl-gd.dll (e.g. a stage's gputest/). They go into the game folder as
# opengl32.dll / libgallium_wgl.dll with the marker opengl32.dll.starbite (what gd-gl-on.cmd writes), so the
# kit's gd-gl-off.cmd and the gputest's sb-gd.bat know they are ours. Refuses a foreign opengl32.dll (no marker).
# Every file is read back by sha256. No registry change, no boot; the phone stays in TWRP.
#   tools/win/gd-kit-push.sh DIR            install / replace
#   tools/win/gd-kit-push.sh --show         what is in the game folder now (read-only)
set -euo pipefail
NTFS=/dev/block/sda27
W=/tmp/wwin
G="$W/Program Files (x86)/Steam/steamapps/common/Geometry Dash"
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "the phone is not in TWRP"; exit 1; }
show() {
    adb shell "ls -la \"$G\"/opengl32.dll* \"$G\"/libgallium_wgl.dll 2>&1; sha256sum \"$G\"/opengl32.dll \"$G\"/libgallium_wgl.dll 2>/dev/null" | tr -d '\r'
}
if [ "${1:-}" = --show ]; then
    adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o ro $NTFS $W" >/dev/null
    show; adb shell "umount $W" >/dev/null 2>&1 || true; exit 0
fi
D=${1:?usage: gd-kit-push.sh DIR [--flag NAME | --no-flag NAME]... | --show}
# T71 (docs/WIN11-GPU.md 6.100): --flag NAME creates C:\ProgramData\Starbite\NAME (e.g. glzcflipd: the game presents
# zero-copy on a FLIP_DISCARD swap chain; needs an sbumd with 0167), --no-flag NAME removes it. Several may be given.
shift
flags_on=(); flags_off=()
while [ $# -gt 0 ]; do case $1 in --flag) flags_on+=("$2"); shift 2;; --no-flag) flags_off+=("$2"); shift 2;; *) echo "unknown argument $1"; exit 1;; esac; done
for f in "${flags_on[@]:-}" "${flags_off[@]:-}"; do case $f in ''|[a-z0-9]*) ;; *) echo "bad flag name $f"; exit 1;; esac; case $f in */*|*..*) echo "bad flag name $f"; exit 1;; esac; done
for f in opengl32-gd.dll libgallium_wgl-gd.dll; do [ -f "$D/$f" ] || { echo "no $D/$f"; exit 1; }; done
adb shell "mkdir -p $W; umount $W 2>/dev/null; mount.ntfs -o rw,inherit $NTFS $W" >/dev/null
trap 'adb shell "sync; umount '"$W"'" >/dev/null 2>&1 || true' EXIT
adb shell "[ -f \"$G/GeometryDash.exe\" ]" || { echo "Geometry Dash is not installed in the usual Steam folder"; exit 1; }
if adb shell "[ -f \"$G/opengl32.dll\" ] && [ ! -f \"$G/opengl32.dll.starbite\" ]"; then
    echo "a foreign opengl32.dll is in the game folder (no starbite marker): not touching it"; exit 1
fi
echo "before:"; show
push() {
    adb push "$1" "$2" >/dev/null
    a=$(shasum -a 256 "$1" | cut -c1-64); b=$(adb shell "sha256sum \"$2\"" | tr -d '\r' | cut -c1-64)
    [ "$a" = "$b" ] || { echo "READ-BACK MISMATCH: $2"; exit 4; }
    echo "pushed $(basename "$1") -> $(basename "$2") (${a:0:8})"
}
push "$D/opengl32-gd.dll" "$G/opengl32.dll"
push "$D/libgallium_wgl-gd.dll" "$G/libgallium_wgl.dll"
adb shell "echo starbite > \"$G/opengl32.dll.starbite\""
P="$W/ProgramData/Starbite"
for f in "${flags_on[@]:-}"; do [ -n "$f" ] || continue; adb shell "mkdir -p '$P' && echo on > '$P/$f'"; echo "flag on: C:\\ProgramData\\Starbite\\$f"; done
for f in "${flags_off[@]:-}"; do [ -n "$f" ] || continue; adb shell "rm -f '$P/$f'"; echo "flag off: C:\\ProgramData\\Starbite\\$f"; done
adb shell "ls '$P' 2>/dev/null | grep -E '^gl' | tr '\n' ' '" | tr -d '\r' | sed 's/^/gl flag files now: /'; echo
echo "done; phone in TWRP"
