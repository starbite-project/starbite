#!/usr/bin/env bash
# extra-stage.sh: assemble the stages for the 32-bit driver (docs/WIN11-GPU-X86.md) and the system-wide OpenGL driver
# (docs/WIN11-GPU-GLSYS.md), from the builds in this worktree's build/win/umd-x86 and build/win/gl-sys:
#   build/win/stage/sbwddm-x86-0172    sbumd32.dll (-> SysWOW64\sbumd.dll), map, SHA256SUMS
#   build/win/stage/sbwddm-t85         the 32-bit test: the 0167 driver set (the x86 build is 0167, so DWM runs the
#                                      same generation) + sb-x86.bat as sb-user.bat, sbd3d32, sbwt32, d3d10core_test32
#   build/win/stage/sbwddm-glsys-0171  sbglicd.dll, sbgl-arm64.dll, sbgl-x64.dll, maps, SHA256SUMS
#   build/win/stage/sbwddm-t86         the OpenGL test: T61's driver set + sb-glsys.bat as sb-user.bat, sbgltest x3
# Nothing touches the phone. Big files are APFS clones (cp -c): no extra disk space.
set -euo pipefail
cd "$(dirname "$0")/../.."
here=$(pwd)
main=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
B=$main/build/win; S=$B/stage; X=$here/build/win/umd-x86; G=$here/build/win/gl-sys
sums() { (cd "$1" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs shasum -a 256 > SHA256SUMS); }
cl() { cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"; }
drivers() {  # $1 = source stage, $2 = destination
    mkdir -p "$2/gputest" "$2/maps"
    for f in sbkeys.sys sbwddm.sys sbwdog.sys sbwddm.inf sbumd.dll; do cl "$1/$f" "$2/$f"; done
    cp "$1"/maps/*.map "$2/maps/"
    cp starbite/win/autotest/sb-gputest.cmd "$2/gputest/"
}
[ "$(shasum -a 256 "$S/sbwddm-user-0167/sbumd.dll" | cut -c1-8)" = d39b83ab ] || { echo "sbwddm-user-0167 is not the 0167 set"; exit 1; }
[ "$(shasum -a 256 "$S/sbwddm-t61/sbumd.dll" | cut -c1-8)" = 6d759c44 ] || { echo "sbwddm-t61 is not T61"; exit 1; }
(cd "$X" && shasum -a 256 -c SHA256SUMS >/dev/null) || { echo "$X does not match its SHA256SUMS"; exit 1; }

d=$S/sbwddm-x86-0172; mkdir -p "$d/maps"
cl "$X/sbumd.dll" "$d/sbumd32.dll"; cl "$X/sbumd.map" "$d/maps/sbumd32.map"; sums "$d"
d=$S/sbwddm-t85; drivers "$S/sbwddm-user-0167" "$d"
cp starbite/win/autotest/sb-x86.bat "$d/gputest/sb-user.bat"
for f in sbd3d32.exe sbwt32.exe d3d10core_test32.exe; do cp "$X/$f" "$d/gputest/"; done
sums "$d"

d=$S/sbwddm-glsys-0171; mkdir -p "$d/maps"
for f in sbglicd.dll sbgl-arm64.dll sbgl-x64.dll; do cl "$G/$f" "$d/$f"; done
for f in sbglicd.map sbgl-arm64.map sbgl-x64.map; do cl "$G/$f" "$d/maps/$f"; done
sums "$d"
d=$S/sbwddm-t86; drivers "$S/sbwddm-t61" "$d"
cp starbite/win/autotest/sb-glsys.bat "$d/gputest/sb-user.bat"
for f in sbgltest-arm64.exe sbgltest-arm64ec.exe sbgltest-x64.exe; do cp "$G/$f" "$d/gputest/"; done
sums "$d"
for d in sbwddm-x86-0172 sbwddm-t85 sbwddm-glsys-0171 sbwddm-t86; do echo "$S/$d: $(wc -l < "$S/$d/SHA256SUMS" | tr -d ' ') files"; done
shasum -a 256 "$S/sbwddm-x86-0172/sbumd32.dll" "$S"/sbwddm-glsys-0171/*.dll | cut -c1-8,65- | sed "s|$S/||"
