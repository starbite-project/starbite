#!/usr/bin/env bash
# ng-stage.sh: assemble the two stages of the "Windows 11 (new GPU)" row (docs/WIN11-GPU.md 6.108):
#   build/win/stage/sbwddm-ng-0169   what the row needs on the Windows volume: sbsel.dll (the selector) and
#                                    sbumd-ng.dll (= sbumd 0167, d39b83ab), maps, the catalog row, SHA256SUMS
#   build/win/stage/sbwddm-t83       the two-row boot test: T61's drivers (the standard rows' set, unchanged) +
#                                    gputest: sb-rows.bat as sb-user.bat, sbcomp, sbshow, sb-gd.bat, GL kit 0169
# Nothing touches the phone. Sources are checked against the sha256 they were measured or built with.
set -euo pipefail
cd "$(dirname "$0")/../.."
main=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
B=$main/build/win; T=$B/stage/sbwddm-t61
NG=$B/stage/sbwddm-ng-0169; R=$B/stage/sbwddm-t83
want() { [ "$(shasum -a 256 "$1" | cut -c1-8)" = "$2" ] || { echo "$1 is not $2"; exit 1; }; }
want "$B/umd-0167/sbumd.dll" d39b83ab
want "$T/sbumd.dll" 6d759c44
[ -f "$B/sbsel-0169/sbsel.dll" ] || { echo "build the selector first: starbite/win/sbsel/build.sh $B/sbsel-0169"; exit 1; }
for f in opengl32-gd.dll libgallium_wgl-gd.dll; do [ -f "$B/gd-kit-ec-0169/$f" ] || { echo "no GL kit 0169"; exit 1; }; done

mkdir -p "$NG/maps"
cp "$B/sbsel-0169/sbsel.dll" "$NG/sbsel.dll"; cp "$B/sbsel-0169/sbsel.map" "$NG/maps/sbsel.map"
cp "$B/umd-0167/sbumd.dll" "$NG/sbumd-ng.dll"
m=$(ls "$B"/umd-0167/*.map 2>/dev/null | head -1); [ -n "$m" ] && cp "$m" "$NG/maps/sbumd-ng.map"
cp starbite/win/sbsel/catalog-row.toml "$NG/catalog-row.toml"
cp starbite/win/sbsel/INSTALL.md "$NG/INSTALL.md"
(cd "$NG" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs shasum -a 256 > SHA256SUMS)

mkdir -p "$R/gputest" "$R/maps"
cp "$T/sbkeys.sys" "$T/sbwddm.sys" "$T/sbwdog.sys" "$T/sbwddm.inf" "$T/sbumd.dll" "$R/"
cp "$T"/maps/*.map "$R/maps/"
cp starbite/win/autotest/sb-gputest.cmd starbite/win/autotest/sb-gd.bat starbite/win/autotest/sb-click.ps1 "$R/gputest/"
cp starbite/win/autotest/sb-rows.bat "$R/gputest/sb-user.bat"
cp "$B/stage/sbwddm-t70/gputest/sbcomp.exe" "$B/stage/sbwddm-t72/gputest/sbshow.exe" "$R/gputest/"
cp "$B/gd-kit-ec-0169/opengl32-gd.dll" "$B/gd-kit-ec-0169/libgallium_wgl-gd.dll" "$R/gputest/"
cp starbite/win/sbsel/T83.md "$R/INSTALL.md"
(cd "$R" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs shasum -a 256 > SHA256SUMS)
echo "$NG: sbsel $(shasum -a 256 "$NG/sbsel.dll" | cut -c1-8), sbumd-ng $(shasum -a 256 "$NG/sbumd-ng.dll" | cut -c1-8)"
echo "$R: sbumd $(shasum -a 256 "$R/sbumd.dll" | cut -c1-8), $(wc -l < "$R/SHA256SUMS" | tr -d ' ') files"
