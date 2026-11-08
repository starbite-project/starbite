#!/bin/sh
# 1.0-rc3: Settings > Alarm > "LED at rated power" on the host's S2MPB02 model: the chip answering (the bring-up logged,
# on), not acknowledging (refused with that reason, the switch stays off), the controller never running (refused as
# such), and a second switch-on tries the chip again (rc2 kept a refusal until a restart). Run after sbos-preview.sh.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/starbite/out/sboshost"
run() { limactl shell --workdir "$root" starbite env SBOS_HOST_DUMPLOG=1 ${FLED:+SBOS_HOST_FLED=$FLED} "$out/sbos-host" "$out" /dev/null "$@" q 2>&1 | grep -E 'settings: the LED|fled:|host: S2MPB02'; }
ALARM="w:300 g:1150,2310,0,700 w:400 t:382,2424 w:800 t:552,671 w:600"
LED="t:1184,1407 w:600"
fails=0
want() { if echo "$1" | grep -q -- "$2"; then echo "ok: $2"; else echo "FAIL: $2"; echo "$1" | sed 's/^/    /'; fails=$((fails + 1)); fi; }
never() { if echo "$1" | grep -q -- "$2"; then echo "FAIL: unexpected: $2"; echo "$1" | sed 's/^/    /'; fails=$((fails + 1)); else echo "ok: no $2"; fi; }
FLED=
a=$(run $ALARM $LED)
want "$a" "fled: CMU_PERIC1 gates USI10"
want "$a" "fled: before: usi mode"
want "$a" "fled: USI10 after the bring-up"
want "$a" "S2MPB02 0x1D 0x90"
want "$a" "settings: the LED at its rated currents (S2MPB02 over I2C): on"
never "$a" "refused"
never "$a" "FORBIDDEN"
FLED=noack
b=$(run $ALARM $LED $LED $LED)
want "$b" "fled: the first read failed: int_stat seen 0x00000401"
want "$b" "fled: refused: the bus runs but no chip acknowledged 0x59"
n=$(echo "$b" | grep -c "fled: before: usi mode")
if [ "$n" -ge 2 ]; then echo "ok: tried again at the second switch-on ($n bring-ups)"; else echo "FAIL: one bring-up only ($n)"; fails=$((fails + 1)); fi
never "$b" "host: S2MPB02 CUR1"
FLED=noclock
c=$(run $ALARM $LED)
want "$c" "fled: refused: the I2C controller never ran (its clock?)"
echo "led test: $fails failed"
