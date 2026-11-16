#!/usr/bin/env bash
# b78-stage.sh: the stages of the T78 follow-up (docs/WIN11-GPU.md 6.112; build/win/patchwork/b78-report.md).
# All carry the diagnostic kernel driver (build/win/kmd-b78/sbwddm.sys: at a GPU job timeout it records what the
# hung job points at and how the GPU's page table has it; no other behaviour change) with T61's sbwdog/sbkeys:
#   sbwddm-t87s   boot check (sb-smoke.bat) of the diagnostic KMD + the diagnostic sbumd b78 (0167 + logging)
#   sbwddm-t87    sb-apps3.bat on the same pair (also used for the noidvs run: the flag file is set from TWRP)
#   sbwddm-t87c   sb-apps3.bat with the user's T61 sbumd: the control (does the older driver hang too?)
#   sbwddm-t88    sb-apps3.bat with the new-tree sbumd (sb-nir, build/win/umd-nir-boot1); run with the flag file
#                 nirshaders: DWM's shaders then go through the new translator
# Nothing touches the phone.
set -euo pipefail
cd "$(dirname "$0")/../.."
here=$(pwd)
main=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
B=$main/build/win; S=$B/stage; T=$S/sbwddm-t61
K=$here/build/win/kmd-b78; U=$here/build/win/umd-b78; N=$here/build/win/umd-nir-boot1
sums() { (cd "$1" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs shasum -a 256 > SHA256SUMS); }
cl() { cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"; }
[ "$(shasum -a 256 "$T/sbumd.dll" | cut -c1-8)" = 6d759c44 ] || { echo "sbwddm-t61 is not T61"; exit 1; }
[ "$(shasum -a 256 "$N/sbumd.dll" | cut -c1-8)" = 63cb8404 ] || { echo "$N/sbumd.dll is not 63cb8404"; exit 1; }
for f in "$K/sbwddm.sys" "$K/sbwddm.map" "$U/sbumd.dll" "$U/sbumd.map"; do [ -f "$f" ] || { echo "no $f"; exit 1; }; done
mk() {  # $1 stage name, $2 sbumd.dll, $3 its map, $4 test script
    local d=$S/$1
    mkdir -p "$d/gputest" "$d/maps"
    for f in sbkeys.sys sbwdog.sys sbwddm.inf; do cl "$T/$f" "$d/$f"; done
    cp "$T/maps/sbkeys.map" "$T/maps/sbwdog.map" "$d/maps/"
    cl "$K/sbwddm.sys" "$d/sbwddm.sys"; cp "$K/sbwddm.map" "$d/maps/sbwddm.map"
    cl "$2" "$d/sbumd.dll"; [ -f "$3" ] && cl "$3" "$d/maps/sbumd.map"
    cp starbite/win/autotest/sb-gputest.cmd "$d/gputest/"
    cp "starbite/win/autotest/$4" "$d/gputest/sb-user.bat"
    if [ "$4" = sb-smoke.bat ]; then
        cp "$B/tests/sbcomp.exe" "$B/tests/sbshow.exe" "$B/winetests/d3d10core_test.exe" "$B/winetests/sbwt.exe" "$d/gputest/"
    else
        cp starbite/win/autotest/sb-web.html "$B/tests/sbshow.exe" "$B/tests/sbsmooth.exe" "$d/gputest/"
    fi
    sums "$d"
    echo "$d: sbwddm $(shasum -a 256 "$d/sbwddm.sys" | cut -c1-8), sbumd $(shasum -a 256 "$d/sbumd.dll" | cut -c1-8), $(wc -l < "$d/SHA256SUMS" | tr -d ' ') files"
}
mk sbwddm-t87s "$U/sbumd.dll" "$U/sbumd.map" sb-smoke.bat
mk sbwddm-t87  "$U/sbumd.dll" "$U/sbumd.map" sb-apps3.bat
mk sbwddm-t87c "$T/sbumd.dll" "$T/maps/sbumd.map" sb-apps3.bat
mk sbwddm-t88  "$N/sbumd.dll" "$N/sbumd.map" sb-apps3.bat
