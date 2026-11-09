#!/bin/sh
# Starbite OS 1.0-rc2: the heater's stop order with a save slot (the hardware off before the record's save) and the
# reserve override (one session only). Run on the Mac after sbos-preview.sh built the host binary.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
OPEN="w:300 g:1150,2310,0,700 w:400"
HEAT="t:1057,663 w:600"
EXTREME="t:1216,670 w:500 g:720,1170,0,1900"
fails=0
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $3"; else echo "FAIL: $3 ($2)"; fails=$((fails + 1)); fi; }
order() { a=$(echo "$1" | grep -n -- "$2" | head -1 | cut -d: -f1); b=$(echo "$1" | grep -n -- "$3" | tail -1 | cut -d: -f1)
          if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then echo "ok: $4"; else echo "FAIL: $4 ($2 before $3: $a $b)"; fails=$((fails + 1)); fi; }
rm -f "$out/heat-test.bin"
r=$(limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 SBOS_HOST_STATE="$out/heat-test.bin" \
    SBOS_HOST_FILES="$root/starbite/sbos/examples" "$out/sbos-host" "$out" /dev/null $OPEN $HEAT $EXTREME w:8000 \
    t:220,670 w:6000 q 2>&1)
want "$r" "heater: start, level 0x00000004" "Extreme started"
want "$r" "heater: stopped: Stopped" "Off stopped it"
order "$r" "heater: the panel level, the white screen and the LED put back" "settings: saved" "the hardware off before the stop record's save"
order "$r" "heater: stopped: Stopped" "settings: saved" "the stop logged before the save"
echo "heat test: $fails failed"
[ "$fails" = 0 ]
