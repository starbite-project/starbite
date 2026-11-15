#!/usr/bin/env bash
# flip-read.sh OUT: the T70 (FLIP1) result in one screen (docs/WIN11-GPU.md 6.95). OUT = the gpu-cycle result folder.
# Per run: DWM survived or restarted, sbcomp's frame rate; then, per DWM process log, the device-state changes, the
# allocation list of the Render that caused each, the flag sets read, and failed locks/presents.
set -euo pipefail
out=${1:?usage: flip-read.sh OUT}
g=$out/pre/gputest/gputest.log
echo "== runs"; grep -a -E "^ELEV|^FLIP1|sbcomp (hwnd|hwndblt):.*(frames|device)|taskkill|SUCCESS|ERROR" "$g" | tr -d '\r' | cut -c1-200
echo "== DWM logs"
for f in "$out"/pre/umdlog/dwm.exe-*.log; do
  [ -f "$f" ] || continue
  n=$(grep -a -c -E "execution state|that Render listed|flags at|FAILED|failed" "$f" || true)
  echo "-- $(basename "$f"): $(wc -l < "$f") lines, $n of interest"
  grep -a -n -E "execution state|that Render listed|flags at|ResolveSharedResource|FAILED|failed|OpenResource: allocation .* 1200x700|CreateDevice interface|device [0-9]+\)$" "$f" | tr -d '\r' | cut -c1-330 | head -80
done
echo "== sbcomp logs"; grep -a -h -E "execution state|that Render listed|flags at|ResolveSharedResource|FAILED|failed|pfnPresentCb" "$out"/pre/umdlog/sbcomp.exe-*.log 2>/dev/null | tr -d '\r' | sort | uniq -c | sort -rn | cut -c1-260 | head -30
echo "== shots"; ls "$out"/pre/umdlog/ 2>/dev/null | grep -i -E "flip-|shot" | tr '\n' ' '; echo
echo "== crash events"; grep -a -E "Faulting application name|Exception code|Faulting module name" "$out"/pre/umdlog/flip-app-events.txt 2>/dev/null | tr -d '\r' | head -24
