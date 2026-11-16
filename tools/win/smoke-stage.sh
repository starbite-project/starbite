#!/usr/bin/env bash
# smoke-stage.sh NAME SBUMD_DLL [SBUMD_MAP]: assemble build/win/stage/sbwddm-NAME for the five-minute boot check of
# a driver build (docs/WIN11-GPU.md 6.105): T61's kernel drivers + the given sbumd.dll + sb-smoke.bat (logon reached?
# DWM up? sbcomp blt and flip windows on our adapter, the first 12 d3d10core tests, KMD counters), with INSTALL.md
# and SHA256SUMS. Nothing touches the phone. Run it with:
#   tools/win/gpu-cycle.sh STAGE OUT --test-key win11-big --full-sec 600 --no-trace --keys 1 --afbc 0
set -euo pipefail
cd "$(dirname "$0")/../.."
name=${1:?usage: smoke-stage.sh NAME SBUMD_DLL [SBUMD_MAP]}; dll=${2:?}; map=${3:-}
B=/Users/dev/project-starbite/build/win; S=$B/stage/sbwddm-$name; T=$B/stage/sbwddm-t61
[ -f "$dll" ] || { echo "no $dll"; exit 1; }
mkdir -p "$S/gputest" "$S/maps"
cp "$T/sbkeys.sys" "$T/sbwddm.sys" "$T/sbwdog.sys" "$T/sbwddm.inf" "$S/"
cp "$T/maps/sbkeys.map" "$T/maps/sbwddm.map" "$T/maps/sbwdog.map" "$S/maps/"
cp "$dll" "$S/sbumd.dll"; [ -n "$map" ] && cp "$map" "$S/maps/sbumd.map"
cp starbite/win/autotest/sb-gputest.cmd "$S/gputest/"; cp starbite/win/autotest/sb-smoke.bat "$S/gputest/sb-user.bat"
cp "$B/tests/sbcomp.exe" "$B/tests/sbshow.exe" "$S/gputest/"
cp "$B/winetests/d3d10core_test.exe" "$B/winetests/sbwt.exe" "$S/gputest/"
h=$(shasum -a 256 "$S/sbumd.dll" | cut -c1-8)
cat > "$S/INSTALL.md" <<EOT
# $name (SMOKE): does Windows start with sbumd $h? (~8 min)

The boot check before any long test on this driver build (docs/WIN11-GPU.md 6.105). Drivers = T61's kernel
drivers + sbumd $h. gputest: sb-user.bat (= starbite/win/autotest/sb-smoke.bat): logon reached, DWM's pid, sbcomp
hwndblt and hwnd (6 s each, a shot each), the first 12 d3d10core tests through sbwt, DWM survived or not, the KMD's
timeout / reset / fault / hang counters, the sbwddm State key, crash events; restart.

## Run (phone in TWRP; big cores, unattended)
\`\`\`
S=/Users/dev/project-starbite/build/win/stage; R=/Users/dev/project-starbite/build/win/results-$name
tools/win/gpu-cycle.sh \$S/sbwddm-$name \$R/ours --test-key win11-big --full-sec 600 --no-trace --keys 1 --afbc 0
tools/win/crash-report.sh \$R/ours ; tools/win/user-pull.sh \$R/ours/user
\`\`\`
then the usual restore. PASS = gputest.log has "SMOKE logon reached", "DWM SURVIVED", sbcomp lines naming
"Starbite Display", and an "SBWT d3d10core SUMMARY" line without a WARNING about another adapter.
No gputest.log = the boot never reached the logon with this driver.
EOT
(cd "$S" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs shasum -a 256 > SHA256SUMS)
echo "$S: sbumd $h, $(wc -l < "$S/SHA256SUMS" | tr -d ' ') files"
